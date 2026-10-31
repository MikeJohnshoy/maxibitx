// rx_filter.h
//
// The shared FFT overlap-save filter (fft_filter.c/.h), applied to
// rx_audio.c's stage 3 - the narrow, post-demodulation "single signal"
// selectivity stage - as one of that stage's two implementations,
// alongside the pre-designed elliptic bank (src/narrow_filter_bank.h). It
// is the default, run at minimum phase.
//
// Against the bank: it tunes continuously rather than to the bank's six
// pitches and four widths, and its stopband keeps falling with distance
// (past -100dB) where an equiripple elliptic floors out at its design
// figure of -50dB. What it costs is attack on keyed CW - a keyed element
// settles in 8.8ms against the elliptic's 5.2ms at 300Hz, minimum phase
// included, because a 300Hz filter at 96kHz is a 0.3% fractional bandwidth
// and this one spends 3073 taps on it. docs/dsp_design_notes/
// rx_narrow_filter_fft_vs_elliptic.md has the measurements; ARCHITECTURE.md
// §10 step 6 the bench checks behind the module.
//
// rx_audio.c creates one instance in rx_audio_init(), runs it on every
// block whether or not it is the selected implementation (so switching
// never thumps), and retunes it whenever the pitch or width changes.
// rx_filter_test.c is the standalone harness for its DSP.
//
// A filter of its own rather than tx_pipeline.c's, because the job
// differs: TX keeps one side of an analytic signal's spectrum and moves it
// to an IF; here the input (stage 2's demodulated audio) is real, and the
// energy at -pitch is the same signal's mirror image, not something to
// reject. fft_filter.c's filter_tune_real() passes both [low,high] and
// [-high,-low], so the output is real and at unity gain (rx_filter_test.c's
// Case A checks both).
//
// Sizing: L (block length) is RX_FILTER_BLOCK_LEN, sound.c's
// PERIOD_FRAMES, since the overlap-save history only stays valid across
// calls that each supply exactly L new samples. M (impulse length) is far
// longer than TX's 1025: TX's 2700Hz passband barely notices a ~340Hz
// transition, but a ~300Hz passband needs one nearer 100Hz, for which
// Harris' estimate (rx_audio_demod_design.md §7.3) gives M of about 3500.
// M = 3073 puts N on 4096, a power of two (FFTW's best case). The measured
// transition, shape factor and latency are in ARCHITECTURE.md §10 step 6.
#ifndef RX_FILTER_H
#define RX_FILTER_H

#include <complex.h>
#include <stdlib.h>
#include "fft_filter.h"

#define RX_FILTER_BLOCK_LEN 1024                                        // must match sound.c's PERIOD_FRAMES
#define RX_FILTER_N 4096                                                 // power-of-2 FFT size - see header derivation above
#define RX_FILTER_IMPULSE_LEN (RX_FILTER_N - RX_FILTER_BLOCK_LEN + 1)    // 3073
#define RX_FILTER_KAISER_BETA 5.0f
#define RX_FILTER_FS_HZ 96000.0f
#define RX_FILTER_BIN_HZ (RX_FILTER_FS_HZ / RX_FILTER_N)                 // Fs/N, 23.4375Hz here

// Default passband width, matching the elliptic bank's own middle design
// point (order=4, 0.5dB ripple, 50dB stopband, ~300Hz -3dB width - see
// tools/gen_narrow_filters.py) - a reasonable default and a fair
// comparison point, not a claim that 300Hz is the ideal width for this
// different filter shape. rx_audio.c retunes this filter to whatever pitch
// and width are selected, so the two implementations always face the same
// passband; this constant only sets where they both start.
#define RX_FILTER_DEFAULT_WIDTH_HZ 300.0f

struct rx_filter {
	struct filter *filt;
	float pitch_hz;   // last tuned passband centre, kept so that
	float width_hz;   // rx_filter_set_min_phase() can re-tune itself
	int min_phase;    // see rx_filter_set_min_phase()
};

// Allocates and tunes the shared filter to [pitch_hz - width_hz/2,
// pitch_hz + width_hz/2] (a REAL bandpass - see fft_filter.h's
// filter_tune_real(), which this calls). fs_hz is implicitly
// RX_FILTER_FS_HZ (matching rx_audio.c's own SAMPLE_RATE_HZ - both are the
// audio thread's fixed 96kHz, not independently configurable).
//
// Plans with FFTW_ESTIMATE rather than filter_new()'s FFTW_MEASURE, so
// start-up isn't held up by a 4096-point planning search on top of
// tx_pipeline_new()'s; the plan it picks fits the 10.7ms block easily
// (ARCHITECTURE.md §10 step 7's follow-up). MAXIBITX_RX_FILTER_FFTW_MEASURE
// (rx_filter.c) forces FFTW_MEASURE.
struct rx_filter *rx_filter_new(float pitch_hz, float width_hz);

// Re-tunes the passband to a new pitch/width, live, with no rebuild.
// Safe to call at any time, the same guarantee filter_tune()/
// filter_tune_real() give (only fir_coeff changes, not the running
// overlap-save state). Not on the audio thread: it allocates.
int rx_filter_retune(struct rx_filter *r, float pitch_hz, float width_hz);

// Selects a linear-phase (off, this module's default) or minimum-phase
// (on) realization of the same passband, and re-tunes to apply it.
// rx_audio.c turns it on: minimum phase is what the radio runs. The
// magnitude response is the same either way; what changes is the time
// domain. A linear-phase FIR of this length delays everything by
// (M-1)/2 = 1536 samples - 16ms at 96kHz - and spreads a keyed CW
// element's attack symmetrically around that, which makes it sound duller
// on CW than the elliptic despite measuring better on magnitude. Minimum
// phase keeps the magnitude and drops most of the delay, at the cost of a
// group delay that varies across the passband.
// rx_narrow_filter_fft_vs_elliptic.md §11.
int rx_filter_set_min_phase(struct rx_filter *r, int on);

// Processes one RX_FILTER_BLOCK_LEN-sample block of real, already-
// demodulated audio (rx_audio.c stage 2's output) through the shared
// filter: forward FFT, unconditional passband multiply (both mirrored
// halves - filter_tune_real()), inverse FFT, real-part extraction. No
// sideband-zero step and no bin-rotate here (unlike tx_pipeline.c) -
// this filter shapes a signal in place, it doesn't move or split one -
// so there's no analogous phase-continuity correction needed either
// (see tx_pipeline.c's own comment on where that requirement comes
// from: a per-block bin ROTATE, which this module never does).
// `in`/`out` are both RX_FILTER_BLOCK_LEN real samples.
void rx_filter_process_block(struct rx_filter *r, const float *in, float *out);

void rx_filter_free(struct rx_filter *r);

#endif /* RX_FILTER_H */
