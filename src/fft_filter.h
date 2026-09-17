// fft_filter.h
//
// A tunable, FFT overlap-save bandpass filter - the shared primitive
// docs/ARCHITECTURE.md's plan is built on. One instance of this does
// the "software-domain sideband separation" job SSB TX needs, and
// (later) the same job rx_audio.c's fixed elliptic stage 3 does today,
// just with a live-adjustable passband instead of a baked-in one.
//
// The frequency-domain design math (make_kaiser/window_filter/
// filter_tune) is ported near-verbatim from real sbitx's
// mj_zbitx/src/fft_filter.c - it's a proven, working technique, not
// something worth re-deriving from scratch. What's NOT copied from
// sbitx is how a live block gets convolved: sbitx does that with
// several global buffers (fft_in/fft_out/fft_m) shared across every
// filter instance and every mode, which is exactly the kind of
// hidden-coupling this project is trying to avoid (see
// docs/ARCHITECTURE.md §2). Here, filter_run() is self-contained: each
// `struct filter` owns its own FFT scratch buffers, its own saved
// overlap history, and its own FFTW plans, so two instances (e.g. a TX
// filter and an RX filter) can run independently with nothing shared
// between them.
//
// Precision: single precision (fftwf/complex float) throughout, for
// both the design math and the per-block convolution - unlike real
// sbitx, which mixes fftwf (design) with plain fftw/double (block
// convolution). float matches struct filter's own fir_coeff type
// (already complex float even in sbitx), avoids linking two precisions
// of FFTW, and vectorizes on the Pi's NEON unit, which has no
// double-precision SIMD - see antialias.c's branch-free loop for the
// same reasoning applied elsewhere in this tree.

#ifndef FFT_FILTER_H
#define FFT_FILTER_H

#include <complex.h>
#include <fftw3.h>

struct filter {
	int L;   // new input samples consumed per filter_run() call
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
// `impulse_len`-long impulse response (impulse_len - 1 must not exceed
// block_len for the FFT-size math below to make sense the way sbitx's
// own 1024/1025 choice does - see filter_tune()'s header for why 1025,
// not 1024 or 1026). Builds both FFTW plans up front (FFTW_MEASURE:
// spends real time up front finding the fastest algorithm for this
// machine, paid once at startup, not per block - see filter_tune()'s
// comment on wisdom-file caching, not yet done here). Coefficients are
// all-zero (full stop) until filter_tune() is called at least once.
struct filter *filter_new(int block_len, int impulse_len);

// (Re)designs the passband: everything in the normalized range
// [low, high) (each a fraction of the sample rate, e.g. -0.5..0.5, same
// convention as sbitx's own filter_tune()) passes; everything else is
// stopped, with edges shaped by a Kaiser window of the given beta
// (~5.0 for sbitx's proven ~54dB stopband target - see
// docs/ARCHITECTURE.md §4's Kaiser-beta derivation). Safe to call again
// at any time (e.g. live pitch/width changes) - it only touches
// fir_coeff, not the running overlap-save state in history/time/freq.
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
// discard half of it. First user: rx_filter.c (rx_audio.c's stage 3,
// docs/ARCHITECTURE.md §5/§10 step 6).
int filter_tune_real(struct filter *f, float low, float high, float kaiser_beta);

// Phase 1 of filter_run(): prepends the saved M-1-sample history to
// `in`'s L new complex samples, forward-FFTs the result, and multiplies
// by fir_coeff - all unconditional, mode-independent passband work.
// Leaves the N-point result in f->freq for the caller to inspect or
// further modify (e.g. zero one sideband's half of the bins, rotate to
// an IF - see docs/ARCHITECTURE.md §5) before calling filter_inverse().
// Also updates history for the next call, from this call's own input.
void filter_forward(struct filter *f, const complex float *in);

// Phase 2: inverse-FFTs f->freq (as filter_forward() left it, plus
// whatever the caller changed) into f->time, discards the first M-1
// samples (the circular-convolution wrap-around region overlap-save
// exists to discard), and writes the remaining L valid samples to
// out[]. Together, filter_forward()+filter_inverse() is one block of
// overlap-save convolution; splitting them in two is what lets a caller
// (SSB TX's sideband zero, an IF bin-rotate) act in between without the
// filter itself knowing anything about modes.
void filter_inverse(struct filter *f, complex float *out);

// Releases everything filter_new() allocated, including the FFTW plans.
void filter_free(struct filter *f);

#endif /* FFT_FILTER_H */
