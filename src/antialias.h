// antialias.h
//
// cleans up the self-image that synthesizing I/Q from a
// single real ADC channel produces near the +-48kHz Nyquist edges.

#ifndef ANTIALIAS_H
#define ANTIALIAS_H

#define ANTIALIAS_TAPS 21

// One instance per rail - I and Q each need independent history, but
// share the same coefficient table (antialias.c). Zero-initialize
// (static/global storage does this automatically) before first use.
//
// Double-length history buffer: every new sample is written at two
// mirrored positions (see antialias_apply()), so the convolution always
// reads a contiguous window with no modulo/wraparound check inside the
// loop - that branch was the one thing stopping GCC from vectorizing
// this at -O3. That gcc optimization cuts the cpu impact by half (though
// the cpu impact of this filter was very low to start with)
struct antialias_state {
    double hist[2 * ANTIALIAS_TAPS];
    int pos;   // write cursor, always in [0, ANTIALIAS_TAPS)
};

// Feeds one sample through the filter, returns the filtered output.
// Call once per sample, per rail.
double antialias_apply(struct antialias_state *f, double x);

#endif /* ANTIALIAS_H */
