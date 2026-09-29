// keyer_spec_model.c - executable form of the keyer mode specification in
// docs/dsp_design_notes/cw_keyer_design_study.md §15, and the golden cases
// a real keyer must reproduce. Not built by the Makefile:
//   gcc -O2 -Wall -std=gnu11 keyer_spec_model.c -o keyer_spec_model && ./keyer_spec_model
//
// Written from the specification, not from any existing keyer. Input is a
// list of timestamped paddle edges (sample times at 96 kHz, logical DOT and
// DASH after any reversal); output is the key-down/up stream, from which
// each case checks the element sequence and the exact element and space
// lengths. A model, not the implementation: it steps one sample at a time
// for clarity, where the real keyer would jump between event and decision
// times.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 96000
#define DOT 0
#define DASH 1

enum mode { STRAIGHT, BUG, ULTIMATIC, IAMBIC_A, IAMBIC_B };
static const char *mode_name[] = { "straight", "bug", "ultimatic", "iambic A", "iambic B" };

struct edge { long t; int line; int down; };

enum phase { IDLE, MARK, SPACE };

struct keyer {
  enum mode mode;
  long T;                 // one dit in samples
  int down[2];            // contact state, DOT and DASH
  long pressed_at[2];     // when each contact last closed
  int mem[2];             // element memories
  enum phase phase;
  int cur;                // element being sent
  long mark_end, decide_at;
};

static void start_element(struct keyer *k, int which, long t) {
  k->cur = which;
  k->phase = MARK;
  k->mark_end = t + (which == DOT ? 1 : 3) * k->T;
  k->decide_at = k->mark_end + k->T;
  k->mem[DOT] = k->mem[DASH] = 0;
  // Iambic B: the opposite paddle counts if it is closed at any point in
  // the element - including already closed as the element starts.
  if (k->mode == IAMBIC_B && k->down[!which]) k->mem[!which] = 1;
}

// A contact closure while an element is in progress (mark or its trailing
// space). Iambic A and B latch the opposite paddle; ultimatic latches
// whichever paddle is not the current element, so a quick tap is never
// lost. The bug has no memories.
static void on_press(struct keyer *k, int line) {
  if (k->phase == IDLE) return;
  if (k->mode == IAMBIC_A || k->mode == IAMBIC_B || k->mode == ULTIMATIC)
    if (line != k->cur) k->mem[line] = 1;
}

// Which element to send next, from idle or at a decision point; -1 = none.
static int choose(struct keyer *k, int from_idle) {
  int both = k->down[DOT] && k->down[DASH];
  switch (k->mode) {
  case IAMBIC_A:
  case IAMBIC_B:
    if (from_idle) {
      if (both) // squeeze from idle: first closed wins, dot on a tie
        return k->pressed_at[DASH] < k->pressed_at[DOT] ? DASH : DOT;
      return k->down[DOT] ? DOT : k->down[DASH] ? DASH : -1;
    } else {
      int opp = !k->cur;
      if (k->mem[opp] || k->down[opp]) return opp;
      if (k->down[k->cur]) return k->cur;
      return -1;
    }
  case ULTIMATIC:
    if (both) // last closed wins
      return k->pressed_at[DASH] > k->pressed_at[DOT] ? DASH : DOT;
    if (k->down[DOT]) return DOT;
    if (k->down[DASH]) return DASH;
    if (!from_idle && k->mem[!k->cur]) return !k->cur;
    return -1;
  case BUG:
    return k->down[DOT] ? DOT : -1; // automatic dots only
  default:
    return -1;
  }
}

// Runs the model over [0, horizon) and writes the key stream.
static void run(enum mode mode, int wpm, const struct edge *ev, int n_ev, long horizon,
                unsigned char *key) {
  struct keyer k;
  memset(&k, 0, sizeof(k));
  k.mode = mode;
  k.T = (FS * 12L / 10 + wpm / 2) / wpm; // 1.2 s / WPM, rounded to a sample
  k.phase = IDLE;
  int e = 0;
  for (long t = 0; t < horizon; t++) {
    while (e < n_ev && ev[e].t == t) {
      int l = ev[e].line;
      if (ev[e].down && !k.down[l]) { k.pressed_at[l] = t; k.down[l] = 1; on_press(&k, l); }
      else if (!ev[e].down) k.down[l] = 0;
      e++;
    }
    if (mode != STRAIGHT) {
      if (k.phase == MARK && t == k.mark_end) k.phase = SPACE;
      if (k.phase == SPACE && t == k.decide_at) {
        int next = choose(&k, 0);
        if (next >= 0) start_element(&k, next, t); else k.phase = IDLE;
      }
      if (k.phase == IDLE) {
        int next = choose(&k, 1);
        if (next >= 0) start_element(&k, next, t);
      }
    }
    if (mode == STRAIGHT) key[t] = k.down[DOT] || k.down[DASH];
    else if (mode == BUG) key[t] = (k.phase == MARK) || k.down[DASH];
    else key[t] = (k.phase == MARK);
  }
}

// Element string (. - or m for a manual mark), plus a check that every
// intra-character space the keyer times is exactly one dit. Automatic marks
// are checked by the classification itself: only exactly 1 or 3 dits decode
// as . or -.
static int decode(const unsigned char *key, long horizon, long T, char *out, int *timing_ok,
                  int operator_timed) {
  int n = 0;
  *timing_ok = 1;
  long last_up = -1;
  for (long t = 1; t < horizon; t++) {
    if (key[t] && !key[t - 1]) {
      if (!operator_timed && last_up >= 0 && t - last_up < 3 * T && t - last_up != T)
        *timing_ok = 0;
      long s = t;
      while (t < horizon && key[t]) t++;
      long len = t - s;
      out[n++] = len == T ? '.' : len == 3 * T ? '-' : 'm';
      last_up = t;
    }
  }
  out[n] = 0;
  return n;
}

struct gcase {
  const char *what;
  enum mode mode;
  int wpm;
  struct edge ev[8];
  int n_ev;
  double horizon_dits;
  const char *expect;
};

// Times in dits, converted per case. D(x) builds a sample time.
#define E(x, line, down) { (long)((x) * 1000), line, down }

int main(void) {
  // Edge times below are written in thousandths of a dit and scaled to
  // samples per case, so each case reads at any speed.
  struct gcase cases[] = {
    { "single dot tap from idle", IAMBIC_A, 20,
      { E(0, DOT, 1), E(0.5, DOT, 0) }, 2, 6, "." },
    { "dot held for 5 dits", IAMBIC_A, 20,
      { E(0, DOT, 1), E(5.0, DOT, 0) }, 2, 12, "..." },
    { "C, squeeze dah-first, release in the last dit (A)", IAMBIC_A, 20,
      { E(0, DASH, 1), E(0.5, DOT, 1), E(10.5, DOT, 0), E(10.5, DASH, 0) }, 4, 20, "-.-." },
    { "same squeeze released in the second dah (A)", IAMBIC_A, 20,
      { E(0, DASH, 1), E(0.5, DOT, 1), E(6.5, DOT, 0), E(6.5, DASH, 0) }, 4, 20, "-.-" },
    { "same squeeze released in the second dah (B)", IAMBIC_B, 20,
      { E(0, DASH, 1), E(0.5, DOT, 1), E(6.5, DOT, 0), E(6.5, DASH, 0) }, 4, 20, "-.-." },
    { "dah with a dot tapped inside it (A)", IAMBIC_A, 20,
      { E(0, DASH, 1), E(0.3, DASH, 0), E(1.0, DOT, 1), E(1.2, DOT, 0) }, 4, 12, "-." },
    { "dah with a dot tapped inside it (B)", IAMBIC_B, 20,
      { E(0, DASH, 1), E(0.3, DASH, 0), E(1.0, DOT, 1), E(1.2, DOT, 0) }, 4, 12, "-." },
    { "squeeze from idle, dot closed 1/10 dit first", IAMBIC_B, 20,
      { E(0, DOT, 1), E(0.1, DASH, 1), E(0.5, DOT, 0), E(0.5, DASH, 0) }, 4, 12, ".-" },
    { "squeeze from idle, dah closed 1/10 dit first", IAMBIC_B, 20,
      { E(0, DASH, 1), E(0.1, DOT, 1), E(0.5, DOT, 0), E(0.5, DASH, 0) }, 4, 12, "-." },
    { "ultimatic: dit held, dah added, dah released", ULTIMATIC, 20,
      { E(0, DOT, 1), E(3.0, DASH, 1), E(10.5, DASH, 0), E(14.5, DOT, 0) }, 4, 24, "..--.." },
    { "ultimatic: dah held, dit tapped (memory)", ULTIMATIC, 20,
      { E(0, DASH, 1), E(1.0, DOT, 1), E(1.3, DOT, 0), E(3.5, DASH, 0) }, 4, 16, "-." },
    { "bug: dits while dit held, then a manual dash", BUG, 20,
      { E(0, DOT, 1), E(5.0, DOT, 0), E(8.0, DASH, 1), E(10.0, DASH, 0) }, 4, 16, "...m" },
    { "straight key follows the contact", STRAIGHT, 20,
      { E(0, DOT, 1), E(2.2, DOT, 0), E(4.0, DOT, 1), E(4.4, DOT, 0) }, 4, 8, "mm" },
    { "C at 1 WPM (A)", IAMBIC_A, 1,
      { E(0, DASH, 1), E(0.5, DOT, 1), E(10.5, DOT, 0), E(10.5, DASH, 0) }, 4, 20, "-.-." },
    { "C at 60 WPM (B)", IAMBIC_B, 60,
      { E(0, DASH, 1), E(0.5, DOT, 1), E(6.5, DOT, 0), E(6.5, DASH, 0) }, 4, 20, "-.-." },
  };
  int fails = 0, ncases = (int)(sizeof(cases) / sizeof(cases[0]));
  for (int c = 0; c < ncases; c++) {
    struct gcase *g = &cases[c];
    long T = (FS * 12L / 10 + g->wpm / 2) / g->wpm;
    struct edge ev[8];
    for (int i = 0; i < g->n_ev; i++) {
      ev[i] = g->ev[i];
      ev[i].t = 1000 + g->ev[i].t * T / 1000; // start 1000 samples in
    }
    long horizon = 1000 + (long)(g->horizon_dits * T);
    unsigned char *key = calloc((size_t)horizon, 1);
    run(g->mode, g->wpm, ev, g->n_ev, horizon, key);
    char got[64];
    int timing_ok;
    decode(key, horizon, T, got, &timing_ok, g->mode == STRAIGHT);
    int ok = strcmp(got, g->expect) == 0 && timing_ok;
    if (!ok) fails++;
    printf("%-4s %-9s %2d WPM  %-58s expect %-7s got %-7s%s\n", ok ? "ok" : "FAIL",
           mode_name[g->mode], g->wpm, g->what, g->expect, got,
           timing_ok ? "" : "  (timing)");
    free(key);
  }
  printf("\n%d of %d cases as specified\n", ncases - fails, ncases);
  return fails ? 1 : 0;
}
