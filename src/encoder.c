// encoder.c
//
// The front-panel knobs, read by edge events rather than by polling, as
// key_input.c reads the key jack. Every knob's three lines are one GPIO v2
// line request; the kernel stamps each edge in its interrupt handler, and
// a thread of its own sleeps in ppoll() on that fd until one arrives.
//
// Turns: an encoder's A and B contacts step through a two-bit Gray code,
// one edge at a time, and rest at a click (detent) with both open. Each
// edge is counted +1 or -1 by the pair of states it joins, so contact
// bounce - one line chattering between two neighbouring states - counts
// up and down and nets to nothing. A click is reported when the encoder
// arrives at a rest state having counted at least half a click's edges in
// one direction; the count restarts at every rest state. If the kernel
// reports lost edges, every knob is re-armed from its lines' levels.
//
// Presses: leading-edge debounce. The first edge that closes the switch is
// a press at once; its edges are then ignored for SWITCH_DEBOUNCE_MS, and
// at the end of that lockout its latest level is taken if it differs.

#define _GNU_SOURCE // ppoll()

#include "encoder.h"
#include "gpio.h"
#include <errno.h>
#include <linux/gpio.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SWITCH_DEBOUNCE_MS 20
#define KERNEL_EVENT_BUFFER 256
#define MAX_LINES (RADIO_KNOBS * 3)

static const char *const knob_name[RADIO_KNOBS] = {"tuning", "volume"};

/* ---- Decoding ------------------------------------------------------------- */

struct knob {
  int active;
  int edges_per_detent;
  int a, b;           // the contacts' levels as last seen
  int count;          // edges counted since the last rest state, + for up
  int sw_raw;         // the switch's level as last seen, bounce included (1 = open)
  int sw_open;        // its debounced state
  int sw_check_due;   // a lockout is running
  int64_t sw_lock_until;
};

static struct knob knobs[RADIO_KNOBS];
static encoder_turn_fn turn_hook = NULL;
static encoder_push_fn push_hook = NULL;

// The Gray-code step from state p to state c (state = A << 1 | B): +1 for
// the sequence 11 -> 01 -> 00 -> 10 -> 11 (A changes first), -1 for the
// reverse, 0 for no change or for both bits changing at once.
static int gray_step(int p, int c) {
  static const int step[16] = {
      //       c: 00  01  10  11
      /* p 00 */ 0, -1, +1, 0,
      /* p 01 */ +1, 0, 0, -1,
      /* p 10 */ -1, 0, 0, +1,
      /* p 11 */ 0, +1, -1, 0,
  };
  return step[(p << 2) | c];
}

// A rest state: where the knob sits at a click. Both contacts open on
// every encoder; one with two edges a click also rests with both closed.
static int at_rest(const struct knob *n, int state) {
  return state == 3 || (n->edges_per_detent == 2 && state == 0);
}

void encoder_set_hooks(encoder_turn_fn on_turn, encoder_push_fn on_push) {
  turn_hook = on_turn;
  push_hook = on_push;
}

void encoder_arm(enum radio_knob k, int edges_per_detent, int a, int b, int sw) {
  struct knob *n = &knobs[k];
  n->active = 1;
  n->edges_per_detent = edges_per_detent == 2 ? 2 : 4;
  n->a = a != 0;
  n->b = b != 0;
  n->count = 0;
  n->sw_raw = n->sw_open = sw != 0;
  n->sw_check_due = 0;
  n->sw_lock_until = 0;
}

static void contact_edge(enum radio_knob k, enum encoder_line line, int level, int64_t ts) {
  struct knob *n = &knobs[k];
  int p = (n->a << 1) | n->b;
  if (line == ENCODER_A)
    n->a = level;
  else
    n->b = level;
  int c = (n->a << 1) | n->b;
  n->count += gray_step(p, c);
  if (!at_rest(n, c))
    return;
  int half = n->edges_per_detent / 2;
  int detents = n->count >= half ? 1 : n->count <= -half ? -1 : 0;
  n->count = 0;
  if (detents && turn_hook)
    turn_hook(k, detents, ts);
}

static void switch_take(enum radio_knob k, int open, int64_t ts) {
  struct knob *n = &knobs[k];
  n->sw_open = open;
  n->sw_lock_until = ts + SWITCH_DEBOUNCE_MS * 1000000LL;
  n->sw_check_due = 1;
  if (!open && push_hook)
    push_hook(k);
}

static void switch_end_lockout_if_due(enum radio_knob k, int64_t t) {
  struct knob *n = &knobs[k];
  while (n->sw_check_due && t >= n->sw_lock_until) {
    n->sw_check_due = 0;
    if (n->sw_raw != n->sw_open)
      switch_take(k, n->sw_raw, n->sw_lock_until);
  }
}

void encoder_edge(enum radio_knob k, enum encoder_line line, int level, int64_t ts_ns) {
  struct knob *n = &knobs[k];
  if (!n->active)
    return;
  level = level != 0;
  if (line != ENCODER_SW) {
    if ((line == ENCODER_A ? n->a : n->b) != level)
      contact_edge(k, line, level, ts_ns);
    return;
  }
  switch_end_lockout_if_due(k, ts_ns);
  n->sw_raw = level;
  if (!n->sw_check_due && n->sw_raw != n->sw_open)
    switch_take(k, n->sw_raw, ts_ns);
}

void encoder_tick(int64_t now_ns) {
  for (int k = 0; k < RADIO_KNOBS; k++)
    if (knobs[k].active)
      switch_end_lockout_if_due((enum radio_knob)k, now_ns);
}

int64_t encoder_next_due(void) {
  int64_t due = 0;
  for (int k = 0; k < RADIO_KNOBS; k++)
    if (knobs[k].active && knobs[k].sw_check_due && (due == 0 || knobs[k].sw_lock_until < due))
      due = knobs[k].sw_lock_until;
  return due;
}

/* ---- The thread ------------------------------------------------------------ */

static int line_fd = -1;
static int n_lines = 0;
static unsigned line_pin[MAX_LINES];
static struct {
  enum radio_knob knob;
  enum encoder_line line;
  uint32_t seqno; // kernel's per-line sequence number of the last edge
} line_role[MAX_LINES];
static struct radio_hw_knob knob_hw[RADIO_KNOBS];
static pthread_t thread;

static int64_t monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Arms every knob from its lines' levels now. At start-up, and again if
// the kernel's event queue overflowed and the levels seen can't be trusted.
static int arm_from_lines(void) {
  int levels = gpio_read_lines(line_fd, n_lines);
  if (levels < 0)
    return -1;
  int lv[RADIO_KNOBS][3];
  for (int k = 0; k < RADIO_KNOBS; k++)
    lv[k][0] = lv[k][1] = lv[k][2] = 1;
  for (int i = 0; i < n_lines; i++)
    lv[line_role[i].knob][line_role[i].line] = (levels >> i) & 1;
  for (int k = 0; k < RADIO_KNOBS; k++)
    if (knob_hw[k].a_pin >= 0)
      encoder_arm((enum radio_knob)k, knob_hw[k].edges_per_detent, lv[k][0], lv[k][1], lv[k][2]);
  return 0;
}

static void *encoder_loop(void *arg) {
  (void)arg;
  struct gpio_v2_line_event evs[16];

  for (;;) {
    struct timespec timeout, *tp = NULL;
    int64_t due = encoder_next_due();
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
    if (r < 0 && errno == EINTR)
      continue;
    if (r < 0 || (r > 0 && !(p.revents & POLLIN))) {
      fprintf(stderr, "knobs: poll failed (%s) - the knobs are no longer read\n",
              r < 0 ? strerror(errno) : "line request gone");
      return NULL;
    }
    if (r > 0) {
      ssize_t got = read(line_fd, evs, sizeof(evs));
      if (got < 0 && errno != EINTR && errno != EAGAIN) {
        fprintf(stderr, "knobs: read failed (%s) - the knobs are no longer read\n",
                strerror(errno));
        return NULL;
      }
      int lost = 0;
      for (ssize_t i = 0; got > 0 && i < got / (ssize_t)sizeof(evs[0]); i++) {
        const struct gpio_v2_line_event *e = &evs[i];
        for (int j = 0; j < n_lines; j++) {
          if (line_pin[j] != e->offset)
            continue;
          if (line_role[j].seqno != 0 && e->line_seqno != line_role[j].seqno + 1)
            lost = 1;
          line_role[j].seqno = e->line_seqno;
          // Pulled up, closing to ground: a rising edge is the line going high.
          encoder_edge(line_role[j].knob, line_role[j].line,
                       e->id == GPIO_V2_LINE_EVENT_RISING_EDGE, (int64_t)e->timestamp_ns);
        }
      }
      if (lost) {
        printf("knobs: the kernel dropped edges - resyncing to where the knobs rest\n");
        arm_from_lines();
      }
    }
    encoder_tick(monotonic_ns());
  }
  return NULL;
}

int encoder_start(encoder_turn_fn on_turn, encoder_push_fn on_push) {
  encoder_set_hooks(on_turn, on_push);
  int n_knobs = 0;
  n_lines = 0;
  for (int k = 0; k < RADIO_KNOBS; k++) {
    knob_hw[k] = radio_hw_knob((enum radio_knob)k);
    if (knob_hw[k].a_pin < 0)
      continue;
    const int pins[3] = {knob_hw[k].a_pin, knob_hw[k].b_pin, knob_hw[k].sw_pin};
    for (int l = 0; l < 3; l++) {
      line_pin[n_lines] = (unsigned)pins[l];
      line_role[n_lines].knob = (enum radio_knob)k;
      line_role[n_lines].line = (enum encoder_line)l;
      line_role[n_lines].seqno = 0;
      n_lines++;
    }
    n_knobs++;
  }
  if (n_knobs == 0)
    return 0;

  line_fd = gpio_request_edge_inputs(line_pin, n_lines, KERNEL_EVENT_BUFFER, "maxibitx-knobs");
  if (line_fd < 0)
    return -1;
  usleep(1000); // let the pull-ups charge an open contact before reading it
  if (arm_from_lines() < 0)
    return -1;

  // Ordinary scheduling: a click handled a millisecond late is not heard.
  int rc = pthread_create(&thread, NULL, encoder_loop, NULL);
  if (rc != 0) {
    fprintf(stderr, "knobs: cannot start the encoder thread: %s\n", strerror(rc));
    return -1;
  }
  for (int k = 0; k < RADIO_KNOBS; k++)
    if (knob_hw[k].a_pin >= 0)
      printf("init: %s knob on BCM %d/%d, switch BCM %d\n", knob_name[k], knob_hw[k].a_pin,
             knob_hw[k].b_pin, knob_hw[k].sw_pin);
  return n_knobs;
}
