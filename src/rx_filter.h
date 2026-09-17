// rx_filter.h
//
// The shared FFT overlap-save filter (fft_filter.c/.h), applied to
// rx_audio.c's stage 3 - the narrow, post-demodulation "single signal"
// selectivity stage that today is a fixed 8-pole elliptic IIR
// (narrow_filter_coeffs[] in rx_audio.c). See docs/ARCHITECTURE.md §5's
// "RX pipeline" section for why this replacement exists (pitch/width as
// live filter_tune()-style parameters instead of a baked-in, offline-
// designed elliptic response) and §10 step 6 for the bench numbers this
// module was checked against before ever touching rx_audio.c's real,
// live audio path.
//
// docs/ARCHITECTURE.md build order step 6 status: this module exists
// and is bench-verified (rx_filter_test.c) against synthetic tones - it
// is NOT wired into rx_audio.c yet. rx_audio.c's existing
// narrow_filter_coeffs[]/narrow_filter_apply() are completely untouched
// and are still what the real, running binary demodulates CW with; this
// is a parallel, proven-on-the-bench-only replacement, not yet a live
// one - same "bench first, live-wire and on-air-compare later" split
// build order steps 4/5 already used for the TX side. See
// ARCHITECTURE.md §10 step 6 for what's proven vs. still open, and
// step 7 for what's still ahead before any live cutover.
//
// Why this needs its own module, not just a call into tx_pipeline.c:
// unlike tx_pipeline.c (a genuinely SHARED pipeline - one instance
// serves CW/SSB/DIGITAL's TX, each just supplying a different i_sample
// source, §5's TX pipeline diagram), this filter has exactly one caller
// (rx_audio.c's stage 3) and does a fundamentally different spectral
// job: TX's pipeline deliberately keeps only ONE SIDE of an
// under-construction analytic signal's spectrum (the explicit
// sideband-zero step, tx_pipeline.c) and rotates it to an IF - the
// other half really is an unwanted image there. Here, the input
// (rx_audio.c stage 2's already-demodulated "audio") is already a
// genuinely real-valued signal, and any real time series has a
// conjugate-symmetric spectrum by construction - the energy at -pitch
// is the SAME signal's own mirror image, not something to reject. A
// plain filter_tune() passband (a single one-sided interval) would
// discard that mirror half as if it were an image, the same amplitude-
// halving mistake tx_pipeline.c's own sideband-zero step deliberately
// (and correctly) makes on PURPOSE for SSB construction - see
// fft_filter.c's new filter_tune_real() for the fix this module is
// built on (passes BOTH [low,high] and its mirror [-high,-low]), and
// rx_filter_test.c's Case A for the bench check confirming the real
// output really does come out near-exactly real (negligible imaginary
// residual) and at unity gain, not halved.
//
// Sizing - deliberately NOT tx_pipeline.h's L=1024/M=1025/N=2048:
// L (block length) is fixed at RX_FILTER_BLOCK_LEN, matching sound.c's
// PERIOD_FRAMES exactly, same reason tx_pipeline.h's
// TX_PIPELINE_BLOCK_LEN matches it - this module's overlap-save history
// only stays valid across calls that each supply exactly L new samples
// (see rx_filter_process_block()). M (impulse length) is a genuinely
// different, much larger number than TX's 1025: TX's passband is
// 300-3000Hz, 2700Hz wide, so M=1025's measured ~340Hz transition (step
// 2's bench number) is a small fraction of the passband and barely
// matters. This filter's target passband is ~300Hz wide TOTAL (matching
// the elliptic it's replacing) - reusing M=1025 here would give a
// transition almost as wide as the whole passband, defeating the point
// of a "narrow" filter entirely. Harris' FIR-length estimate
// (N ~= (Fs/transition_Hz)*(Astop_dB/22)) scaled against that same
// measured TX data point (M=1025 -> ~340Hz at beta=5/~54dB) for a
// ~100Hz target transition suggests M on the order of 1025*(340/100) ~=
// 3485; RX_FILTER_IMPULSE_LEN below is chosen instead to land RX_FILTER_N
// exactly on a power of two (FFTW's most efficient case) close to that
// estimate, then BENCH-VERIFIED (not just trusted from the formula) by
// rx_filter_test.c - see docs/ARCHITECTURE.md §10 step 6 for the actual
// measured transition width/shape factor this sizing achieves, and for
// the real added block-processing + FIR group-delay latency cost this
// trades for it (a genuine, honestly-quantified cost this filter has
// that the elliptic IIR didn't - see rx_filter_test.c's own latency
// note and §9's "FFT overlap-save block processing adds real
// algorithmic latency" open question, previously flagged for TX only).
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

// Default passband width, matching rx_audio.c's existing elliptic
// filter's own design point (order=4, 0.5dB ripple, 50dB stopband,
// centered on CW_PITCH_HZ with a ~300Hz -3dB width) - a reasonable
// starting comparison point, not a claim that 300Hz is the ideal width
// for this different filter shape (§9's "Width control's real range" is
// still an open question, to be bench-mapped once this filter has a
// live caller).
#define RX_FILTER_DEFAULT_WIDTH_HZ 300.0f

struct rx_filter {
	struct filter *filt;
};

// Allocates and tunes the shared filter to [pitch_hz - width_hz/2,
// pitch_hz + width_hz/2] (a REAL bandpass - see fft_filter.h's
// filter_tune_real(), which this calls). fs_hz is implicitly
// RX_FILTER_FS_HZ (matching rx_audio.c's own SAMPLE_RATE_HZ - both are
// minibitx's fixed 96kHz audio thread rate, not independently
// configurable).
//
// Internally calls fft_filter.h's filter_new_ex() with FFTW_ESTIMATE,
// not plain filter_new()'s FFTW_MEASURE - a real-hardware finding from
// step 7's first on-air test: RX_FILTER_N=4096 is bigger than
// tx_pipeline.c's TX_PIPELINE_N=2048, and rx_audio_init() (which calls
// this) runs during maxibitx's own startup, right alongside
// tx_pipeline_new()'s own pre-existing FFTW_MEASURE search - the two
// compounded into a noticeably longer startup than minibitx's. ESTIMATE
// trades that search away for an immediately-chosen, good-enough plan;
// see docs/ARCHITECTURE.md §10 step 7's follow-up entry for the actual
// measured per-block cost this was checked against before shipping it
// (this filter's block period is ~10.7ms at 96kHz/1024 samples - the
// real-time budget ESTIMATE's plan has to fit inside, same as MEASURE's
// plan did).
struct rx_filter *rx_filter_new(float pitch_hz, float width_hz);

// Re-tunes the passband to a new pitch/width, live - the whole point of
// this migration per docs/ARCHITECTURE.md §5 ("no coefficient
// regeneration, no rebuild, for either"). Safe to call at any time, same
// guarantee filter_tune()/filter_tune_real() themselves give (only
// touches fir_coeff, not the running overlap-save state).
int rx_filter_retune(struct rx_filter *r, float pitch_hz, float width_hz);

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
