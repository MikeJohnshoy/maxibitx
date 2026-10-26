// codec_check_test.c
//
// Bench harness for codec_check.c (make test-codec-check): a thread plays
// the audio thread, feeding blocks whose right channel is noise - steady
// for a good codec, its level stepping through a 128-sample cycle for a
// bad one - and a stand-in for the interface restart that may make the
// codec good. Checked: the score, a clean start left alone, a bad start
// restarted until clean, giving up after CODEC_CHECK_TRIES, no measurement
// while transmitting, and a measurement started over after lost frames.

#include "codec_check.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    printf((cond) ? "  ok   " : "  FAIL ");                                                        \
    if (!(cond))                                                                                   \
      failures++;                                                                                  \
    printf(__VA_ARGS__);                                                                           \
    printf("\n");                                                                                  \
  } while (0)

/* ---- The codec, simulated ------------------------------------------------ */

static atomic_int bad;        // the right channel carries the 128-sample pattern
static atomic_int fixes_left; // restarts until the codec comes good (-1 never)
static atomic_int restarts;
static atomic_int usable = 1; // 0: transmitting
static atomic_int gap_every;  // frames between lost-frame events, or 0
static atomic_int feeding;

static int restart(void) {
  restarts++;
  int left = fixes_left;
  if (left > 0 && --fixes_left == 0)
    bad = 0;
  return 0;
}

static uint32_t seed = 12345;
static double noise(void) { // roughly Gaussian, unit variance
  double s = 0;
  for (int k = 0; k < 4; k++) {
    seed = seed * 1664525u + 1013904223u;
    s += (seed >> 8) / 16777216.0 - 0.5;
  }
  return s * sqrt(3.0);
}

static void *audio(void *arg) {
  (void)arg;
  int32_t b[1024 * 2];
  long n = 0, since_gap = 0;
  while (feeding) {
    for (int i = 0; i < 1024; i++, n++) {
      double g = bad ? 1 + 0.8 * cos(2 * M_PI * 5 * (n % 128) / 128.0) : 1;
      // a slow wave at the 1024-sample period as well, as the real line has
      double hum = 3e5 * sin(2 * M_PI * n / 1024.0);
      b[2 * i] = (int32_t)(1e6 * noise());
      b[2 * i + 1] = (int32_t)(1e6 * g * noise() + hum);
    }
    codec_check_block(b, 1024, 2, usable);
    since_gap += 1024;
    if (gap_every && since_gap >= gap_every) {
      codec_check_gap();
      since_gap = 0;
    }
    usleep(200);
  }
  return NULL;
}

// Runs the check against the simulated codec until it decides, or for at
// most max_ms. Returns the outcome.
static int run(int start_bad, int fixes, int max_ms) {
  bad = start_bad;
  fixes_left = fixes;
  restarts = 0;
  feeding = 1;
  pthread_t t;
  pthread_create(&t, NULL, audio, NULL);
  codec_check_start(restart);
  for (int ms = 0; ms < max_ms && codec_check_outcome() == CODEC_CHECK_RUNNING; ms += 10)
    usleep(10000);
  int r = codec_check_outcome();
  if (r != CODEC_CHECK_RUNNING) {
    feeding = 0;
    pthread_join(t, NULL);
  }
  return r;
}

static void stop(void) { // after a run() left undecided
  for (int ms = 0; ms < 5000 && codec_check_outcome() == CODEC_CHECK_RUNNING; ms += 10)
    usleep(10000);
  feeding = 0;
  usleep(50000);
}

int main(void) {
  printf("the score\n");
  double p[CODEC_CHECK_PERIOD];
  for (int k = 0; k < CODEC_CHECK_PERIOD; k++)
    p[k] = 1;
  CHECK(codec_check_score(p) < 1e-12, "power the same at every phase: 0 (%g)", codec_check_score(p));
  for (int k = 0; k < CODEC_CHECK_PERIOD; k++)
    p[k] = 1 + 0.6 * cos(2 * M_PI * CODEC_CHECK_HARMONIC * k / CODEC_CHECK_PERIOD + 1);
  CHECK(fabs(codec_check_score(p) - 0.6) < 1e-9, "a 5th harmonic 0.6 of the mean: 0.6 (%g)",
        codec_check_score(p));
  for (int k = 0; k < CODEC_CHECK_PERIOD; k++)
    p[k] = 1 + 0.6 * cos(2 * M_PI * 3 * k / CODEC_CHECK_PERIOD);
  CHECK(codec_check_score(p) < 1e-9, "other harmonics don't count (%g)", codec_check_score(p));

  printf("start-up\n");
  int r = run(0, 0, 5000);
  CHECK(r == CODEC_CHECK_CLEAN && restarts == 0, "a clean codec: clean, not restarted (%d)",
        (int)restarts);
  r = run(1, 1, 5000);
  CHECK(r == CODEC_CHECK_CLEAN && restarts == 1, "patterned, one restart fixes it: clean (%d)",
        (int)restarts);
  r = run(1, 2, 5000);
  CHECK(r == CODEC_CHECK_CLEAN && restarts == 2, "fixed by the second restart: clean (%d)",
        (int)restarts);
  r = run(1, -1, 8000);
  CHECK(r == CODEC_CHECK_BAD_STATE && restarts == CODEC_CHECK_TRIES,
        "never fixed: gives up after %d restarts (%d)", CODEC_CHECK_TRIES, (int)restarts);

  printf("transmitting and lost frames\n");
  usable = 0;
  r = run(1, 1, CODEC_CHECK_SETTLE_MS + 1000);
  CHECK(r == CODEC_CHECK_RUNNING && restarts == 0, "transmitting: nothing measured");
  usable = 1;
  stop();
  CHECK(codec_check_outcome() == CODEC_CHECK_CLEAN && restarts == 1,
        "back to receive: measured and restarted (%d)", (int)restarts);
  gap_every = CODEC_CHECK_FRAMES / 2;
  r = run(1, 1, CODEC_CHECK_SETTLE_MS + 1000);
  CHECK(r == CODEC_CHECK_RUNNING, "frames lost every %d: never a whole measurement",
        CODEC_CHECK_FRAMES / 2);
  gap_every = 0;
  stop();
  CHECK(codec_check_outcome() == CODEC_CHECK_CLEAN, "no more losses: measured");

  printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
  return failures ? 1 : 0;
}
