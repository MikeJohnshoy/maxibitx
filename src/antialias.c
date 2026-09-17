// antialias.c
//
// Coefficients from docs/dsp_design_notes/antialias_filter_design.md

#include "antialias.h"

static const double fir_coeffs[ANTIALIAS_TAPS] = {
     0.00232502, -0.00630660,  0.01119704, -0.01332687,  0.00734644,
     0.01168262, -0.04531016,  0.08949296, -0.13468276,  0.16869859,
     0.81864267,
     0.16869859, -0.13468276,  0.08949296, -0.04531016,  0.01168262,
     0.00734644, -0.01332687,  0.01119704, -0.00630660,  0.00232502,
};

double antialias_apply(struct antialias_state *f, double x) {
    // Double-write: hist[pos] and hist[pos+TAPS] always hold the same
    // value, so a contiguous TAPS-long read starting anywhere from
    // pos+1 to pos+TAPS never needs to wrap around the buffer.
    f->hist[f->pos] = x;
    f->hist[f->pos + ANTIALIAS_TAPS] = x;

    int base = f->pos + 1;
    double acc = 0.0;
    for (int i = 0; i < ANTIALIAS_TAPS; i++)
        acc += fir_coeffs[i] * f->hist[base + i];

    f->pos++;
    if (f->pos == ANTIALIAS_TAPS) f->pos = 0;
    return acc;
}
