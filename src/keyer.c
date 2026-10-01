// keyer.c
//
// The paddle keyer, written from the specification in
// docs/dsp_design_notes/cw_keyer_design_study.md §15. Time is counted in
// samples from cw_init(); every paddle edge arrives at its own sample, so
// "before" and "during" an element are exact. Rather than stepping every
// sample, each block is walked from one instant that matters to the next -
// an edge, the end of a mark, a decision point - and the key value is
// constant in between.
//
// An element is a mark (T for a dot, 3T for a dash) and a trailing space of
// T; the end of that space is its decision point, where the next element is
// chosen. Iambic A and B latch a closure of the opposite paddle made while
// an element is in progress; B also latches the opposite paddle if it is
// already closed as an element starts. Ultimatic gives the most recently
// closed paddle the win and latches a tap of the other. The bug sends
// automatic dots with the dash contact keying directly.
// tools/keyer_study/keyer_spec_model.c is the same specification stepped one
// sample at a time, and keyer_test.c checks this against it.
//
// Text runs on the same elements: a character's pattern (morse.c) supplies
// each next element at its decision point, and a gap phase stretches the
// space after a character to 3T, or to 7T before the next word. Speed codes
// in the text move its speed from the next character on, the gap before
// that character included.

#include "keyer.h"
#include "morse.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

enum phase { IDLE, MARK, SPACE, GAP };

static _Atomic int wanted_mode = KEYER_STRAIGHT; // as last set
static _Atomic int wpm = KEYER_WPM_DEFAULT;
static enum keyer_mode mode = KEYER_STRAIGHT; // in force - audio thread only

// Keyer state, audio thread only. Times are absolute sample indexes.
static int64_t base;          // the current block's first sample
static int down[2];           // paddles, KEY_DOT / KEY_DASH
static int64_t pressed_at[2]; // when each last closed
static int mem[2];            // element memories
static unsigned contacts;     // straight key: bit per enum key_contact, closed
static enum phase phase = IDLE;
static int cur;               // the element in progress
static int64_t mark_end, decide_at;

// Text queue: control threads write under text_lock, the audio thread reads
// without it. Indexes run freely; the slot is index % KEYER_TEXT_QUEUE_LEN.
static unsigned char text_queue[KEYER_TEXT_QUEUE_LEN];
static _Atomic unsigned text_head = 0; // next slot to write
static _Atomic unsigned text_tail = 0; // next slot to read
static pthread_mutex_t text_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int text_stop_request = 0;
static _Atomic unsigned text_stop_at = 0;  // keyer_stop_text(): discard up to here
static _Atomic unsigned text_interrupts = 0; // text stopped by a contact closing
static _Atomic int text_sending = 0; // text_mode, for other threads

// Text in progress, audio thread only.
static int text_mode;             // the element or gap in progress is text's
static int text_abort;            // end text after the element in progress
static const char *text_pattern;  // the character being sent
static int text_index;            // its next element
static int64_t gap_start, gap_end; // a GAP phase
static int text_speed;            // WPM added by speed codes, text only
static int text_cut;              // stopped: drop the rest of this character
static unsigned text_char_at;     // queue index of the character in progress
static int64_t text_resume_at;    // text arriving after its queue ran dry waits till here

// One dit: 1.2 s / WPM at 96 kHz, rounded to a sample.
static int64_t dit_samples(void) {
  int w = atomic_load(&wpm) + (text_mode ? text_speed : 0);
  if (w < KEYER_WPM_MIN)
    w = KEYER_WPM_MIN;
  if (w > KEYER_WPM_MAX)
    w = KEYER_WPM_MAX;
  return (115200 + w / 2) / w;
}

static void start_element(int which, int64_t t) {
  int64_t T = dit_samples(); // read here, so a speed change waits for an element start
  cur = which;
  phase = MARK;
  mark_end = t + (which == KEY_DOT ? 1 : 3) * T;
  decide_at = mark_end + T;
  mem[KEY_DOT] = mem[KEY_DASH] = 0;
  // Iambic B: the opposite paddle counts if it is closed at any point in the
  // element, including already closed as it starts.
  if (mode == KEYER_IAMBIC_B && down[!which])
    mem[!which] = 1;
}

/* ---- Text --------------------------------------------------------------- */

static int text_peek(unsigned char *c) {
  unsigned t = atomic_load_explicit(&text_tail, memory_order_relaxed);
  if (t == atomic_load_explicit(&text_head, memory_order_acquire))
    return 0;
  *c = text_queue[t % KEYER_TEXT_QUEUE_LEN];
  return 1;
}

static void text_pop(void) {
  atomic_store_explicit(&text_tail, atomic_load_explicit(&text_tail, memory_order_relaxed) + 1,
                        memory_order_release);
}

static void text_discard(void) {
  atomic_store_explicit(&text_tail, atomic_load_explicit(&text_head, memory_order_acquire),
                        memory_order_release);
}

static void text_end(void) {
  text_mode = 0;
  text_abort = 0;
  text_cut = 0;
  text_speed = 0;
  atomic_store(&text_sending, 0);
}

// Takes any speed codes at the head of the queue, applying them, and then
// peeks at the character after them. Returns 0 if there is none.
static int text_peek_char(unsigned char *c) {
  while (text_peek(c)) {
    if (*c < MORSE_SPEED_DOWN || *c > MORSE_SPEED_BASE)
      return 1;
    if (*c == MORSE_SPEED_BASE)
      text_speed = 0;
    else
      text_speed += *c == MORSE_SPEED_UP ? MORSE_SPEED_STEP : -MORSE_SPEED_STEP;
    if (text_speed < -KEYER_WPM_MAX)
      text_speed = -KEYER_WPM_MAX;
    if (text_speed > KEYER_WPM_MAX)
      text_speed = KEYER_WPM_MAX;
    text_pop();
  }
  return 0;
}

static int element_of(char symbol) { return symbol == '-' ? KEY_DASH : KEY_DOT; }

// Starts the next character in the queue at time t, skipping spaces (a word
// space is timed by the gap before it, so spaces here lead nowhere) and
// anything the table lacks. Returns 0 if the queue had none.
static int text_start_char(int64_t t) {
  unsigned char c;
  text_mode = 1; // so the speed codes below count
  while (text_peek_char(&c)) {
    text_char_at = atomic_load_explicit(&text_tail, memory_order_relaxed);
    text_pop();
    const char *p = morse_pattern(c);
    if (!p)
      continue;
    text_pattern = p;
    text_index = 1;
    atomic_store(&text_sending, 1);
    start_element(element_of(p[0]), t);
    return 1;
  }
  text_mode = 0;
  text_speed = 0;
  return 0;
}

// Text's decision point, at time t: the character's next element, or the
// gap to the next character or word, or the end.
static void text_next(int64_t t) {
  unsigned char c;
  if (text_cut) {
    // keyer_stop_text(): the rest of the character is dropped, and whatever
    // was queued since is a new text, a word space on at the keyer's speed.
    text_cut = 0;
    text_speed = 0;
    if (!text_peek_char(&c)) {
      text_end();
      phase = IDLE;
      return;
    }
    while (text_peek_char(&c) && c == ' ')
      text_pop();
    gap_start = t;
    gap_end = t + 6 * dit_samples();
    phase = GAP;
    return;
  }
  if (text_pattern[text_index]) {
    start_element(element_of(text_pattern[text_index++]), t);
    return;
  }
  if (!text_peek_char(&c)) {
    // More may yet come (a TCI callsign is fed a character at a time): if it
    // does, it waits out the 3T a character space needs.
    text_resume_at = t + 2 * dit_samples();
    text_end();
    phase = IDLE;
    return;
  }
  // T of space has already passed since the mark: 2T more makes the 3T
  // between characters, 6T more the 7T between words.
  if (c == ' ') {
    while (text_peek_char(&c) && c == ' ')
      text_pop();
    gap_end = t + 6 * dit_samples();
  } else {
    gap_end = t + 2 * dit_samples();
  }
  gap_start = t;
  phase = GAP;
}

// The character in progress ends with the element in progress.
static void text_abort_current(void) {
  if (!text_mode)
    return;
  if (phase == GAP) {
    text_end();
    phase = IDLE;
  } else {
    text_abort = 1;
  }
}

static int text_waiting(void) {
  return atomic_load_explicit(&text_head, memory_order_acquire) !=
         atomic_load_explicit(&text_tail, memory_order_relaxed);
}

// A contact closed: the element in progress completes, and the rest of the
// text is discarded.
static void text_stop(void) {
  if (text_mode || text_waiting())
    atomic_fetch_add(&text_interrupts, 1);
  text_discard();
  text_abort_current();
}

// keyer_stop_text(): the text queued before upto is dropped, the element in
// progress completes, and text queued since follows a word space later.
// A character queued after the stop, already started, is left alone.
static void text_stop_before(unsigned upto) {
  unsigned t = atomic_load_explicit(&text_tail, memory_order_relaxed);
  if ((int)(upto - t) > 0)
    atomic_store_explicit(&text_tail, upto, memory_order_release);
  if (!text_mode || (int)(upto - text_char_at) <= 0)
    return;
  if (phase == GAP) {
    text_speed = 0;
    int64_t word = gap_start + 6 * dit_samples();
    if (word > gap_end && atomic_load_explicit(&text_head, memory_order_acquire) != upto)
      gap_end = word; // only if new text follows
  } else {
    text_cut = 1;
  }
}

// Which element to send, from idle or at a decision point; -1 for none.
static int choose(int from_idle) {
  int both = down[KEY_DOT] && down[KEY_DASH];
  switch (mode) {
  case KEYER_IAMBIC_A:
  case KEYER_IAMBIC_B:
    if (from_idle) {
      if (both) // first closed wins, the dot on a tie
        return pressed_at[KEY_DASH] < pressed_at[KEY_DOT] ? KEY_DASH : KEY_DOT;
      return down[KEY_DOT] ? KEY_DOT : down[KEY_DASH] ? KEY_DASH : -1;
    } else {
      int opp = !cur;
      if (mem[opp] || down[opp])
        return opp;
      if (down[cur])
        return cur;
      return -1;
    }
  case KEYER_ULTIMATIC:
    if (both) // last closed wins
      return pressed_at[KEY_DASH] > pressed_at[KEY_DOT] ? KEY_DASH : KEY_DOT;
    if (down[KEY_DOT])
      return KEY_DOT;
    if (down[KEY_DASH])
      return KEY_DASH;
    if (!from_idle && mem[!cur]) // a tap made during the element is never lost
      return !cur;
    return -1;
  case KEYER_BUG:
    return down[KEY_DOT] ? KEY_DOT : -1; // automatic dots only
  default:
    return -1;
  }
}

static void apply_edge(const struct key_event *e, int64_t t) {
  unsigned bit = 1u << e->contact;
  contacts = e->closed ? (contacts | bit) : (contacts & ~bit);
  if (e->closed)
    text_stop(); // any closure stops text
  int p = e->paddle;
  if (e->closed && !down[p]) {
    down[p] = 1;
    pressed_at[p] = t;
    // A closure of the other paddle while an element is in progress sets
    // its memory. The bug keyer has none.
    if (phase != IDLE && p != cur &&
        (mode == KEYER_IAMBIC_A || mode == KEYER_IAMBIC_B || mode == KEYER_ULTIMATIC))
      mem[p] = 1;
  } else if (!e->closed) {
    down[p] = 0;
  }
}

// Everything due at time t, after that instant's edges.
static void step(int64_t t) {
  if (phase == MARK && t >= mark_end)
    phase = SPACE;
  if (phase == SPACE && t >= decide_at) {
    if (text_mode && !text_abort) {
      text_next(t);
    } else {
      // Paddles decide, including right after text a closure stopped: the
      // closure set a memory, or its paddle is still down.
      text_end();
      int next = choose(0);
      if (next >= 0)
        start_element(next, t);
      else
        phase = IDLE;
    }
  }
  if (phase == GAP && t >= gap_end && !text_start_char(t)) {
    text_end();
    phase = IDLE;
  }
  if (phase == IDLE) {
    int want = atomic_load(&wanted_mode);
    if (want != (int)mode) {
      mode = (enum keyer_mode)want;
      mem[KEY_DOT] = mem[KEY_DASH] = 0;
    }
    // Queued text starts once the key is idle; a contact held closed stops
    // it instead, as a closure during it would.
    if (contacts != 0) {
      if (text_waiting())
        atomic_fetch_add(&text_interrupts, 1);
      text_discard();
    } else if (t >= text_resume_at && text_start_char(t)) {
      return;
    }
    int next = choose(1);
    if (next >= 0)
      start_element(next, t);
  }
}

static int key_value(void) {
  switch (mode) {
  case KEYER_STRAIGHT:
    return contacts != 0 || (text_mode && phase == MARK);
  case KEYER_BUG:
    return phase == MARK || down[KEY_DASH]; // automatic dots OR the manual dash
  default:
    return phase == MARK;
  }
}

int keyer_run_block(const struct key_event *ev, int n_ev, uint8_t *key, int n) {
  int any = 0, e = 0, pos = 0;
  if (atomic_exchange(&text_stop_request, 0))
    text_stop_before(atomic_load(&text_stop_at));
  while (pos < n) {
    int64_t t = base + pos;
    while (e < n_ev && ev[e].offset <= pos)
      apply_edge(&ev[e++], t);
    step(t);

    // The next instant anything can change.
    int64_t next = base + n;
    if (e < n_ev && base + ev[e].offset < next)
      next = base + ev[e].offset;
    if (phase == MARK && mark_end < next)
      next = mark_end;
    else if (phase == SPACE && decide_at < next)
      next = decide_at;
    else if (phase == GAP && gap_end < next)
      next = gap_end;
    else if (phase == IDLE && text_resume_at > t && text_resume_at < next)
      next = text_resume_at;
    if (next <= t)
      next = t + 1;

    int v = key_value();
    int end = (int)(next - base);
    for (; pos < end; pos++)
      key[pos] = (uint8_t)v;
    any |= v | (phase != IDLE);
  }
  // Offsets past the block (none from key_input_take()) still count.
  while (e < n_ev)
    apply_edge(&ev[e++], base + n - 1);
  base += n;
  return any;
}

void keyer_reset(void) {
  base = 0;
  down[KEY_DOT] = down[KEY_DASH] = 0;
  pressed_at[KEY_DOT] = pressed_at[KEY_DASH] = 0;
  mem[KEY_DOT] = mem[KEY_DASH] = 0;
  contacts = 0;
  phase = IDLE;
  cur = KEY_DOT;
  mark_end = decide_at = gap_start = gap_end = text_resume_at = 0;
  mode = (enum keyer_mode)atomic_load(&wanted_mode);
  text_discard();
  text_end();
  atomic_store(&text_stop_request, 0);
}

int keyer_send_text(const char *text) {
  size_t len = strlen(text);
  pthread_mutex_lock(&text_lock);
  unsigned h = atomic_load_explicit(&text_head, memory_order_relaxed);
  unsigned t = atomic_load_explicit(&text_tail, memory_order_acquire);
  if (len > KEYER_TEXT_QUEUE_LEN - (h - t)) {
    pthread_mutex_unlock(&text_lock);
    return -1;
  }
  for (size_t i = 0; i < len; i++)
    text_queue[(h + i) % KEYER_TEXT_QUEUE_LEN] = (unsigned char)text[i];
  atomic_store_explicit(&text_head, h + (unsigned)len, memory_order_release);
  pthread_mutex_unlock(&text_lock);
  return 0;
}

// Lock-free, since cw.c calls it from the audio thread: text_stop_at only
// moves forward, so two stops racing leave the later one's mark.
void keyer_stop_text(void) {
  unsigned h = atomic_load(&text_head), cur = atomic_load(&text_stop_at);
  while ((int)(h - cur) > 0 && !atomic_compare_exchange_weak(&text_stop_at, &cur, h)) {
  }
  atomic_store(&text_stop_request, 1);
}

int keyer_text_queued(void) {
  unsigned t = atomic_load(&text_tail); // first: the tail never passes the head
  return (int)(atomic_load(&text_head) - t);
}

int keyer_text_letters_queued(void) {
  pthread_mutex_lock(&text_lock); // holds the head still; the tail only advances
  unsigned h = atomic_load(&text_head);
  int n = 0;
  for (unsigned i = atomic_load(&text_tail); i != h; i++)
    if (morse_pattern(text_queue[i % KEYER_TEXT_QUEUE_LEN]))
      n++;
  pthread_mutex_unlock(&text_lock);
  return n;
}

unsigned keyer_text_interrupts(void) { return atomic_load(&text_interrupts); }

int keyer_text_busy(void) {
  return atomic_load(&text_sending) ||
         atomic_load(&text_head) != atomic_load(&text_tail);
}

int keyer_text_room(void) {
  return KEYER_TEXT_QUEUE_LEN - (int)(atomic_load(&text_head) - atomic_load(&text_tail));
}

int keyer_set_mode(enum keyer_mode m) {
  if ((int)m < 0 || m >= KEYER_MODES)
    return -1;
  atomic_store(&wanted_mode, (int)m);
  return 0;
}

enum keyer_mode keyer_get_mode(void) { return (enum keyer_mode)atomic_load(&wanted_mode); }

int keyer_set_wpm(int w) {
  if (w < KEYER_WPM_MIN)
    w = KEYER_WPM_MIN;
  if (w > KEYER_WPM_MAX)
    w = KEYER_WPM_MAX;
  atomic_store(&wpm, w);
  return w;
}

int keyer_get_wpm(void) { return atomic_load(&wpm); }

const char *keyer_mode_name(enum keyer_mode m) {
  static const char *const names[KEYER_MODES] = {"straight", "bug", "ultimatic", "iambic A",
                                                 "iambic B"};
  return ((int)m >= 0 && m < KEYER_MODES) ? names[m] : "?";
}
