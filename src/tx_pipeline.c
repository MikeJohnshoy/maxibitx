// tx_pipeline.c
//
// See tx_pipeline.h for what this is and the IF-shift derivation. This
// file adds the two steps fft_filter.c doesn't provide: the sideband
// zero and the bin rotate.

#include <stdio.h>
#include <string.h>
#include "tx_pipeline.h"

struct tx_pipeline *tx_pipeline_new(void)
{
	struct tx_pipeline *p = malloc(sizeof(struct tx_pipeline));

	// FFTW_ESTIMATE, not FFTW_MEASURE: this runs inside
	// sound_thread_start() after playback is primed but before the audio
	// thread starts, and a MEASURE search there drained the playback buffer
	// into startup xruns (ARCHITECTURE.md §10 step 7). Set
	// MAXIBITX_TX_PIPELINE_FFTW_MEASURE to force MEASURE for comparison.
	const char *force_measure = getenv("MAXIBITX_TX_PIPELINE_FFTW_MEASURE");
	unsigned flags = (force_measure && *force_measure) ? FFTW_MEASURE : FFTW_ESTIMATE;
	printf("tx_pipeline: using %s for its FFTW plan (TX_PIPELINE_N=%d)%s\n",
	       flags == FFTW_MEASURE ? "FFTW_MEASURE" : "FFTW_ESTIMATE", TX_PIPELINE_N,
	       flags == FFTW_MEASURE ? " - MAXIBITX_TX_PIPELINE_FFTW_MEASURE set, expect a slower startup" : "");

	p->filt = filter_new_ex(TX_PIPELINE_BLOCK_LEN, TX_PIPELINE_IMPULSE_LEN, flags);
	// filter_tune_real(), not filter_tune(): see tx_pipeline_retune().
	filter_tune_real(p->filt, 300.0f / TX_PIPELINE_FS_HZ, 3000.0f / TX_PIPELINE_FS_HZ,
	                  TX_PIPELINE_KAISER_BETA);
	// Plain malloc: only memcpy'd by rotate_bins(), never passed to FFTW,
	// so FFTW's alignment isn't needed.
	p->rotate_scratch = malloc(p->filt->N * sizeof(complex float));
	p->block_count = 0;
	return p;
}

int tx_pipeline_retune(struct tx_pipeline *p, float low_hz, float high_hz, float fs_hz)
{
	// filter_tune_real(), not filter_tune(). filter_tune()'s passband is
	// one-sided (positive frequencies only), which leaves nothing for
	// TX_PIPELINE_KEEP_LOWER to keep - LSB put out 0W on air until this
	// changed (ARCHITECTURE.md §10 step 8; tx_pipeline_test.c Case D).
	return filter_tune_real(p->filt, low_hz / fs_hz, high_hz / fs_hz, TX_PIPELINE_KAISER_BETA);
}

// Sideband zero. Bins n <= N/2 are positive frequencies (kept for
// TX_PIPELINE_KEEP_UPPER); n > N/2 are negative (kept for
// TX_PIPELINE_KEEP_LOWER), the same convention as filter_tune().
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

// Circularly rotates the N-point spectrum by shift_bins - the frequency-
// domain equivalent of mixing with a complex exponential at
// shift_bins*Fs/N Hz. Uses the caller's scratch buffer, so no malloc on
// the audio path.
static void rotate_bins(complex float *freq, int N, int shift_bins, complex float *scratch)
{
	// Not needed for correctness (N is always TX_PIPELINE_N). It keeps
	// GCC's -Wstringop-overflow from treating N as possibly negative once
	// this is inlined, and warning about the memcpy size.
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

	// Real audio in, imaginary part 0 (as sbitx's tx_process()).
	for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++)
		in_c[i] = in[i];

	// Each half needs its own rotation to land on the filter center - see
	// TX_IF_SHIFT_HZ_LSB (tx_pipeline.h).
	int shift_bins = (sideband == TX_PIPELINE_KEEP_LOWER) ? TX_IF_SHIFT_BINS_LSB : TX_IF_SHIFT_BINS;

	filter_forward(f, in_c);
	zero_sideband(f->freq, f->N, sideband);
	rotate_bins(f->freq, f->N, shift_bins, p->rotate_scratch);
	filter_inverse(f, out_c);

	// Phase continuity. Rotating each block's spectrum by k bins modulates
	// the block's *local* samples by e^(j*2*pi*k*m/N), but overlap-save's
	// output starts L samples later in global time every block, which adds
	// a per-block phase of e^(-j*2*pi*k*b*L/N). With L/N exactly 1/2
	// (1024/2048) that factor is e^(-j*pi*k*b): +1 unless both k and b are
	// odd, then -1. Both rotations here are odd (467, 497), so without this
	// the carrier flips sign every other block (measured: the wanted tone
	// at -97.75dB, ARCHITECTURE.md §10 step 4). If the block/impulse sizes
	// ever change so that N != 2L, this needs a general phase correction,
	// not a sign.
	long b = p->block_count++;
	// Parity of the rotation actually applied this call.
	int flip = (shift_bins % 2 != 0) && (b % 2 != 0);

	// The real part of the rotated one-sided spectrum is the real IF
	// waveform. x2 because a real tone's energy is split evenly between +f
	// and -f and the sideband zero discarded one half; without it the
	// output is -6.02dB. Keeps the pipeline unity-gain, so power
	// calibration (sound.c's TX_GAIN_CORRECTION) doesn't have to absorb it.
	for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++)
		out[i] = (flip ? -2.0f : 2.0f) * crealf(out_c[i]);
}

void tx_pipeline_free(struct tx_pipeline *p)
{
	filter_free(p->filt);
	free(p->rotate_scratch);
	free(p);
}
