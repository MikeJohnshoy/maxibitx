// tone_gen.c - see tone_gen.h.

#include "tone_gen.h"
#include <math.h>

#define TONE_GEN_FS 96000.0

// Written by control threads, read by the audio thread once per sample.
// A plain int is enough: any value it holds is a valid mode.
static volatile int mode = TONE_GEN_OFF;

// Audio-thread-only phase accumulators, radians, kept in [0, 2*pi).
// Double precision and sin() rather than vfo.c's table oscillator, so the
// generator's own spurs sit far below anything the PA produces.
static double phase_a = 0.0;
static double phase_b = 0.0;

void tone_gen_set_mode(enum tone_gen_mode m) {
    if (m == TONE_GEN_OFF || m == TONE_GEN_SINGLE || m == TONE_GEN_TWO)
        mode = m;
}

enum tone_gen_mode tone_gen_get_mode(void) {
    return (enum tone_gen_mode)mode;
}

static double advance(double *phase, double hz) {
    double s = sin(*phase);
    *phase += 2.0 * M_PI * hz / TONE_GEN_FS;
    if (*phase >= 2.0 * M_PI)
        *phase -= 2.0 * M_PI;
    return s;
}

double tone_gen_sample(void) {
    switch (mode) {
    case TONE_GEN_SINGLE:
        return advance(&phase_a, TONE_GEN_SINGLE_HZ);
    case TONE_GEN_TWO:
        // 0.5 each: the sum peaks at 1.0, the same PEP as one full-scale
        // tone, with half its average power.
        return 0.5 * advance(&phase_a, TONE_GEN_TWO_LOW_HZ) +
               0.5 * advance(&phase_b, TONE_GEN_TWO_HIGH_HZ);
    default:
        return 0.0;
    }
}
