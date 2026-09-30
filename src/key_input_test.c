// key_input_test.c
//
// Bench harness for key_input.c's input logic: the steps its thread runs
// (key_input_arm(), key_input_edge(), key_input_tick()) fed synthetic edges
// and times, and key_input_take() placing the result in blocks. No GPIO -
// gpio.c is linked but never called.
//
//   make test-key-input && ./test-key-input
//
// Debounce: a clean press and release; bounce trains on both edges; a
// glitch shorter than the lockout, settled by a tick and by a later edge;
// debounce off. Mono plug: a grounded ring at start-up, a key held down at
// start-up, both contacts closed at start-up, a plug change. Also paddle
// reversal, the on_closed hook, re-arming with a contact closed, edge
// placement in a block (late, early, out of order), and queue overflow.

#include <stdint.h>
#include <stdio.h>

#include "key_input.h"

#define MS 1000000LL
#define US 1000LL
#define T0 (1000 * MS) // arbitrary start time; 0 is kept clear

static int failures = 0;
static void check(int ok, const char *what) {
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    failures++;
}

static int hook_calls = 0;
static void hook(void) { hook_calls++; }

// Everything queued, with n samples per ms from T0: offset = microseconds
// after T0 when n_per_ms is 1000.
#define MAX_EV 512
static struct key_event evs[MAX_EV];
static int take_all(int64_t t_end) {
  int total = 0, got;
  do {
    int64_t span_ms = (t_end - T0) / MS;
    got = key_input_take(T0, t_end, (int)(span_ms * 1000), evs + total, 64);
    total += got;
  } while (got == 64 && total + 64 <= MAX_EV);
  return total;
}

// Starts a case: empties the queue and arms with the given contacts closed.
static void start(unsigned closed_mask, int debounce_ms) {
  struct key_event drain[64];
  while (key_input_take(0, INT64_MAX / 4, 1, drain, 64) > 0) {
  }
  key_input_set_reverse(0);
  key_input_arm(closed_mask, T0, debounce_ms, hook);
  take_all(T0 + 1 * MS); // anything arming queued
  hook_calls = 0;
}

static int is_ev(int i, int contact, int closed, int offset_us) {
  return evs[i].contact == contact && evs[i].closed == closed && evs[i].offset == offset_us;
}

int main(void) {
  char msg[160];

  printf("A. clean press and release, 3 ms debounce\n");
  start(0, 3);
  key_input_edge(KEY_RING, 1, T0 + 10 * MS);
  key_input_tick(T0 + 13 * MS);
  key_input_edge(KEY_RING, 0, T0 + 30 * MS);
  key_input_tick(T0 + 40 * MS);
  int n = take_all(T0 + 50 * MS);
  check(n == 2 && is_ev(0, KEY_RING, 1, 10000) && is_ev(1, KEY_RING, 0, 30000),
        "closed at 10.000 ms, open at 30.000 ms");
  check(evs[0].paddle == KEY_DASH, "ring is the dash");
  check(hook_calls == 1, "on_closed called once");

  printf("B. bounce trains on press and release\n");
  start(0, 3);
  int64_t press[] = {10000, 10200, 10500, 10900, 11300}; // us, alternating closed/open
  for (int i = 0; i < 5; i++)
    key_input_edge(KEY_TIP, i % 2 == 0, T0 + press[i] * US);
  key_input_tick(T0 + 20 * MS);
  int64_t rel[] = {30000, 30100, 30400}; // open/closed/open
  for (int i = 0; i < 3; i++)
    key_input_edge(KEY_TIP, i % 2 == 1, T0 + rel[i] * US);
  key_input_tick(T0 + 40 * MS);
  n = take_all(T0 + 50 * MS);
  check(n == 2 && is_ev(0, KEY_TIP, 1, 10000) && is_ev(1, KEY_TIP, 0, 30000),
        "one closure at the first edge, one opening at the first edge");
  check(evs[0].paddle == KEY_DOT, "tip is the dot");
  check(hook_calls == 1, "on_closed called once");

  printf("C. glitch shorter than the lockout, settled by a tick\n");
  start(0, 3);
  key_input_edge(KEY_RING, 1, T0 + 10 * MS);
  key_input_edge(KEY_RING, 0, T0 + 10 * MS + 500 * US);
  check(key_input_next_due() == T0 + 13 * MS, "next_due is the lockout's end");
  key_input_tick(T0 + 13 * MS);
  n = take_all(T0 + 50 * MS);
  check(n == 2 && is_ev(0, KEY_RING, 1, 10000) && is_ev(1, KEY_RING, 0, 13000),
        "closed at 10 ms, open at the lockout's end, 13 ms");
  check(key_input_next_due() == T0 + 16 * MS, "taking the opening starts its own lockout");
  key_input_tick(T0 + 16 * MS);
  check(key_input_next_due() == 0, "nothing due once that ends");

  printf("D. glitch settled by a later edge, no tick\n");
  start(0, 3);
  key_input_edge(KEY_RING, 1, T0 + 10 * MS);
  key_input_edge(KEY_RING, 0, T0 + 10 * MS + 500 * US);
  key_input_edge(KEY_RING, 1, T0 + 20 * MS);
  n = take_all(T0 + 50 * MS);
  check(n == 3 && is_ev(0, KEY_RING, 1, 10000) && is_ev(1, KEY_RING, 0, 13000) &&
            is_ev(2, KEY_RING, 1, 20000),
        "closed 10, open 13, closed 20");

  printf("E. debounce off\n");
  start(0, 0);
  key_input_edge(KEY_RING, 1, T0 + 10 * MS);
  key_input_edge(KEY_RING, 0, T0 + 10 * MS + 500 * US);
  n = take_all(T0 + 50 * MS);
  check(n == 2 && is_ev(0, KEY_RING, 1, 10000) && is_ev(1, KEY_RING, 0, 10500),
        "both edges at their own times");

  printf("F. mono plug: ring grounded at start-up\n");
  start(1u << KEY_RING, 3);
  check(key_input_excluded() == (1u << KEY_RING), "ring excluded at once");
  key_input_edge(KEY_TIP, 1, T0 + 50 * MS);
  key_input_edge(KEY_TIP, 0, T0 + 70 * MS);
  key_input_tick(T0 + 251 * MS);
  check(key_input_excluded() == (1u << KEY_RING), "still excluded after the window");
  key_input_edge(KEY_RING, 0, T0 + 400 * MS); // plug pulled
  check(key_input_excluded() == 0, "ring opening clears the exclusion");
  key_input_edge(KEY_RING, 1, T0 + 500 * MS);
  key_input_tick(T0 + 600 * MS);
  n = take_all(T0 + 700 * MS);
  check(n == 3 && is_ev(0, KEY_TIP, 1, 50000) && is_ev(1, KEY_TIP, 0, 70000) &&
            is_ev(2, KEY_RING, 1, 500000),
        "tip keys; the grounded ring never reaches the queue; the ring works after");
  check(hook_calls == 2, "on_closed for the tip closure and the later ring closure only");

  printf("G. key held down at start-up\n");
  start(1u << KEY_RING, 3);
  key_input_edge(KEY_RING, 0, T0 + 100 * MS);
  check(key_input_excluded() == 0, "released inside the window: not a plug");
  key_input_edge(KEY_RING, 1, T0 + 200 * MS);
  key_input_tick(T0 + 400 * MS);
  n = take_all(T0 + 500 * MS);
  check(n == 1 && is_ev(0, KEY_RING, 1, 200000),
        "the held closure is not replayed; the next one is");

  printf("H. both contacts closed at start-up\n");
  start((1u << KEY_RING) | (1u << KEY_TIP), 3);
  check(key_input_excluded() == 3, "both excluded");
  key_input_edge(KEY_TIP, 0, T0 + 50 * MS);
  check(key_input_excluded() == (1u << KEY_RING), "tip opens: tip in use, ring timed");
  key_input_edge(KEY_TIP, 1, T0 + 80 * MS);
  key_input_edge(KEY_TIP, 0, T0 + 120 * MS);
  key_input_tick(T0 + 301 * MS);
  check(key_input_excluded() == (1u << KEY_RING), "ring confirmed as grounded");
  n = take_all(T0 + 400 * MS);
  check(n == 2 && is_ev(0, KEY_TIP, 1, 80000) && is_ev(1, KEY_TIP, 0, 120000),
        "tip keys normally");

  printf("I. paddle reversal\n");
  start(0, 3);
  key_input_set_reverse(1);
  key_input_edge(KEY_TIP, 1, T0 + 10 * MS);
  key_input_edge(KEY_RING, 1, T0 + 20 * MS);
  n = take_all(T0 + 50 * MS);
  check(n == 2 && evs[0].paddle == KEY_DASH && evs[1].paddle == KEY_DOT,
        "reversed: tip = dash, ring = dot");
  check(key_input_get_reverse() == 1, "reads back");
  key_input_set_reverse(0);
  key_input_edge(KEY_TIP, 0, T0 + 30 * MS);
  key_input_edge(KEY_RING, 0, T0 + 40 * MS);

  printf("J. re-arming with a contact closed\n");
  start(0, 3);
  key_input_edge(KEY_TIP, 1, T0 + 10 * MS);
  take_all(T0 + 20 * MS);
  key_input_arm(1u << KEY_TIP, T0 + 30 * MS, 3, hook);
  n = take_all(T0 + 40 * MS);
  check(n == 1 && is_ev(0, KEY_TIP, 0, 30000) && key_input_excluded() == (1u << KEY_TIP),
        "the closed contact is excluded and the audio thread told it opened");
  key_input_edge(KEY_TIP, 0, T0 + 50 * MS);

  printf("K. placement in a block\n");
  start(0, 0);
  key_input_edge(KEY_TIP, 1, T0 + 5 * MS);
  key_input_edge(KEY_TIP, 0, T0 + 15 * MS);
  key_input_edge(KEY_TIP, 1, T0 + 20 * MS);
  key_input_edge(KEY_RING, 1, T0 + 25 * MS);
  key_input_edge(KEY_RING, 0, T0 + 22 * MS); // queued after the 25 ms edge
  struct key_event b[8];
  int m = key_input_take(T0 + 10 * MS, T0 + 20 * MS, 1024, b, 8);
  snprintf(msg, sizeof msg, "block 10-20 ms: late edge at 0, mid-block edge at 512 (got %d: %d, %d)",
           m, m > 0 ? b[0].offset : -1, m > 1 ? b[1].offset : -1);
  check(m == 2 && b[0].offset == 0 && b[1].offset == 512, msg);
  check(key_input_pending(), "the edge at exactly 20 ms waits for the next block");
  m = key_input_take(T0 + 20 * MS, T0 + 30 * MS, 1024, b, 8);
  snprintf(msg, sizeof msg,
           "block 20-30 ms: 0, 512, and the out-of-order 22 ms edge held at 512 (got %d: %d %d %d)",
           m, m > 0 ? b[0].offset : -1, m > 1 ? b[1].offset : -1, m > 2 ? b[2].offset : -1);
  check(m == 3 && b[0].offset == 0 && b[1].offset == 512 && b[2].offset == 512, msg);
  check(!key_input_pending(), "queue empty");
  key_input_edge(KEY_TIP, 0, T0 + 40 * MS);

  printf("L. queue overflow\n");
  start(0, 0);
  for (int i = 0; i < 301; i++) // ends closed; the queue holds 256
    key_input_edge(KEY_TIP, i % 2 == 0, T0 + (10 + i) * MS);
  n = take_all(T0 + 400 * MS);
  snprintf(msg, sizeof msg, "%d edges delivered, the last a resync to closed", n);
  check(n == 257 && evs[n - 1].contact == KEY_TIP && evs[n - 1].closed == 1, msg);

  printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
