// cw_spot.c
//
// Finds the strongest CW tone in a stretch of audio (cw_spot.h).
//
// The audio is cut into half-second segments overlapping by half, each
// Hann-windowed, and the power at every CW_SPOT_STEP_HZ across the band is
// measured in each (Goertzel) and averaged. Averaging is what lets a
// threshold tell a signal from noise: one segment's noise bins scatter
// so widely that the loudest of a few hundred sits about 10 dB over their
// median, while the 5 averaged here bring that to about 4 dB. The tone
// has to stand CW_SPOT_MIN_DB over the median; keyed CW at 0 dB SNR (in
// 2500 Hz) stands about 25 dB over it.
//
// The peak's frequency comes from a parabola through its bin and the two
// beside it, in dB, which puts a steady tone within a few tenths of a Hz.
// Keying spreads a CW signal into sidebands either side of its carrier,
// evenly, so it doesn't move the peak.
//
// Those sidebands reach a long way from a strong signal: one 30 dB over
// the noise still stands 12 dB over it 55 Hz away, and level with it at
// 120 Hz. So the search runs GUARD_HZ beyond the band on each side, and a
// tone is only reported if the strongest over that whole stretch lies in
// the band. A weak station in the band beside a strong one just outside it
// gets nothing rather than a move onto the strong one's sidebands.

#include "cw_spot.h"
#include <math.h>
#include <stdlib.h>

#define SEG_SAMPLES (CW_SPOT_RATE / 2)  // 0.5 s: 2 Hz resolution with Hann
#define SEG_HOP (SEG_SAMPLES / 2)
#define CW_SPOT_STEP_HZ 2.0
#define CW_SPOT_MAX_BINS 512
#define CW_SPOT_MIN_DB 10.0
#define GUARD_HZ 150.0

static int compare_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

int cw_spot_find(const float *x, int n, double lo_hz, double hi_hz, double *freq_hz) {
  double band_lo = lo_hz, band_hi = hi_hz;
  lo_hz -= GUARD_HZ;
  if (lo_hz < CW_SPOT_STEP_HZ)
    lo_hz = CW_SPOT_STEP_HZ;
  hi_hz += GUARD_HZ;
  int bins = (int)((hi_hz - lo_hz) / CW_SPOT_STEP_HZ) + 1;
  if (bins < 3 || n < SEG_SAMPLES)
    return 0;
  if (bins > CW_SPOT_MAX_BINS)
    bins = CW_SPOT_MAX_BINS;

  double power[CW_SPOT_MAX_BINS] = {0};
  double coeff[CW_SPOT_MAX_BINS];
  for (int b = 0; b < bins; b++)
    coeff[b] = 2.0 * cos(2.0 * M_PI * (lo_hz + b * CW_SPOT_STEP_HZ) / CW_SPOT_RATE);

  double *seg = malloc(SEG_SAMPLES * sizeof(double));
  if (!seg)
    return 0;
  for (int start = 0; start + SEG_SAMPLES <= n; start += SEG_HOP) {
    for (int i = 0; i < SEG_SAMPLES; i++)
      seg[i] = x[start + i] * (0.5 - 0.5 * cos(2.0 * M_PI * i / (SEG_SAMPLES - 1)));
    for (int b = 0; b < bins; b++) {
      double s1 = 0, s2 = 0, c = coeff[b];
      for (int i = 0; i < SEG_SAMPLES; i++) {
        double s = seg[i] + c * s1 - s2;
        s2 = s1;
        s1 = s;
      }
      power[b] += s1 * s1 + s2 * s2 - c * s1 * s2;
    }
  }
  free(seg);

  int peak = 0;
  for (int b = 1; b < bins; b++)
    if (power[b] > power[peak])
      peak = b;
  if (peak == 0 || peak == bins - 1)
    return 0;

  double sorted[CW_SPOT_MAX_BINS];
  for (int b = 0; b < bins; b++)
    sorted[b] = power[b];
  qsort(sorted, bins, sizeof(double), compare_double);
  double median = sorted[bins / 2];
  if (!(power[peak] > median * pow(10.0, CW_SPOT_MIN_DB / 10.0)))
    return 0;

  double ym = 10.0 * log10(power[peak - 1] + 1e-30);
  double y0 = 10.0 * log10(power[peak] + 1e-30);
  double yp = 10.0 * log10(power[peak + 1] + 1e-30);
  double den = ym - 2.0 * y0 + yp;
  double delta = den < 0 ? 0.5 * (ym - yp) / den : 0.0;
  double f = lo_hz + (peak + delta) * CW_SPOT_STEP_HZ;
  if (f < band_lo || f > band_hi)
    return 0;
  *freq_hz = f;
  return 1;
}
