// encoder_test.c
//
// Bench harness for the front-panel knobs: encoder.c's decoding fed
// synthetic edges through the steps its thread runs (encoder_arm(),
// encoder_edge(), encoder_tick()), and knobs.c's handlers with the radio
// and the speaker stubbed. No GPIO - gpio.c is linked but never called.
//
//   make test-encoder && ./test-encoder
//
// Turns: clicks each way on a four-edge and a two-edge encoder, contact
// bounce on every edge, most of a click and back, a repeated level, arming
// partway round. Presses:
// bounce on press and release, a switch closed at start-up, a re-press
// inside the lockout. Knobs: the tuning step and its range, the speed-up
// and where it stops, a push stepping through 10 Hz to 10 kHz, the dial's
// limits, no tuning while transmitting; volume clicks, mute on a push and
// unmute on a turn; one console line per knob once it has settled.

#include <stdint.h>
#include <stdio.h>

#include "encoder.h"
#include "knobs.h"
#include "radio.h"
#include "rx_audio.h"

#define MS 1000000LL
#define T0 (1000 * MS)

static int failures = 0;
static void check(int ok, const char *what) {
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    failures++;
}

/* ---- Stubs: the radio and the speaker ------------------------------------- */

int freq_hdr = 14060000;
int in_tx = 0;
static int tunes = 0;
void radio_tune_to(uint32_t f) {
  freq_hdr = (int)f;
  tunes++;
}
static int volume = 50, muted = 0;
void rx_audio_set_volume(int percent) {
  volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
}
int rx_audio_get_volume(void) { return volume; }
void rx_audio_set_mute(int on) { muted = on != 0; }
int rx_audio_get_mute(void) { return muted; }

// encoder_start() is never called; this only satisfies the link.
struct radio_hw_knob radio_hw_knob(enum radio_knob k) {
  (void)k;
  return (struct radio_hw_knob){-1, -1, -1, 0};
}

/* ---- Recording what encoder.c reports ------------------------------------- */

static int clicks[RADIO_KNOBS], pushes[RADIO_KNOBS];
static void on_turn(enum radio_knob k, int detents, int64_t ts) {
  (void)ts;
  clicks[k] += detents;
}
static void on_push(enum radio_knob k) { pushes[k]++; }

static int64_t now;
static int a_lv, b_lv;

static void start(int edges_per_detent) {
  encoder_set_hooks(on_turn, on_push);
  encoder_arm(RADIO_KNOB_TUNING, edges_per_detent, 1, 1, 1);
  encoder_arm(RADIO_KNOB_VOLUME, 4, 1, 1, 1);
  for (int k = 0; k < RADIO_KNOBS; k++)
    clicks[k] = pushes[k] = 0;
  a_lv = b_lv = 1;
  now = T0;
}

// One contact goes to level, with bounce chatters first.
static void edge(enum encoder_line line, int level, int bounce) {
  for (int i = 0; i < bounce; i++) {
    encoder_edge(RADIO_KNOB_TUNING, line, level, now);
    now += 50000;
    encoder_edge(RADIO_KNOB_TUNING, line, !level, now);
    now += 50000;
  }
  encoder_edge(RADIO_KNOB_TUNING, line, level, now);
  if (line == ENCODER_A)
    a_lv = level;
  else
    b_lv = level;
  now += 2 * MS;
}

// A whole click on a four-edge encoder: up is A then B falling, A then B
// rising (11 01 00 10 11).
static void click(int up, int bounce) {
  enum encoder_line first = up ? ENCODER_A : ENCODER_B, second = up ? ENCODER_B : ENCODER_A;
  edge(first, 0, bounce);
  edge(second, 0, bounce);
  edge(first, 1, bounce);
  edge(second, 1, bounce);
}

static void press(enum radio_knob k, int closed, int bounce) {
  for (int i = 0; i < bounce; i++) {
    encoder_edge(k, ENCODER_SW, !closed, now);
    now += 100000;
    encoder_edge(k, ENCODER_SW, closed, now);
    now += 100000;
  }
  encoder_edge(k, ENCODER_SW, !closed, now);
}

static void wait_ms(int ms) {
  now += ms * MS;
  encoder_tick(now);
}

int main(void) {
  printf("Turns\n");
  start(4);
  for (int i = 0; i < 3; i++)
    click(1, 0);
  check(clicks[RADIO_KNOB_TUNING] == 3, "three clean clicks up count +3");
  for (int i = 0; i < 5; i++)
    click(0, 0);
  check(clicks[RADIO_KNOB_TUNING] == -2, "five clicks down bring it to -2");

  start(4);
  click(1, 3);
  click(1, 5);
  click(0, 4);
  check(clicks[RADIO_KNOB_TUNING] == 1, "bounce on every edge: up, up, down count +1");

  start(4);
  edge(ENCODER_A, 0, 0);
  edge(ENCODER_B, 0, 0);
  edge(ENCODER_A, 1, 0); // three edges up ...
  edge(ENCODER_A, 0, 0);
  edge(ENCODER_B, 1, 0);
  edge(ENCODER_A, 1, 0); // ... and back the way it came
  check(clicks[RADIO_KNOB_TUNING] == 0, "most of a click and back again counts nothing");

  start(4);
  edge(ENCODER_A, 0, 0);
  encoder_edge(RADIO_KNOB_TUNING, ENCODER_A, 0, now); // the level it already has
  edge(ENCODER_B, 0, 0);
  encoder_edge(RADIO_KNOB_TUNING, ENCODER_B, 0, now);
  edge(ENCODER_A, 1, 0);
  edge(ENCODER_B, 1, 0);
  check(clicks[RADIO_KNOB_TUNING] == 1, "an edge repeating a line's level counts nothing");

  start(2);
  edge(ENCODER_A, 0, 2);
  edge(ENCODER_B, 0, 2); // rests with both closed: one click
  edge(ENCODER_A, 1, 2);
  edge(ENCODER_B, 1, 2); // and both open: another
  check(clicks[RADIO_KNOB_TUNING] == 2, "a two-edge encoder counts a click at each rest");
  edge(ENCODER_B, 0, 0);
  edge(ENCODER_A, 0, 0);
  check(clicks[RADIO_KNOB_TUNING] == 1, "... and back down one");

  start(4);
  encoder_arm(RADIO_KNOB_TUNING, 4, 0, 1, 1); // resting partway, A closed
  edge(ENCODER_B, 0, 0);
  edge(ENCODER_A, 1, 0);
  edge(ENCODER_B, 1, 0);
  check(clicks[RADIO_KNOB_TUNING] == 1, "armed partway round, the rest of a click counts");

  printf("Presses\n");
  start(4);
  press(RADIO_KNOB_VOLUME, 1, 4);
  check(pushes[RADIO_KNOB_VOLUME] == 1 && pushes[RADIO_KNOB_TUNING] == 0,
        "a bouncing press is one press, on the volume knob only");
  wait_ms(100);
  press(RADIO_KNOB_VOLUME, 0, 4);
  wait_ms(100);
  check(pushes[RADIO_KNOB_VOLUME] == 1, "its bouncing release is no press");
  press(RADIO_KNOB_VOLUME, 1, 0);
  now += 5 * MS;
  press(RADIO_KNOB_VOLUME, 0, 0);
  now += 5 * MS;
  press(RADIO_KNOB_VOLUME, 1, 0); // released and pressed again inside the lockout
  check(pushes[RADIO_KNOB_VOLUME] == 2, "a press, then a re-press inside the lockout ...");
  check(encoder_next_due() != 0, "... leaves a lockout to end");
  wait_ms(30);
  check(pushes[RADIO_KNOB_VOLUME] == 2, "... which finds it still closed: no extra press");
  press(RADIO_KNOB_VOLUME, 0, 0);
  wait_ms(30);
  press(RADIO_KNOB_VOLUME, 1, 0);
  check(pushes[RADIO_KNOB_VOLUME] == 3, "released and pressed after it: a press");
  wait_ms(30);
  press(RADIO_KNOB_VOLUME, 0, 0);
  wait_ms(30);
  check(encoder_next_due() == 0, "nothing due once settled");

  encoder_arm(RADIO_KNOB_TUNING, 4, 1, 1, 0); // switch held at start-up
  pushes[RADIO_KNOB_TUNING] = 0;
  wait_ms(30);
  check(pushes[RADIO_KNOB_TUNING] == 0, "a switch closed at start-up is no press");
  press(RADIO_KNOB_TUNING, 0, 0);
  wait_ms(30);
  press(RADIO_KNOB_TUNING, 1, 0);
  check(pushes[RADIO_KNOB_TUNING] == 1, "... until it opens and closes");

  printf("Tuning knob\n");
  check(knobs_get_step() == KNOBS_STEP_DEFAULT_HZ, "the step starts at 10 Hz");
  check(knobs_set_step(0) == 1 && knobs_set_step(50000000) == KNOBS_STEP_MAX_HZ,
        "the step is kept within 1 Hz to 10 MHz");
  knobs_set_step(10);
  freq_hdr = 14060000;
  int64_t t = T0;
  for (int i = 0; i < 3; i++)
    knobs_turn(RADIO_KNOB_TUNING, 1, t += 100 * MS);
  check(freq_hdr == 14060030, "three slow clicks up: +30 Hz");
  knobs_turn(RADIO_KNOB_TUNING, -1, t += 5 * MS);
  check(freq_hdr == 14060020, "a quick click the other way is not sped up: -10 Hz");
  knobs_turn(RADIO_KNOB_TUNING, -1, t += 30 * MS);
  knobs_turn(RADIO_KNOB_TUNING, -1, t += 15 * MS);
  knobs_turn(RADIO_KNOB_TUNING, -1, t += 5 * MS);
  check(freq_hdr == 14060020 - 20 - 50 - 100, "faster clicks one way: x2, x5, x10");
  knobs_set_step(1000);
  int before = freq_hdr;
  knobs_turn(RADIO_KNOB_TUNING, 1, t += 100 * MS);
  knobs_turn(RADIO_KNOB_TUNING, 1, t += 5 * MS);
  check(freq_hdr == before + 2000, "no speed-up at 1 kHz and above");
  in_tx = 1;
  tunes = 0;
  knobs_turn(RADIO_KNOB_TUNING, 1, t += 100 * MS);
  check(tunes == 0 && freq_hdr == before + 2000, "nothing while transmitting");
  in_tx = 0;
  freq_hdr = KNOBS_FREQ_MAX_HZ - 500;
  knobs_turn(RADIO_KNOB_TUNING, 1, t += 100 * MS);
  check(freq_hdr == KNOBS_FREQ_MAX_HZ, "stops at 30 MHz");
  tunes = 0;
  knobs_turn(RADIO_KNOB_TUNING, 1, t += 100 * MS);
  check(tunes == 0, "... and doesn't retune once there");

  knobs_set_step(10);
  int seen[5];
  for (int i = 0; i < 5; i++) {
    knobs_push(RADIO_KNOB_TUNING);
    seen[i] = knobs_get_step();
  }
  check(seen[0] == 100 && seen[1] == 1000 && seen[2] == 10000 && seen[3] == 10 && seen[4] == 100,
        "pushes: 10 Hz -> 100 Hz -> 1 kHz -> 10 kHz -> 10 Hz -> 100 Hz");
  knobs_set_step(1);
  knobs_push(RADIO_KNOB_TUNING);
  check(knobs_get_step() == 10, "from 1 Hz a push goes to 10 Hz");
  knobs_set_step(100000);
  knobs_push(RADIO_KNOB_TUNING);
  check(knobs_get_step() == 10, "from 100 kHz a push goes to 10 Hz");

  printf("Volume knob\n");
  volume = 50;
  knobs_turn(RADIO_KNOB_VOLUME, 1, t += 5 * MS);
  knobs_turn(RADIO_KNOB_VOLUME, 1, t += 5 * MS);
  check(volume == 54, "two clicks up: +4%, no speed-up");
  knobs_push(RADIO_KNOB_VOLUME);
  check(muted && volume == 54, "a push mutes, the volume kept");
  knobs_push(RADIO_KNOB_VOLUME);
  check(!muted, "another unmutes");
  knobs_push(RADIO_KNOB_VOLUME);
  knobs_turn(RADIO_KNOB_VOLUME, -1, t += 100 * MS);
  check(!muted && volume == 52, "turning unmutes, and takes the click");
  for (int i = 0; i < 40; i++)
    knobs_turn(RADIO_KNOB_VOLUME, 1, t += 100 * MS);
  check(volume == 100, "stops at 100%");

  printf("Settled lines\n");
  knobs_log_settled(t + 10000 * MS); // clear what the cases above left
  knobs_set_step(10);
  freq_hdr = 14060000;
  for (int i = 0; i < 5; i++)
    knobs_turn(RADIO_KNOB_TUNING, 1, t += 100 * MS);
  check(knobs_log_settled(t + 200 * MS) == 0, "nothing while the knob is still turning");
  check(knobs_log_settled(t + 600 * MS) == 1, "one line once it has been still 500 ms");
  check(knobs_log_settled(t + 2000 * MS) == 0, "... and only one");
  knobs_turn(RADIO_KNOB_VOLUME, 1, t += 100 * MS);
  knobs_turn(RADIO_KNOB_TUNING, -1, t += 100 * MS);
  check(knobs_log_settled(t + 600 * MS) == 2, "both knobs turned: a line each");
  in_tx = 1;
  knobs_turn(RADIO_KNOB_TUNING, 1, t += 100 * MS);
  in_tx = 0;
  check(knobs_log_settled(t + 600 * MS) == 0, "a click that changed nothing (in TX): no line");

  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
