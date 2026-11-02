// cw_shape_test.c
//
// Bench check of the transmitted CW's keying sidebands against ARRL's Clean
// Signal Initiative mask, as DL1YCF reads it (the mask's 180, 300 and
// 675 Hz are bandwidths, so these are offsets from the carrier):
//
//   -20 dBc beyond 90 Hz, -40 dBc beyond 150 Hz, -60 dBc beyond 338 Hz
//
// The signal is the radio's own: a dot paddle held on the iambic keyer, so
// a string of dots, keyed through key_input.c, keyer.c and cw.c's envelope
// one sample at a time as sound.c does, then through tx_pipeline.c - its
// 300-3000 Hz filter, sideband step, shift to the IF and limiter - to the
// real IF sound.c hands the codec. Its spectrum is the line spectrum of a
// periodic signal, taken over 2.7 s with a 7-term Blackman-Harris window
// (sidelobes below -180 dB) and normalised to the carrier line, so each
// figure is the strongest line beyond the offset, either side: the "peak
// strength" DL1YCF optimised his ramp against.
//
// The mask is checked at 40 WPM, the speed ARRL tests at, at full power
// and with the limiter holding the carrier 10 and 20 dB down (reduced
// POWER). DL1YCF's own tighter limits beyond 600, 900 and 1200 Hz (-80,
// -100, -120 dBc), and 20 and 60 WPM, are printed, not checked: the codec
// and the power amplifier set the real floor well above -100 dBc.
// docs/dsp_design_notes/tx_cw_keying_spectrum.md has the background.
//
//   make test-cw-shape && ./test-cw-shape
//
// Links cw.c, key_input.c, keyer.c, morse.c, gpio.c, vfo.c, fft_filter.c
// and tx_pipeline.c, with radio_get_mode() and radio_set_tx() stubbed
// below. The FFT is a plain double-precision radix-2 one, so the check
// doesn't depend on FFTW's double-precision library.

#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cw.h"
#include "key_input.h"
#include "keyer.h"
#include "radio.h"
#include "tx_pipeline.h"
#include "vfo.h"

#define FS 96000.0
#define BLOCK TX_PIPELINE_BLOCK_LEN
#define T0 1000000000LL // simulated capture clock, ns
#define SETTLE_S 1.0    // keyed before the spectrum is taken
#define N_FFT (1 << 18) // 2.73 s, 0.37 Hz bins
#define SEARCH_HZ 3000.0 // offsets searched either side of the carrier

enum radio_mode radio_get_mode(void) { return RADIO_MODE_CW; }
int radio_set_tx(int on) {
  (void)on;
  return 0;
}

static int failures = 0;
static void check(int ok, const char *what) {
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    failures++;
}

static void fft(double complex *x, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) {
      double complex t = x[i];
      x[i] = x[j];
      x[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    double complex w = cexp(-2.0 * M_PI * I / len);
    for (int i = 0; i < n; i += len) {
      double complex wk = 1.0;
      for (int k = 0; k < len / 2; k++) {
        double complex u = x[i + k], v = x[i + k + len / 2] * wk;
        x[i + k] = u + v;
        x[i + k + len / 2] = u - v;
        wk *= w;
      }
    }
  }
}

// 7-term Blackman-Harris window, sidelobes about -180 dB.
static double bh7(long i, long n) {
  static const double a[7] = {0.27105140069342, 0.43329793923448, 0.21812299954311,
                              0.06592544638803, 0.01081174209837, 0.00077658482522,
                              0.00001388721735};
  double x = 2.0 * M_PI * i / (n - 1), w = 0;
  for (int k = 0; k < 7; k++)
    w += (k % 2 ? -a[k] : a[k]) * cos(k * x);
  return w;
}

struct shape {
  double carrier_hz;
  double bin_hz;
  double *db; // dBc per bin, 0 at the carrier
  long carrier_bin;
};

// The radio's IF for a held dot paddle at wpm, through the limiter at
// ceiling; its spectrum in dBc.
static struct shape keyed_spectrum(int wpm, float ceiling) {
  cw_init();
  keyer_set_mode(KEYER_IAMBIC_A);
  keyer_set_wpm(wpm);
  key_input_arm(0, T0, 3, cw_key_closed);
  struct tx_pipeline *p = tx_pipeline_new();
  tx_pipeline_set_ceiling(p, ceiling);

  long settle = (long)(SETTLE_S * FS / BLOCK) + 1;
  long blocks = settle + N_FFT / BLOCK + 1;
  double complex *x = calloc(N_FFT, sizeof *x);
  if (!x)
    exit(2);
  float in[BLOCK], out[BLOCK];
  long got = 0;
  for (long k = 1; k <= blocks && got < N_FFT; k++) {
    int64_t t_end = T0 + (int64_t)llround(k * BLOCK * 1e9 / FS);
    if (k == 2) // the dot paddle (the tip) closes and stays closed
      key_input_edge(KEY_TIP, 1, t_end - 3000000);
    key_input_tick(t_end);
    cw_poll_key(t_end, BLOCK);
    for (int i = 0; i < BLOCK; i++)
      in[i] = (float)cw_get_sample();
    tx_pipeline_process_block(p, TX_PIPELINE_CW, in, out);
    if (k > settle)
      for (int i = 0; i < BLOCK && got < N_FFT; i++, got++)
        x[got] = out[i] * bh7(got, N_FFT);
  }
  tx_pipeline_free(p);
  fft(x, N_FFT);

  struct shape s = {0};
  s.bin_hz = FS / N_FFT;
  s.db = malloc(sizeof(double) * (N_FFT / 2));
  double peak = 0;
  for (long i = 1; i < N_FFT / 2; i++) {
    double v = creal(x[i]) * creal(x[i]) + cimag(x[i]) * cimag(x[i]);
    s.db[i] = v;
    if (v > peak) {
      peak = v;
      s.carrier_bin = i;
    }
  }
  for (long i = 1; i < N_FFT / 2; i++)
    s.db[i] = 10.0 * log10(s.db[i] / peak + 1e-30);
  s.db[0] = -300;
  s.carrier_hz = s.carrier_bin * s.bin_hz;
  free(x);
  return s;
}

// The strongest line more than offset_hz from the carrier, on either side,
// within SEARCH_HZ.
static double beyond(const struct shape *s, double offset_hz) {
  double worst = -300;
  long lo = (long)ceil(offset_hz / s->bin_hz), hi = (long)(SEARCH_HZ / s->bin_hz);
  for (long d = lo; d <= hi; d++) {
    long a = s->carrier_bin + d, b = s->carrier_bin - d;
    if (a < N_FFT / 2 && s->db[a] > worst)
      worst = s->db[a];
    if (b > 0 && s->db[b] > worst)
      worst = s->db[b];
  }
  return worst;
}

static const double mask_hz[3] = {90, 150, 338}, mask_db[3] = {-20, -40, -60};
static const double ext_hz[3] = {600, 900, 1200}, ext_db[3] = {-80, -100, -120};

static void measure(int wpm, float ceiling, int checked) {
  struct shape s = keyed_spectrum(wpm, ceiling);
  char msg[200];
  printf("%d WPM, limiter ceiling %.3f (%.0f dB), carrier at the IF's %.1f Hz\n", wpm, ceiling,
         20 * log10(ceiling), s.carrier_hz);
  for (int k = 0; k < 3; k++) {
    double v = beyond(&s, mask_hz[k]);
    snprintf(msg, sizeof msg, "beyond %3.0f Hz: %7.1f dBc (CSI mask %4.0f, margin %5.1f dB)",
             mask_hz[k], v, mask_db[k], mask_db[k] - v);
    if (checked)
      check(v <= mask_db[k], msg);
    else
      printf("       %s\n", msg);
  }
  for (int k = 0; k < 3; k++)
    printf("       beyond %4.0f Hz: %7.1f dBc (DL1YCF's %4.0f)\n", ext_hz[k],
           beyond(&s, ext_hz[k]), ext_db[k]);
  free(s.db);
}

int main(void) {
  vfo_init_phase_table();

  printf("the CSI mask at 40 WPM, at full power and reduced POWER\n");
  measure(40, 1.0f, 1);
  measure(40, 0.316f, 1);
  measure(40, 0.1f, 1);

  printf("\nother speeds, for information\n");
  measure(20, 1.0f, 0);
  measure(60, 1.0f, 0);

  printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
