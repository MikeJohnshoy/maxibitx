// decim48k.h
//
// Decimates 96kHz audio by 2, to 48kHz: sound.c runs rx_audio.c's
// receive audio tap (uac_out) through it for the USB gadget (WSJT-X) and
// TCI, which both run at 48kHz. See
// docs/dsp_design_notes/usb_uac_decimation_design.md for the filter
// design (written for the I/Q it once carried; the filter is the same).

#ifndef DECIM48K_H
#define DECIM48K_H

#define DECIM48K_TAPS 25

// One instance per rail (sound.c has one, for mono audio); rails share the
// coefficient table (decim48k.c) but each needs its own history and
// decimation phase. Zero-initialize (static/global storage does this
// automatically) before first use. Same double-write history layout as
// antialias_state - see antialias.h for why.
struct decim48k_state {
    double hist[2 * DECIM48K_TAPS];
    int pos;     // write cursor, always in [0, DECIM48K_TAPS)
    int phase;   // toggles every call; only every other call emits output
};

// Feed one 96kHz-rate input sample. Every call updates the filter's
// history (needed for correct output on the calls that do emit), but
// only every other call actually produces a decimated 48kHz output
// sample - that call writes *out and returns 1; the discarded call
// returns 0 and leaves *out untouched. Call once per input sample, per
// rail; rails that must stay aligned (I and Q) are fed on every sample in
// lockstep.
int decim48k_apply(struct decim48k_state *f, double x, double *out);

#endif /* DECIM48K_H */
