// fft_filter.h
//
// A tunable, FFT overlap-save bandpass filter - the shared primitive
// docs/ARCHITECTURE.md's plan is built on. One instance of this does
// the "software-domain sideband separation" job SSB TX needs, and another
// (rx_filter.c) is one of the two implementations of rx_audio.c's stage-3
// narrow filter - the one whose passband tunes continuously, against an
// elliptic bank offering a fixed set of pitches and widths.
//
// The frequency-domain design math (make_kaiser/window_filter/
// filter_tune) is ported near-verbatim from sbitx's fft_filter.c
// (mj_zbitx/src/fft_filter.c). The block convolution is not: sbitx shares
// global buffers (fft_in/fft_out/fft_m) across every filter and mode,
// where here each `struct filter` owns its FFT scratch buffers, its
// overlap history and its FFTW plans, so instances (a TX filter and an RX
// filter) run independently with nothing shared (docs/ARCHITECTURE.md
// §2).
//
// Precision: single precision (fftwf/complex float) throughout, design
// and convolution alike (sbitx convolves in double). It matches
// fir_coeff's type, avoids linking two precisions of FFTW, and
// vectorizes on the Pi's NEON unit, which has no double-precision SIMD.

#ifndef FFT_FILTER_H
#define FFT_FILTER_H

#include <complex.h>
#include <fftw3.h>

struct filter {
	int L;   // new input samples consumed per filter_forward()/filter_inverse() pair
	int M;   // impulse response length (design-time only)
	int N;   // FFT size, L + M - 1

	complex float *fir_coeff; // N-point frequency-domain passband, set by filter_tune()
	complex float *history;   // M-1 saved samples, the tail of the previous block's time-domain input
	complex float *time;      // N-point scratch: [history | new input] going in, raw (unextracted) convolution result coming out of the inverse FFT
	complex float *freq;      // N-point scratch: forward-FFT output, then fir_coeff-multiplied - see filter_forward()/filter_inverse()

	fftwf_plan plan_fwd; // time -> freq
	fftwf_plan plan_rev; // freq -> time
};

// Allocates a filter sized for `block_len` new samples per call and an
// `impulse_len`-long impulse response; the FFT size N is their sum less
// one (sbitx's 1024/1025 gives 2048). Builds both FFTW plans up front
// with FFTW_MEASURE: a search for this machine's fastest algorithm, paid
// once at start-up (there is no wisdom-file cache), not per block.
// Coefficients are all-zero (full stop) until filter_tune() is called.
// A wrapper over filter_new_ex() at FFTW_MEASURE (tx_pipeline.c, the
// bench harnesses).
struct filter *filter_new(int block_len, int impulse_len);

// filter_new() with the FFTW plan flags (FFTW_MEASURE, FFTW_ESTIMATE, ...)
// as a parameter. FFTW_ESTIMATE skips the search for a good-enough plan at
// once - rx_filter.c uses it, so its N=4096 search doesn't add to
// tx_pipeline.c's at start-up. That plan runs every block, so its
// per-block cost is what to check (ARCHITECTURE.md §10 step 7's
// follow-up).
struct filter *filter_new_ex(int block_len, int impulse_len, unsigned fftw_flags);

// (Re)designs the passband: everything in the normalized range
// [low, high) (each a fraction of the sample rate, e.g. -0.5..0.5, same
// convention as sbitx's own filter_tune()) passes; everything else is
// stopped, with edges shaped by a Kaiser window of the given beta
// (5.0 for sbitx's ~54dB stopband, docs/ARCHITECTURE.md §4). Safe to call
// again at any time (live pitch/width changes) - it only touches
// fir_coeff, not the running overlap-save state in history/time/freq -
// but not from the audio thread: it allocates and plans.
//
// This is a single, one-sided passband interval - correct for isolating
// one sideband of an analytic/SSB-style signal under construction
// (docs/ARCHITECTURE.md §5's TX pipeline, tx_pipeline.c), where the
// *other* half of the spectrum is deliberately unwanted image content.
// It is the wrong tool for filtering a genuinely real-valued signal
// (e.g. rx_audio.c's stage 3, post-demodulation audio) - a real time
// series always has a conjugate-symmetric spectrum by construction
// (energy at +f and -f are the SAME signal's own mirror images, not an
// image to reject), so passing only [low, high] and zeroing its mirror
// would discard half of that signal's own real energy. filter_tune_real()
// below is for that case.
int filter_tune(struct filter *f, float low, float high, float kaiser_beta);

// (Re)designs the passband as a REAL bandpass: passes both [low, high]
// and its mirror image [-high, -low] (same normalized-frequency
// convention as filter_tune() above), so a genuinely real-valued
// signal's energy on both sides of 0 Hz survives intact - see
// filter_tune()'s own comment for why plain filter_tune() would instead
// discard half of it. Used by rx_filter.c (rx_audio.c's stage 3).
int filter_tune_real(struct filter *f, float low, float high, float kaiser_beta);

// Replaces the response filter_tune()/filter_tune_real() just designed
// with a minimum-phase one of the SAME magnitude: same passband, same
// skirts, but the group delay collapsed and the impulse decaying from
// time zero instead of sitting symmetrically about M/2. Returns -1 if
// nothing has been tuned yet.
//
// Call it after every tune - a retune rebuilds fir_coeff from scratch,
// so the conversion has to be reapplied (rx_filter.c does this).
//
// The cost is phase linearity: group delay is no longer constant across
// the passband. For a CW filter that is the right trade - the ear hears
// the 16ms delay and the slow attack, not the phase curve - but it is a
// real one, which is why this is opt-in rather than what filter_tune()
// does by default. See fft_filter.c for how the factorization works.
int filter_min_phase(struct filter *f);

// One block of overlap-save convolution, in two halves so a caller can act
// on the spectrum in between. filter_forward(): prepends the saved
// M-1-sample history to
// `in`'s L new complex samples, forward-FFTs the result, and multiplies
// by fir_coeff - all unconditional, mode-independent passband work.
// Leaves the N-point result in f->freq for the caller to inspect or
// further modify (e.g. zero one sideband's half of the bins, rotate to
// an IF - see docs/ARCHITECTURE.md §5) before calling filter_inverse().
// Also updates history for the next call, from this call's own input.
void filter_forward(struct filter *f, const complex float *in);

// filter_inverse(): inverse-FFTs f->freq (as filter_forward() left it, plus
// whatever the caller changed) into f->time, discards the first M-1
// samples (the circular-convolution wrap-around region overlap-save
// exists to discard), and writes the remaining L valid samples to
// out[]. Together, filter_forward()+filter_inverse() is one block of
// overlap-save convolution; splitting them in two is what lets a caller
// (SSB TX's sideband zero, an IF bin-rotate) act in between without the
// filter itself knowing anything about modes.
void filter_inverse(struct filter *f, complex float *out);

// Clears the overlap-save history, so the next filter_forward() sees
// silence behind its block instead of the tail of whatever ran last.
// history is the only state carried between calls - time and freq are
// scratch, rewritten every block, and fir_coeff is the design.
//
// A filter that runs continuously never wants this. It is for one that
// stops and restarts, where the gap means the old tail isn't the
// signal's own past: tx_pipeline_reset() calls it when a transmission
// begins.
void filter_reset(struct filter *f);

// Releases everything filter_new() allocated, including the FFTW plans.
void filter_free(struct filter *f);

#endif /* FFT_FILTER_H */
