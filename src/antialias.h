// antialias.h
//
// Cleans up the self-image that synthesizing I/Q from a single real ADC
// channel produces near the +-48kHz Nyquist edges: sound_process() runs
// I and Q through it right after the fs/4 mix, before any I/Q consumer.
// docs/dsp_design_notes/antialias_filter_design.md has the design.

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
// loop - such a branch stops GCC vectorizing it at -O3, which halves this
// (already small) filter's CPU cost.
struct antialias_state {
    double hist[2 * ANTIALIAS_TAPS];
    int pos;   // write cursor, always in [0, ANTIALIAS_TAPS)
};

// Feeds one sample through the filter, returns the filtered output.
// Call once per sample, per rail.
double antialias_apply(struct antialias_state *f, double x);

#endif /* ANTIALIAS_H */
