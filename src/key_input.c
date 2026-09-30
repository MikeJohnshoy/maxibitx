// key_input.c
//
// The key jack, read by edge events rather than by polling. Both contacts
// are one GPIO v2 line request, and the kernel stamps every edge with
// CLOCK_MONOTONIC in its interrupt handler. A thread of its own sleeps in
// ppoll() on that fd - no CPU while the key is idle - and for each edge:
// debounces it; drops it if the contact is excluded as a ring grounded by a
// mono plug; queues {time, contact, paddle, closed} for the audio thread in
// a single-producer, single-consumer ring neither side blocks on; and on a
// closure calls the on_closed hook, which is how cw.c requests TX up to a
// block early. docs/dsp_design_notes/cw_keyer_design_study.md §16.

#define _GNU_SOURCE // ppoll()

#include "key_input.h"
#include "gpio.h"
#include "radio_hw.h" // KEY_TIP_GPIO, KEY_RING_GPIO
#include <errno.h>
#include <linux/gpio.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DETECT_WINDOW_MS 250
#define QUEUE_LEN 256         // edges queued for the audio thread; a power of two
#define KERNEL_EVENT_BUFFER 256

static const unsigned contact_gpio[KEY_CONTACTS] = {KEY_TIP_GPIO, KEY_RING_GPIO};
static const char *const contact_name[KEY_CONTACTS] = {"tip", "ring"};

/* ---- Input-thread state ------------------------------------------------- */

struct contact {
  int raw;            // level of the latest edge seen, bounce included (1 = closed)
  int closed;         // debounced state
  int check_due;      // a debounce lockout is running
  int64_t lock_until; // end of that lockout
  int excluded;       // treated as a grounded ring: its edges are not passed on
  uint32_t line_seqno; // kernel's per-line sequence number of the last edge
};

static struct contact contacts[KEY_CONTACTS];
static int64_t debounce_ns = 0;
static key_closed_fn closed_hook = NULL;

enum detect_state {
  DETECT_IDLE, // no detection in progress
  DETECT_ONE,  // one contact closed since arming, excluded until the window ends
  DETECT_BOTH, // both closed since arming, both excluded until one opens
};
static enum detect_state detect = DETECT_IDLE;
static int candidate = KEY_RING;    // the contact DETECT_ONE is timing
static int64_t detect_deadline = 0; // when DETECT_ONE confirms it

static _Atomic int reverse = 0;

/* ---- The queue to the audio thread -------------------------------------- */

struct queued {
  int64_t ts;
  uint8_t contact, paddle, closed;
};
static struct queued queue[QUEUE_LEN];
static _Atomic unsigned q_head = 0; // written by the input thread only
static _Atomic unsigned q_tail = 0; // written by the audio thread only
static _Atomic int q_overflowed = 0;

// Each contact's state as last queued (bit per contact, 1 = closed and not
// excluded). The audio thread resyncs to it after an overflow.
static _Atomic unsigned live = 0;

// The audio thread's view: each contact's state after the last edge it took.
static unsigned taken = 0;

static int paddle_of(int c) {
  return ((c == KEY_TIP) != (atomic_load(&reverse) != 0)) ? KEY_DOT : KEY_DASH;
}

static void queue_edge(int c, int closed, int64_t ts) {
  unsigned bit = 1u << c;
  unsigned m = atomic_load(&live);
  atomic_store(&live, closed ? (m | bit) : (m & ~bit));

  unsigned h = atomic_load_explicit(&q_head, memory_order_relaxed);
  unsigned t = atomic_load_explicit(&q_tail, memory_order_acquire);
  if (h - t >= QUEUE_LEN) {
    // Only possible if the audio thread has stopped taking edges.
    if (!atomic_exchange(&q_overflowed, 1))
      printf("key: edge queue full - dropping edges; the audio thread will resync to the "
             "key's state\n");
  } else {
    queue[h % QUEUE_LEN] = (struct queued){ts, (uint8_t)c, (uint8_t)paddle_of(c),
                                           (uint8_t)(closed != 0)};
    atomic_store_explicit(&q_head, h + 1, memory_order_release);
  }

  if (closed && closed_hook)
    closed_hook();
}

/* ---- Mono-plug detection ------------------------------------------------ */
//
// A mono plug in the stereo jack grounds the ring for as long as it is in.
// When armed, a contact found closed is excluded straight away, so a grounded
// ring can never key the radio; the exclusion is confirmed if it stays closed
// for DETECT_WINDOW_MS, or dropped if it opens sooner (a key held down during
// start-up). Both closed means the plug and a held key, or a squeezed paddle:
// both are excluded until one opens, and the other is then timed as above. A
// confirmed exclusion ends when that contact opens, which means a new plug.

static void detect_confirm_if_due(int64_t t) {
  if (detect == DETECT_ONE && t >= detect_deadline) {
    detect = DETECT_IDLE;
    printf("key: %s (BCM %u) closed since start-up - treating as a mono straight-key plug; "
           "that contact is ignored\n",
           contact_name[candidate], contact_gpio[candidate]);
  }
}

// A contact's debounced state has changed.
static void detect_on_change(int c, int closed, int64_t ts) {
  struct contact *k = &contacts[c];
  switch (detect) {
  case DETECT_ONE:
    if (c == candidate && !closed) {
      k->excluded = 0;
      detect = DETECT_IDLE;
      printf("key: %s (BCM %u) opened within %d ms - a key held down, not a mono plug; both "
             "contacts in use\n",
             contact_name[c], contact_gpio[c], DETECT_WINDOW_MS);
    }
    break;
  case DETECT_BOTH:
    if (!closed) {
      k->excluded = 0;
      candidate = 1 - c;
      detect = DETECT_ONE;
      detect_deadline = ts + DETECT_WINDOW_MS * 1000000LL;
    }
    break;
  case DETECT_IDLE:
    if (k->excluded && !closed) {
      k->excluded = 0;
      printf("key: %s (BCM %u) opened - plug changed, both contacts in use\n", contact_name[c],
             contact_gpio[c]);
    }
    break;
  }
}

/* ---- Debounce ----------------------------------------------------------- */
//
// Leading-edge: the first edge that changes a contact's state is taken at
// its own timestamp, then that contact's edges are ignored for debounce_ms;
// at the end of that lockout its latest level is compared with the state
// taken, and a difference is taken at the lockout's end. No added latency on
// either edge, at the cost that no mark or space is shorter than debounce_ms.
// The kernel's own debounce isn't used: its events carry the time the
// debounce period ended, not the edge's.

static void take_change(int c, int closed, int64_t ts) {
  struct contact *k = &contacts[c];
  k->closed = closed;
  if (debounce_ns > 0) {
    k->lock_until = ts + debounce_ns;
    k->check_due = 1;
  }
  int was_excluded = k->excluded;
  detect_on_change(c, closed, ts);
  if (!was_excluded)
    queue_edge(c, closed, ts);
}

// Ends contact c's lockout if it ran out by time t, taking the level the
// contact settled at if that differs from the state taken.
static void end_lockout_if_due(int c, int64_t t) {
  struct contact *k = &contacts[c];
  while (k->check_due && t >= k->lock_until) {
    k->check_due = 0;
    if (k->raw != k->closed)
      take_change(c, k->raw, k->lock_until);
  }
}

/* ---- The steps (key_input.h) -------------------------------------------- */

void key_input_arm(unsigned closed_mask, int64_t now_ns, int debounce_ms,
                   key_closed_fn on_closed) {
  debounce_ns = (int64_t)(debounce_ms > 0 ? debounce_ms : 0) * 1000000;
  closed_hook = on_closed;
  unsigned was_live = atomic_load(&live);

  int n_closed = 0;
  for (int c = 0; c < KEY_CONTACTS; c++) {
    struct contact *k = &contacts[c];
    k->raw = k->closed = (closed_mask >> c) & 1;
    k->check_due = 0;
    k->lock_until = 0;
    k->excluded = k->closed; // anything closed now is a ring until shown otherwise
    n_closed += k->closed;
  }
  if (n_closed == 0) {
    detect = DETECT_IDLE;
  } else if (n_closed == 1) {
    detect = DETECT_ONE;
    candidate = contacts[KEY_TIP].closed ? KEY_TIP : KEY_RING;
    detect_deadline = now_ns + DETECT_WINDOW_MS * 1000000LL;
  } else {
    detect = DETECT_BOTH;
    printf("key: tip (BCM %u) and ring (BCM %u) both closed - ignoring both until one "
           "opens\n",
           contact_gpio[KEY_TIP], contact_gpio[KEY_RING]);
  }

  // Arming leaves nothing live (a closed contact is excluded); tell the
  // audio thread about any contact it still has closed from before.
  for (int c = 0; c < KEY_CONTACTS; c++)
    if ((was_live >> c) & 1)
      queue_edge(c, 0, now_ns);
}

void key_input_edge(enum key_contact c, int closed, int64_t ts_ns) {
  detect_confirm_if_due(ts_ns);
  end_lockout_if_due(c, ts_ns);
  struct contact *k = &contacts[c];
  k->raw = closed != 0;
  if (k->check_due)
    return; // bounce, inside the lockout
  if (k->raw != k->closed)
    take_change(c, k->raw, ts_ns);
}

void key_input_tick(int64_t now_ns) {
  for (int c = 0; c < KEY_CONTACTS; c++)
    end_lockout_if_due(c, now_ns);
  detect_confirm_if_due(now_ns);
}

int64_t key_input_next_due(void) {
  int64_t due = 0;
  for (int c = 0; c < KEY_CONTACTS; c++)
    if (contacts[c].check_due && (due == 0 || contacts[c].lock_until < due))
      due = contacts[c].lock_until;
  if (detect == DETECT_ONE && (due == 0 || detect_deadline < due))
    due = detect_deadline;
  return due;
}

unsigned key_input_excluded(void) {
  unsigned m = 0;
  for (int c = 0; c < KEY_CONTACTS; c++)
    if (contacts[c].excluded)
      m |= 1u << c;
  return m;
}

void key_input_set_reverse(int on) { atomic_store(&reverse, on != 0); }

int key_input_get_reverse(void) { return atomic_load(&reverse); }

/* ---- The audio thread's side -------------------------------------------- */

int key_input_take(int64_t block_start_ns, int64_t block_end_ns, int n,
                   struct key_event *ev, int max) {
  int64_t span = block_end_ns - block_start_ns;
  if (span <= 0)
    span = 1;
  int m = 0, last = 0;
  unsigned t = atomic_load_explicit(&q_tail, memory_order_relaxed);
  unsigned h = atomic_load_explicit(&q_head, memory_order_acquire);

  while (t != h && m < max) {
    const struct queued *q = &queue[t % QUEUE_LEN];
    if (q->ts >= block_end_ns)
      break; // belongs to the next interval
    int off = 0;
    if (q->ts > block_start_ns) {
      off = (int)((q->ts - block_start_ns) * n / span);
      if (off > n - 1)
        off = n - 1;
    }
    if (off < last)
      off = last; // an edge queued out of time order goes no earlier than the one before it
    ev[m++] = (struct key_event){off, q->contact, q->paddle, q->closed};
    taken = q->closed ? (taken | (1u << q->contact)) : (taken & ~(1u << q->contact));
    last = off;
    t++;
  }
  atomic_store_explicit(&q_tail, t, memory_order_release);

  // After an overflow, once the queue has drained, bring each contact to its
  // current state at the end of the block.
  if (t == h && m + KEY_CONTACTS <= max && atomic_exchange(&q_overflowed, 0)) {
    unsigned now_live = atomic_load(&live);
    for (int c = 0; c < KEY_CONTACTS; c++) {
      unsigned bit = 1u << c;
      if ((taken ^ now_live) & bit) {
        int closed = (now_live & bit) != 0;
        ev[m++] = (struct key_event){n - 1, (uint8_t)c, (uint8_t)paddle_of(c), (uint8_t)closed};
        taken ^= bit;
      }
    }
  }
  return m;
}

int key_input_pending(void) {
  return atomic_load_explicit(&q_head, memory_order_acquire) !=
         atomic_load_explicit(&q_tail, memory_order_acquire);
}

/* ---- The input thread ----------------------------------------------------- */

static int line_fd = -1;
static pthread_t input_thread;

static int64_t monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void handle_event(const struct gpio_v2_line_event *e) {
  int c = (e->offset == KEY_TIP_GPIO) ? KEY_TIP : KEY_RING;
  struct contact *k = &contacts[c];
  if (k->line_seqno != 0 && e->line_seqno != k->line_seqno + 1)
    printf("key: the kernel dropped %u edge(s) on the %s (BCM %u) - its event queue "
           "overflowed\n",
           e->line_seqno - k->line_seqno - 1, contact_name[c], contact_gpio[c]);
  k->line_seqno = e->line_seqno;
  // Pulled up, closing to ground: a falling edge is the contact closing.
  key_input_edge(c, e->id == GPIO_V2_LINE_EVENT_FALLING_EDGE, (int64_t)e->timestamp_ns);
}

// The thread is stopping on an error: open every contact the audio thread
// has closed, so a key held at that moment can't hold TX with no release
// ever to come.
static void input_loop_fail(const char *what) {
  fprintf(stderr, "key: %s failed (%s) - the key is no longer read\n", what, strerror(errno));
  unsigned m = atomic_load(&live);
  for (int c = 0; c < KEY_CONTACTS; c++)
    if ((m >> c) & 1)
      queue_edge(c, 0, monotonic_ns());
}

static void *input_loop(void *arg) {
  (void)arg;
  struct gpio_v2_line_event evs[16];

  for (;;) {
    struct timespec timeout, *tp = NULL;
    int64_t due = key_input_next_due();
    if (due) {
      int64_t wait = due - monotonic_ns();
      if (wait < 0)
        wait = 0;
      timeout.tv_sec = wait / 1000000000LL;
      timeout.tv_nsec = wait % 1000000000LL;
      tp = &timeout;
    }

    struct pollfd p = {.fd = line_fd, .events = POLLIN};
    int r = ppoll(&p, 1, tp, NULL);
    if (r == 0) {
      // Timed out for a lockout or detection window: first take any edge
      // the kernel queued meanwhile, so the tick below judges the contact
      // by its latest level.
      struct timespec zero = {0, 0};
      r = ppoll(&p, 1, &zero, NULL);
    }
    if (r < 0) {
      if (errno == EINTR)
        continue;
      input_loop_fail("poll");
      return NULL;
    }
    if (r > 0 && !(p.revents & POLLIN)) {
      errno = EIO; // POLLERR/POLLHUP/POLLNVAL: the line request is gone
      input_loop_fail("poll");
      return NULL;
    }
    if (r > 0) {
      ssize_t got = read(line_fd, evs, sizeof(evs));
      if (got < 0 && errno != EINTR && errno != EAGAIN) {
        input_loop_fail("read");
        return NULL;
      }
      for (ssize_t i = 0; got > 0 && i < got / (ssize_t)sizeof(evs[0]); i++)
        handle_event(&evs[i]);
    }
    key_input_tick(monotonic_ns());
  }
  return NULL;
}

int key_input_start(int debounce_ms, key_closed_fn on_closed) {
  line_fd = gpio_request_edge_inputs(contact_gpio, KEY_CONTACTS, KERNEL_EVENT_BUFFER,
                                     "maxibitx-key");
  if (line_fd < 0)
    return -1;

  usleep(1000); // let the pull-ups charge an open contact before reading it
  int levels = gpio_read_lines(line_fd, KEY_CONTACTS);
  if (levels < 0)
    return -1;
  unsigned closed = 0;
  for (int c = 0; c < KEY_CONTACTS; c++)
    if (!((levels >> c) & 1))
      closed |= 1u << c; // low = closed to ground
  key_input_arm(closed, monotonic_ns(), debounce_ms, on_closed);

  // Real-time, one step below the audio thread: its wake-up latency is how
  // soon an early TX request goes out, and it does almost nothing when it
  // runs. Not fatal if refused - timestamps come from the kernel either way.
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
  struct sched_param sch = {.sched_priority = sched_get_priority_max(SCHED_FIFO) - 1};
  pthread_attr_setschedparam(&attr, &sch);
  int rc = pthread_create(&input_thread, &attr, input_loop, NULL);
  pthread_attr_destroy(&attr);
  if (rc != 0)
    rc = pthread_create(&input_thread, NULL, input_loop, NULL);
  if (rc != 0) {
    fprintf(stderr, "key: cannot start the input thread: %s\n", strerror(rc));
    return -1;
  }
  return 0;
}
