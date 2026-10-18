// cw_spot_test.c
//
// Bench harness for cw_spot.c (make test-cw-spot): 1.5 s of audio at
// CW_SPOT_RATE, a tone (steady or keyed with Morse at a given speed, 5 ms
// edges) plus white noise at an SNR in 2500 Hz, searched as rx_audio.c
// does, within 150 Hz of a 700 Hz pitch unless stated.

#include "cw_spot.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define N CW_SPOT_SAMPLES
#define FS CW_SPOT_RATE

static uint64_t rng = 12345;
static double uniform(void) {
  rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
  return ((rng >> 11) + 0.5) / 9007199254740992.0;
}
static double gauss(void) {
  return sqrt(-2.0 * log(uniform())) * cos(2.0 * M_PI * uniform());
}

static float audio[N];

// Keying envelope: "CQ DE KB2ML K" over and over from a random point, at
// wpm (0: a steady carrier), with 5 ms raised-cosine edges.
static void envelope(double wpm, double *env) {
  static const char *const code = "-.-. --.- / -.. . / -.- -... ..--- -- .-.. / -.- / ";
  if (wpm <= 0) {
    for (int i = 0; i < N; i++)
      env[i] = 1.0;
    return;
  }
  double unit = 1.2 / wpm * FS;
  static double key[4 * N];
  int len = 0;
  while (len < 4 * N) {
    for (const char *c = code; *c && len < 4 * N; c++) {
      int on = *c == '.' || *c == '-';
      int units = *c == '-' ? 3 : *c == '.' ? 1 : *c == ' ' ? 2 : 4;
      for (int i = 0; i < (int)(units * unit) && len < 4 * N; i++)
        key[len++] = on;
      for (int i = 0; on && i < (int)unit && len < 4 * N; i++)
        key[len++] = 0;
    }
  }
  int start = (int)(uniform() * 2 * N);
  int rise = FS / 200;
  double level = 0;
  for (int i = 0; i < N; i++) {
    double want = key[start + i];
    if (level < want)
      level = fmin(want, level + 1.0 / rise);
    else if (level > want)
      level = fmax(want, level - 1.0 / rise);
    env[i] = 0.5 - 0.5 * cos(M_PI * level);
  }
}

static void make(double tone_hz, double amp, double wpm, double snr_db, int fresh) {
  static double env[N];
  if (fresh)
    memset(audio, 0, sizeof(audio));
  if (amp > 0) {
    envelope(wpm, env);
    double ph = uniform() * 2 * M_PI;
    for (int i = 0; i < N; i++)
      audio[i] += (float)(amp * env[i] * sin(2 * M_PI * tone_hz * i / FS + ph));
  }
  if (snr_db < 99) {
    // a tone of amplitude 1 against noise in 2500 Hz; the noise spans 3 kHz
    double sigma = sqrt(0.5 / pow(10, snr_db / 10) * (FS / 2.0) / 2500.0);
    for (int i = 0; i < N; i++)
      audio[i] += (float)(sigma * gauss());
  }
}

static int failures;
static void check(int ok, const char *what) {
  printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok)
    failures++;
}

static void one(const char *name, double tone_hz, double wpm, double snr_db, double max_err) {
  make(tone_hz, 1.0, wpm, snr_db, 1);
  double f = 0;
  int found = cw_spot_find(audio, N, 550, 850, &f);
  char what[160];
  snprintf(what, sizeof(what), "%-36s %s %.2f Hz, error %+.2f Hz (limit %.1f)", name,
           found ? "found" : "NOT found", found ? f : 0.0, found ? f - tone_hz : 0.0, max_err);
  check(found && fabs(f - tone_hz) <= max_err, what);
}

int main(void) {
  printf("cw_spot: a 700 Hz pitch, searched 550-850 Hz; SNR in 2500 Hz\n");
  one("steady tone, 20 dB", 737.3, 0, 20, 0.3);
  one("keyed 20 WPM, 10 dB", 616.4, 20, 10, 1.0);
  one("keyed 12 WPM, 10 dB", 683.2, 12, 10, 1.0);
  one("keyed 35 WPM, 10 dB", 761.7, 35, 10, 1.0);
  one("keyed 50 WPM, 10 dB", 702.9, 50, 10, 1.0);
  one("keyed 20 WPM, 0 dB", 655.5, 20, 0, 1.5);
  one("keyed 20 WPM, -6 dB", 744.1, 20, -6, 2.0);

  // across the band
  double worst = 0;
  int missed = 0;
  for (double off = -140; off <= 140; off += 7.3) {
    make(700 + off, 1.0, 20, 10, 1);
    double f;
    if (!cw_spot_find(audio, N, 550, 850, &f))
      missed++;
    else if (fabs(f - 700 - off) > worst)
      worst = fabs(f - 700 - off);
  }
  char what[160];
  snprintf(what, sizeof(what), "keyed 20 WPM, 10 dB, -140 to +140 Hz: worst error %.2f Hz, %d missed",
           worst, missed);
  check(worst <= 1.0 && missed == 0, what);

  // the stronger of two
  make(740, 1.0, 20, 15, 1);
  make(640, 0.3, 25, 99, 0);
  double f = 0;
  int found = cw_spot_find(audio, N, 550, 850, &f);
  snprintf(what, sizeof(what), "two stations (740 Hz and 640 Hz 10 dB down): found %.1f Hz", f);
  check(found && fabs(f - 740) < 1.0, what);

  // a strong station outside a 150 Hz filter's band, alone and beside a
  // weak one in it: nothing, rather than a move onto its sidebands
  for (double out = 830; out <= 920; out += 30) {
    make(out, 10.0, 20, 10, 1);
    found = cw_spot_find(audio, N, 625, 775, &f);
    snprintf(what, sizeof(what), "a strong station %.0f Hz outside the band: nothing found",
             out - 775);
    check(!found, what);
  }
  make(830, 10.0, 20, 10, 1);
  make(710, 0.3, 25, 99, 0);
  found = cw_spot_find(audio, N, 625, 775, &f);
  check(!found, "a weak station in the band beside a strong one 55 Hz outside: nothing found");
  // with the strong one well away from the band, the weak one is found
  make(1050, 10.0, 20, 10, 1);
  make(710, 0.3, 25, 99, 0);
  found = cw_spot_find(audio, N, 625, 775, &f);
  snprintf(what, sizeof(what), "the weak one with the strong one 275 Hz outside: found %.1f Hz", f);
  check(found && fabs(f - 710) < 1.0, what);

  // noise alone
  int false_finds = 0;
  for (int t = 0; t < 200; t++) {
    make(0, 0, 0, 0, 1);
    if (cw_spot_find(audio, N, 550, 850, &f))
      false_finds++;
  }
  snprintf(what, sizeof(what), "noise alone, 200 tries: %d found", false_finds);
  check(false_finds == 0, what);

  // cost: the widest search
  make(700, 1.0, 20, 10, 1);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int r = 0; r < 20; r++)
    cw_spot_find(audio, N, 550, 850, &f);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double ms = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 20 / 1e6;
  printf("  cost: %.1f ms per search across 300 Hz, 600 with the guard (an interface thread)\n", ms);

  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
