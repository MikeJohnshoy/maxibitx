// upsample48k.h
//
// Upsamples 48kHz real audio to the 96kHz transmit rate - the mirror
// image of decim48k.c's decimation. sound.c runs the transmit audio that
// arrives at 48kHz - WSJT-X's from the USB gadget, or a TCI client's -
// through it, since its TX audio block (and tx_pipeline.c) is fixed at
// 96kHz like every other TX audio source (cw.c's sidetone, the mic).
//
// Classic zero-stuff-then-lowpass L=2 interpolation, not a novel design:
// insert one zero sample after each real input sample (doubling the rate),
// then run the combined stream through a lowpass filter aimed at the
// ORIGINAL signal's 24kHz Nyquist edge to reject the spectral images
// zero-stuffing introduces at multiples of 48kHz, then restore the
// average energy the inserted zeros halved with a gain-of-2 makeup factor
// (upsample48k.c's UPSAMPLE48K_GAIN).
//
// Uses decim48k.c's 25-tap coefficients (a copy: decim48k.c keeps its
// table file-static). An anti-aliasing filter for decimation by 2 and an
// anti-imaging filter for interpolation by 2 are the same prototype
// lowpass; only where the zeros go and the gain-of-L correction differ.
// docs/dsp_design_notes/usb_uac_decimation_design.md derives it (15kHz
// passband edge, 24kHz stopband edge).

#ifndef UPSAMPLE48K_H
#define UPSAMPLE48K_H

#define UPSAMPLE48K_TAPS 25

// One instance per real-audio rail (sound.c has one, mono transmit
// audio). Zero-initialize (static/global storage does this
// automatically) before first use. Same double-write history layout as
// decim48k_state/antialias_state - see antialias.h for why.
struct upsample48k_state {
    double hist[2 * UPSAMPLE48K_TAPS];
    int pos; // write cursor, always in [0, UPSAMPLE48K_TAPS)
};

// Feed one 48kHz-rate input sample; always produces exactly two 96kHz-rate
// output samples in out[0]/out[1] (the interpolated pair straddling this
// input sample) - unlike decim48k_apply()'s one-in/sometimes-one-out
// shape (decimation discards half its calls' worth of output),
// interpolation is one-in/always-two-out, so this has no bool return to
// check. Call once per 48kHz-rate input sample; out[]'s two samples are
// then this input's contribution to the 96kHz stream, in order.
void upsample48k_apply(struct upsample48k_state *f, double x, double out[2]);

#endif /* UPSAMPLE48K_H */
