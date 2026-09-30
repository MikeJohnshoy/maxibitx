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

#include "keyer.h"
#include <stdatomic.h>

enum phase { IDLE, MARK, SPACE };

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

// One dit: 1.2 s / WPM at 96 kHz, rounded to a sample.
static int64_t dit_samples(void) {
  int w = atomic_load(&wpm);
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
    int next = choose(0);
    if (next >= 0)
      start_element(next, t);
    else
      phase = IDLE;
  }
  if (phase == IDLE) {
    int want = atomic_load(&wanted_mode);
    if (want != (int)mode) {
      mode = (enum keyer_mode)want;
      mem[KEY_DOT] = mem[KEY_DASH] = 0;
    }
    int next = choose(1);
    if (next >= 0)
      start_element(next, t);
  }
}

static int key_value(void) {
  switch (mode) {
  case KEYER_STRAIGHT:
    return contacts != 0;
  case KEYER_BUG:
    return phase == MARK || down[KEY_DASH]; // automatic dots OR the manual dash
  default:
    return phase == MARK;
  }
}

int keyer_run_block(const struct key_event *ev, int n_ev, uint8_t *key, int n) {
  int any = 0, e = 0, pos = 0;
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
  mark_end = decide_at = 0;
  mode = (enum keyer_mode)atomic_load(&wanted_mode);
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
