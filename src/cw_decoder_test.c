// cw_decoder_test.c
//
// Bench harness for cw_decoder.c (make test-cw-decoder): keys text into a
// 700 Hz tone with 5 ms raised-cosine edges, adds white noise, feeds the
// decoder 1024 samples at a time as rx_audio.c does, and scores the text by
// edit distance (character error rate, spaces ignored). The fists are
// simple: element and gap lengths spread by a lognormal of the given
// relative size. tools/cw_study/cw_bench.py is the full bench.

#include "cw_decoder.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FS 96000
#define PITCH 700.0
#define MAX_S 240                   // seconds of audio at most

static const char *const morse[128] = {
    ['A'] = ".-",   ['B'] = "-...", ['C'] = "-.-.", ['D'] = "-..",  ['E'] = ".",
    ['F'] = "..-.", ['G'] = "--.",  ['H'] = "....", ['I'] = "..",   ['J'] = ".---",
    ['K'] = "-.-",  ['L'] = ".-..", ['M'] = "--",   ['N'] = "-.",   ['O'] = "---",
    ['P'] = ".--.", ['Q'] = "--.-", ['R'] = ".-.",  ['S'] = "...",  ['T'] = "-",
    ['U'] = "..-",  ['V'] = "...-", ['W'] = ".--",  ['X'] = "-..-", ['Y'] = "-.--",
    ['Z'] = "--..", ['0'] = "-----", ['1'] = ".----", ['2'] = "..---", ['3'] = "...--",
    ['4'] = "....-", ['5'] = ".....", ['6'] = "-....", ['7'] = "--...", ['8'] = "---..",
    ['9'] = "----.", ['/'] = "-..-.", ['?'] = "..--..",
};

static const char *const SHORT_TEXT = "CQ POTA DE KB2ML K UR 579 NY BK";
static const char *const TEXT =
    "CQ CQ POTA DE KB2ML KB2ML K W1AW DE KB2ML GM TNX CALL UR 579 579 IN NY "
    "NAME MIKE HW? R TU UR 559 BK N0POT/P DE KB2ML TU 5NN K1234 K "
    "RIG SBITX ANT DIPOLE PWR 5W WX SUNNY 73 ES GL";

static uint64_t rng = 12345;
static double uniform(void) {
  rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
  return ((rng >> 11) + 0.5) / 9007199254740992.0;
}
static double gauss(void) {
  return sqrt(-2.0 * log(uniform())) * cos(2.0 * M_PI * uniform());
}

// One element or gap of `units` dots, spread by sd (relative).
static double spread(double units, double sd) {
  return units * exp(sd * gauss() - sd * sd / 2);
}

static float *audio;
static long n_audio;

static void put_tone(double secs, int on) {
  long n = (long)(secs * FS);
  long rise = (long)(0.005 * FS);
  for (long i = 0; i < n && n_audio < (long)MAX_S * FS; i++, n_audio++) {
    double env = 0;
    if (on) {
      env = 1;
      if (i < rise)
        env = 0.5 - 0.5 * cos(M_PI * i / rise);
      if (n - i < rise)
        env = 0.5 - 0.5 * cos(M_PI * (n - i) / rise);
    }
    audio[n_audio] += (float)(env * sin(2 * M_PI * PITCH * n_audio / FS));
  }
}

// Key text at wpm (character speed farns, if larger: Farnsworth spacing),
// with spread sd, after lead seconds of silence.
static void key(const char *text, double wpm, double farns, double sd, double lead) {
  double u = 1.2 / (farns > wpm ? farns : wpm);
  double k = 1.0;
  if (farns > wpm) {
    double ta = (60.0 * farns - 37.2 * wpm) / (wpm * farns);
    k = ta / 19.0 / u;
  }
  put_tone(lead, 0);
  for (const char *c = text; *c; c++) {
    if (*c == ' ') {
      put_tone(spread(7 * k, sd) * u - 3 * k * u, 0);   // a word gap less the char gap given
      continue;
    }
    const char *p = morse[(int)*c];
    for (int e = 0; p[e]; e++) {
      // put_tone's edges fall inside the tone: lengthen it by one edge so
      // its half-amplitude points are the element's length apart
      put_tone(spread(p[e] == '.' ? 1 : 3, sd) * u + 0.005, 1);
      put_tone(spread(1, sd) * u - 0.005, 0);
    }
    put_tone(spread(3 * k, sd) * u - u, 0);           // the rest of the character gap
  }
}

static void add_noise(double snr_db) {
  // tone peak 1; SNR in 2500 Hz
  double sigma = sqrt(0.5 / pow(10, snr_db / 10) * (FS / 2.0) / 2500.0);
  for (long i = 0; i < n_audio; i++)
    audio[i] += (float)(sigma * gauss());
}

static char decoded[8192];
static int learned_wpm;

static void decode(void) {
  char buf[CW_DECODER_TEXT_KEEP + 1];
  size_t used = 0;
  decoded[0] = 0;
  learned_wpm = 0;
  cw_decoder_init();
  uint32_t seq = cw_decoder_read(UINT32_MAX, buf, 1);  // from now on
  for (long i = 0; i + 1024 <= n_audio + 2 * FS; i += 1024) {
    static float block[1024];
    for (int k = 0; k < 1024; k++)
      block[k] = (i + k < n_audio) ? audio[i + k] : 0.0f;
    cw_decoder_process(block, 1024, (int)PITCH);
    if (cw_decoder_get_wpm())
      learned_wpm = cw_decoder_get_wpm();
    seq = cw_decoder_read(seq, buf, sizeof(buf));
    size_t l = strlen(buf);
    if (used + l < sizeof(decoded)) {
      memcpy(decoded + used, buf, l + 1);
      used += l;
    }
  }
}

static int edit_distance(const char *a, const char *b) {
  int la = (int)strlen(a), lb = (int)strlen(b);
  int *prev = malloc((lb + 1) * sizeof(int)), *cur = malloc((lb + 1) * sizeof(int));
  for (int j = 0; j <= lb; j++)
    prev[j] = j;
  for (int i = 1; i <= la; i++) {
    cur[0] = i;
    for (int j = 1; j <= lb; j++) {
      int c = prev[j - 1] + (a[i - 1] != b[j - 1]);
      if (prev[j] + 1 < c) c = prev[j] + 1;
      if (cur[j - 1] + 1 < c) c = cur[j - 1] + 1;
      cur[j] = c;
    }
    int *t = prev; prev = cur; cur = t;
  }
  int r = prev[lb];
  free(prev);
  free(cur);
  return r;
}

static void squeeze(const char *in, char *out) {   // no spaces or newlines
  for (; *in; in++)
    if (*in != ' ' && *in != '\n')
      *out++ = *in;
  *out = 0;
}

static double cer(const char *ref, const char *hyp) {
  static char r[8192], h[8192];
  squeeze(ref, r);
  squeeze(hyp, h);
  return (double)edit_distance(r, h) / (double)strlen(r);
}

static int failures;

static void check(int ok, const char *what) {
  printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok)
    failures++;
}

static void run_text(const char *name, const char *text, double wpm, double farns, double sd,
                     double snr, double max_cer) {
  memset(audio, 0, sizeof(float) * (size_t)MAX_S * FS);
  n_audio = 0;
  key(text, wpm, farns, sd, 1.0);
  put_tone(2.0, 0);
  add_noise(snr);
  decode();
  double c = cer(text, decoded);
  char what[160];
  snprintf(what, sizeof(what), "%-34s CER %5.1f%% (limit %.0f%%), %d WPM learned",
           name, 100 * c, 100 * max_cer, learned_wpm);
  check(c <= max_cer, what);
  if (c > max_cer) {
    printf("        sent: %s\n", text);
    printf("        got:  %s\n", decoded);
  }
}

static void run(const char *name, double wpm, double farns, double sd, double snr, double max_cer) {
  run_text(name, TEXT, wpm, farns, sd, snr, max_cer);
}

int main(void) {
  audio = calloc((size_t)MAX_S * FS, sizeof(float));
  printf("cw_decoder: 15 dB SNR in 2500 Hz unless stated\n");
  run("machine, 20 WPM", 20, 0, 0.0, 15, 0.0);
  run("machine, 35 WPM", 35, 0, 0.0, 15, 0.01);
  run_text("machine, 5 WPM", SHORT_TEXT, 5, 0, 0.0, 15, 0.0);
  run_text("Farnsworth 5/18, spread 10%", SHORT_TEXT, 5, 18, 0.10, 15, 0.0);
  run("Farnsworth 10/18, spread 10%", 10, 18, 0.10, 15, 0.02);
  run("hand-sent, 12 WPM, spread 12%", 12, 0, 0.12, 15, 0.03);
  run("hand-sent, 25 WPM, spread 12%", 25, 0, 0.12, 15, 0.03);
  run("hand-sent, 35 WPM, spread 12%", 35, 0, 0.12, 15, 0.03);
  run("machine, 20 WPM, 6 dB", 20, 0, 0.0, 6, 0.02);

  // noise only: no text
  memset(audio, 0, sizeof(float) * (size_t)MAX_S * FS);
  n_audio = 0;
  put_tone(30.0, 0);
  add_noise(15);
  decode();
  char none[64];
  squeeze(decoded, none);
  check(strlen(none) == 0, "30 s of noise alone: no text");

  // a second station at another speed, after a pause
  memset(audio, 0, sizeof(float) * (size_t)MAX_S * FS);
  n_audio = 0;
  key("CQ CQ DE W1AW W1AW K", 30, 0, 0.08, 1.0);
  put_tone(3.0, 0);
  key("W1AW DE KB2ML KB2ML UR 599 BK", 12, 0, 0.08, 0.0);
  put_tone(2.0, 0);
  add_noise(15);
  decode();
  double c = cer("CQ CQ DE W1AW W1AW K W1AW DE KB2ML KB2ML UR 599 BK", decoded);
  char what[120];
  snprintf(what, sizeof(what), "30 WPM, a pause, then 12 WPM: CER %.1f%%", 100 * c);
  check(c <= 0.03 && strchr(decoded, '\n') != NULL, what);

  // reading: the sequence numbers
  char buf[16];
  uint32_t end = cw_decoder_read(UINT32_MAX, buf, sizeof(buf));
  check(buf[0] == 0, "read: a number from the future gives nothing, and the end");
  uint32_t s = cw_decoder_read(end - 20, buf, sizeof(buf));
  check(strlen(buf) == 15 && s == end - 5, "read: a short buffer gets the oldest asked for");
  s = cw_decoder_read(end - CW_DECODER_TEXT_KEEP - 50, buf, sizeof(buf));
  check(end > CW_DECODER_TEXT_KEEP + 50 && s == end - CW_DECODER_TEXT_KEEP + 15,
        "read: too old starts at the oldest kept");

  // cost
  struct timespec t0, t1;
  float block[1024];
  for (int k = 0; k < 1024; k++)
    block[k] = audio[k + 2 * FS];
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int r = 0; r < 2000; r++)
    cw_decoder_process(block, 1024, (int)PITCH);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 2000 / 1000.0;
  printf("  cost: %.1f us per 1024-sample block (%.2f%% of the 10.67 ms budget)\n", us, us / 106.67);

  free(audio);
  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
