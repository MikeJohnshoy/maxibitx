// tx_pipeline.c
//
// See tx_pipeline.h for what this is, and its TX_IF_SHIFT_HZ comment for
// the placement derivation. This file owns the two pieces of new math
// the shared pipeline needs that fft_filter.c doesn't already provide:
// the explicit sideband-zero step and the shared IF bin-rotate.

#include <stdio.h>
#include <string.h>
#include "tx_pipeline.h"

struct tx_pipeline *tx_pipeline_new(void)
{
	struct tx_pipeline *p = malloc(sizeof(struct tx_pipeline));

	// FFTW_ESTIMATE by default, not filter_new()'s FFTW_MEASURE - the
	// same trade rx_filter_new() made (see its own comment in
	// rx_filter.c) for the same reason, applied here after real-hardware
	// evidence (docs/ARCHITECTURE.md §10 step 7's follow-up) that this
	// specific FFTW_MEASURE search - which runs synchronously inside
	// sound_thread_start(), AFTER pcm_playback is opened/primed but
	// BEFORE the audio thread that's supposed to keep it fed is even
	// created - was long enough to drain sound.c's newly-added playback
	// pre-fill before the real per-block writes ever got a chance to
	// start, producing a burst of startup xruns that priming alone
	// couldn't fix (the buffer was primed, then sat idle draining for
	// the whole length of this search). FFTW_ESTIMATE removes that gap
	// instead of trying to out-buffer it. Overridable via
	// MAXIBITX_TX_PIPELINE_FFTW_MEASURE (same diagnostic pattern as
	// rx_filter.c's own env var) for re-testing whether this was really
	// the cause, or reverting if FFTW_ESTIMATE's own per-block cost ever
	// turns out not to fit TX_PIPELINE_N=2048's real-time budget on some
	// board (checked, not assumed, for RX's own larger N=4096 - not yet
	// separately re-checked here, though TX's N is smaller, so it should
	// only be an easier case).
	const char *force_measure = getenv("MAXIBITX_TX_PIPELINE_FFTW_MEASURE");
	unsigned flags = (force_measure && *force_measure) ? FFTW_MEASURE : FFTW_ESTIMATE;
	printf("tx_pipeline: using %s for its FFTW plan (TX_PIPELINE_N=%d)%s\n",
	       flags == FFTW_MEASURE ? "FFTW_MEASURE" : "FFTW_ESTIMATE", TX_PIPELINE_N,
	       flags == FFTW_MEASURE ? " - MAXIBITX_TX_PIPELINE_FFTW_MEASURE set, expect a slower startup" : "");

	p->filt = filter_new_ex(TX_PIPELINE_BLOCK_LEN, TX_PIPELINE_IMPULSE_LEN, flags);
	filter_tune(p->filt, 300.0f / TX_PIPELINE_FS_HZ, 3000.0f / TX_PIPELINE_FS_HZ,
	            TX_PIPELINE_KAISER_BETA);
	// Plain malloc, not fftwf_alloc_complex: this buffer is only ever a
	// memcpy scratch for rotate_bins() below, never handed to FFTW
	// itself, so it doesn't need FFTW's alignment guarantee.
	p->rotate_scratch = malloc(p->filt->N * sizeof(complex float));
	p->block_count = 0;
	return p;
}

int tx_pipeline_retune(struct tx_pipeline *p, float low_hz, float high_hz, float fs_hz)
{
	return filter_tune(p->filt, low_hz / fs_hz, high_hz / fs_hz, TX_PIPELINE_KAISER_BETA);
}

// Explicit sideband zero - see fft_filter.h's filter_forward() comment
// on why this is the caller's job, not the filter's. Same bin convention
// filter_tune()/fft_filter_test.c already use: bins n <= N/2 are
// positive frequencies (kept for TX_PIPELINE_KEEP_UPPER), n > N/2 are
// the folded negative frequencies (kept for TX_PIPELINE_KEEP_LOWER).
static void zero_sideband(complex float *freq, int N, enum tx_pipeline_sideband sideband)
{
	if (sideband == TX_PIPELINE_KEEP_UPPER) {
		for (int i = N / 2 + 1; i < N; i++)
			freq[i] = 0;
	} else {
		for (int i = 0; i <= N / 2; i++)
			freq[i] = 0;
	}
}

// Circularly rotates an N-point frequency-domain buffer by `shift_bins`
// bins - the frequency-domain equivalent of multiplying the time-domain
// signal by a complex exponential at shift_bins*Fs/N Hz, i.e. an IF
// shift with no separate NCO needed. Needs a full-buffer scratch copy
// (can't rotate a circular buffer in place one element at a time without
// one) - p->rotate_scratch (tx_pipeline_new()) exists so this never
// mallocs on the audio-rate path.
static void rotate_bins(complex float *freq, int N, int shift_bins, complex float *scratch)
{
	// The explicit bounds check below isn't for correctness (N is always
	// TX_PIPELINE_N, a positive compile-time constant, at this
	// function's one real call site) - it's what stops GCC's
	// -Wstringop-overflow range analysis from treating N as
	// unconstrained across this static helper being inlined two levels
	// deep into tx_pipeline_process_block. Without it, GCC can't
	// convince itself N stays non-negative (it's read from a plain int
	// struct field with no visible range restriction) and warns about a
	// memcpy bound that, taken as a negative int reinterpreted as
	// size_t, looks like ~16 exabytes - not anything actually reachable.
	if (N <= 0)
		return;
	memcpy(scratch, freq, (size_t)N * sizeof(complex float));
	for (int i = 0; i < N; i++)
		freq[(i + shift_bins + N) % N] = scratch[i];
}

void tx_pipeline_process_block(struct tx_pipeline *p, enum tx_pipeline_sideband sideband,
                                const float *in, float *out)
{
	struct filter *f = p->filt;
	complex float in_c[TX_PIPELINE_BLOCK_LEN];
	complex float out_c[TX_PIPELINE_BLOCK_LEN];

	// Real audio in, imaginary part 0 - same as real sbitx's own
	// fft_in[j] = i_sample (tx_process(), mj_zbitx/src/sbitx.c).
	for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++)
		in_c[i] = in[i];

	filter_forward(f, in_c);
	zero_sideband(f->freq, f->N, sideband);
	rotate_bins(f->freq, f->N, TX_IF_SHIFT_BINS, p->rotate_scratch);
	filter_inverse(f, out_c);

	// --- Per-block phase-continuity correction -----------------------
	// A bin-rotate isn't "free" the way it first looks: rotating each
	// block's own N-point spectrum by k bins modulates that block's
	// LOCAL time samples (index m = 0..N-1) by e^(j*2*pi*k*m/N) - but
	// overlap-save's valid output for block b sits at *local* indices
	// m = M-1..N-1 every single block, while the *global* sample index
	// those correspond to keeps advancing by L each block
	// (global = b*L - (M-1) + m). Substituting m = global - b*L + (M-1)
	// splits the per-sample modulation into the continuous term this
	// pipeline actually wants (e^(j*2*pi*k*global/N), a clean, steady
	// IF shift) times a per-BLOCK constant e^(j*2*pi*k*(M-1-b*L)/N) that
	// is NOT the same from one block to the next unless k*L/N happens
	// to be an integer. Left uncorrected, that extra per-block phase
	// silently rotates the carrier's phase by a different, unrelated
	// amount at every block boundary - not a subtle rounding error, a
	// real, measured bug: an early version of this function without
	// this correction measured the wanted tone at -97.75dB instead of
	// ~0dB (docs/ARCHITECTURE.md §10 step 4), because for this
	// pipeline's fixed L=1024/N=2048 (L/N always exactly 1/2 - see
	// tx_pipeline.h), k*L/N = k/2 - an integer only when k is even, and
	// TX_IF_SHIFT_BINS (467) is odd, so the uncorrected version was
	// flipping the carrier's sign by exactly 180 degrees every other
	// block, and a long coherent measurement across many blocks (the
	// only way to get a clean reading on a non-bin-aligned real tone
	// like CW_PITCH_HZ - see tx_pipeline_test.c) averages a signal
	// that's alternating sign to almost nothing.
	//
	// The fix only needs to cancel the per-block factor
	// e^(j*2*pi*k*(M-1-b*L)/N); the (M-1)/N part is a fixed constant
	// (same every block - just an arbitrary, harmless overall starting
	// phase) and the -b*L/N part is what actually varies with b. Since
	// L/N is exactly 1/2 here, k*b*L/N = k*b/2 is always an exact
	// integer or exact half-integer - never any other fraction - so
	// e^(j*pi*k*b) collapses to a plain +-1 (no trig, no float drift):
	// +1 whenever k is even (no correction ever needed) or whenever b
	// is even, and -1 only when BOTH k and b are odd. p->block_count
	// (incremented once per call) is exactly the running b this needs;
	// if this pipeline's fixed block/impulse-length choice
	// (TX_PIPELINE_BLOCK_LEN/IMPULSE_LEN, tx_pipeline.h) ever changes
	// away from an exact 2:1 N:L ratio, this simplification stops
	// applying and the general e^(j*2*pi*k*b*L/N) correction (a real
	// phase, not just a sign) would need implementing instead.
	long b = p->block_count++;
	int flip = (TX_IF_SHIFT_BINS % 2 != 0) && (b % 2 != 0);

	// The bin-zero+rotate above turned f->freq into a frequency-shifted
	// analytic (one-sided-spectrum) signal - taking the real part here
	// is what actually produces the real IF waveform the DAC wants; see
	// docs/ARCHITECTURE.md §4's "mathematically the same job a
	// Hilbert-transform phasing exciter does" note.
	//
	// The x2 factor: a real input tone A*cos(wn) splits into two equal
	// complex exponentials of amplitude A/2 each (at +f and -f);
	// zero_sideband() above deliberately throws one of those A/2 halves
	// away, and crealf() of what's left reproduces only that surviving
	// A/2, not A - an inherent, well-known property of analytic-signal
	// SSB construction (discard half the energy, and the real part of a
	// single complex exponential of magnitude A/2 stays A/2), not
	// specific to this implementation. Left uncorrected this cost a
	// real, measured -6.02dB (~0.5x) here - caught the same way step
	// 2's fft_filter.c 1/N-vs-1/N^2 gain bug was: by literally measuring
	// it against an expected ~0dB, not assuming unity gain. Correcting
	// it here (rather than leaving it to be silently absorbed into some
	// later, unrelated gain constant - see docs/ARCHITECTURE.md §10 step
	// 2's writeup on exactly that failure mode in real sbitx's own
	// filter_tune()) keeps this pipeline unity-gain by construction for
	// a full-amplitude real input tone, same discipline as fft_filter.c.
	for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++)
		out[i] = (flip ? -2.0f : 2.0f) * crealf(out_c[i]);
}

void tx_pipeline_free(struct tx_pipeline *p)
{
	filter_free(p->filt);
	free(p->rotate_scratch);
	free(p);
}
