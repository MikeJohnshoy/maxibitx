// fft_filter_test.c
//
// Standalone bench harness for fft_filter.c/.h - synthetic complex
// tones through the filter, checked against known dB targets, before
// this ever touches real hardware. Same methodology
// docs/dsp_design_notes/rx_audio_demod_design.md describes for
// test_rx_audio.c: NOT part of the normal build (see Makefile's
// separate `test-fft-filter` target, which doesn't touch `all`/SRC),
// run by hand:
//
//   make test-fft-filter && ./test-fft-filter
//
// Four cases, in order of how directly they matter to
// docs/ARCHITECTURE.md's decisions:
//
//   C - Sideband separation (the central decision, §4): a tone in the
//       wanted half and a tone in the unwanted half of a *symmetric*
//       passband, first with no bin-zeroing (both survive - the filter
//       alone can't tell them apart), then with the explicit
//       "zero the unwanted half of the bins" step real sbitx's
//       tx_process() uses. Confirms the explicit zero, not the filter's
//       own rolloff, is what's actually doing the separation.
//   B - Stopband attenuation: a tone well outside a realistic SSB
//       passband, checked against the ~54dB target beta=5 implies
//       (docs/ARCHITECTURE.md §4's Kaiser-beta derivation).
//   D - Transition width: sweeps a tone across the passband edge and
//       finds where it crosses -54dB, checked against the ~300Hz/side
//       the same derivation predicted from N=1025, Fs=96000.
//   A - Passband: a tone well inside the passband survives near 0dB -
//       the sanity check the other three assume already holds.
//
// All at minibitx's real sample rate and block size (96000 Hz,
// L=1024 - sound.c's PERIOD_FRAMES) so every Hz/dB number below is
// what the real audio thread would actually see, not a scaled-down
// stand-in.

#include <stdio.h>
#include <math.h>
#include "fft_filter.h"

#define FS 96000.0f
#define BLOCK_LEN 1024
#define IMPULSE_LEN 1025
#define KAISER_BETA 5.0f

// A continuous complex tone generator - keeps phase across calls so
// consecutive blocks are one continuous sinusoid, the same way a real
// baseband signal would look to the filter across a block boundary.
struct tone {
	double phase;
	double phase_inc; // radians/sample
};

static void tone_init(struct tone *t, float freq_hz)
{
	t->phase = 0;
	t->phase_inc = 2.0 * M_PI * freq_hz / FS;
}

static void tone_gen(struct tone *t, complex float *out, int n)
{
	for (int i = 0; i < n; i++) {
		out[i] = cosf(t->phase) + I * sinf(t->phase);
		t->phase += t->phase_inc;
		if (t->phase > 2.0 * M_PI)
			t->phase -= 2.0 * M_PI;
	}
}

static float mean_abs(const complex float *buf, int n)
{
	float sum = 0;
	for (int i = 0; i < n; i++)
		sum += cabsf(buf[i]);
	return sum / n;
}

// Zeros the negative-frequency half of an N-point frequency-domain
// buffer - bins n > N/2, same convention filter_tune() uses (folded
// negative frequencies live in the upper half). This is exactly the
// "zero out the other sideband" step from real sbitx's tx_process(),
// pulled out here so case C can turn it on and off.
static void zero_negative_freqs(complex float *freq, int N)
{
	for (int i = N / 2 + 1; i < N; i++)
		freq[i] = 0;
}

// Runs `settle_blocks` blocks through the filter to let the overlap-save
// history and any filter-edge transient die out, then measures mean
// |output| over one more block. If `zero_neg` is set, zeros the
// negative-frequency half of f->freq between filter_forward() and
// filter_inverse() on every block (case C's explicit-zero step).
static float steady_state_output(struct filter *f, struct tone *t, int settle_blocks, int zero_neg)
{
	complex float in[BLOCK_LEN], out[BLOCK_LEN];
	float result = 0;
	for (int b = 0; b < settle_blocks + 1; b++) {
		tone_gen(t, in, BLOCK_LEN);
		filter_forward(f, in);
		if (zero_neg)
			zero_negative_freqs(f->freq, f->N);
		filter_inverse(f, out);
		if (b == settle_blocks)
			result = mean_abs(out, BLOCK_LEN);
	}
	return result;
}

// Same idea, but sums TWO simultaneous tones' input each block (case
// C needs a wanted and an unwanted tone present at once, not tested
// separately - separation is only a meaningful test if both are there
// for the filter to actually have to distinguish).
static float steady_state_output_2tone(struct filter *f, struct tone *wanted, struct tone *unwanted,
                                         float measure_wanted, int settle_blocks, int zero_neg)
{
	complex float a[BLOCK_LEN], b_[BLOCK_LEN], sum[BLOCK_LEN], out[BLOCK_LEN];
	float result = 0;
	for (int blk = 0; blk < settle_blocks + 1; blk++) {
		tone_gen(wanted, a, BLOCK_LEN);
		tone_gen(unwanted, b_, BLOCK_LEN);
		for (int i = 0; i < BLOCK_LEN; i++)
			sum[i] = a[i] + b_[i];
		filter_forward(f, sum);
		if (zero_neg)
			zero_negative_freqs(f->freq, f->N);
		filter_inverse(f, out);
		if (blk == settle_blocks) {
			// Demodulate the tone we're asked to measure back to DC by
			// mixing it down, then average - isolates its amplitude
			// from the other tone's, rather than measuring their
			// (interfering) sum directly.
			double ph = 0, inc = -2.0 * M_PI * measure_wanted / FS;
			complex float acc = 0;
			for (int i = 0; i < BLOCK_LEN; i++) {
				acc += out[i] * (cosf(ph) + I * sinf(ph));
				ph += inc;
			}
			result = cabsf(acc) / BLOCK_LEN;
		}
	}
	return result;
}

static float to_db(float measured, float reference)
{
	if (measured <= 0)
		return -300.0f; // effectively -inf, clamp for printing
	return 20.0f * log10f(measured / reference);
}

int main(void)
{
	printf("fft_filter.c bench - Fs=%.0f Hz, L=%d, M=%d, N=%d (docs/ARCHITECTURE.md step 2)\n\n",
	       FS, BLOCK_LEN, IMPULSE_LEN, BLOCK_LEN + IMPULSE_LEN - 1);

	// --- Case A: passband -------------------------------------------
	{
		struct filter *f = filter_new(BLOCK_LEN, IMPULSE_LEN);
		filter_tune(f, 300.0f / FS, 3000.0f / FS, KAISER_BETA);
		struct tone t;
		tone_init(&t, 1500.0f); // dead center of a 300-3000Hz passband
		float out = steady_state_output(f, &t, 4, 0);
		printf("A. Passband (1500 Hz in a 300-3000 Hz band):  %.2f dB (want ~0 dB)\n", to_db(out, 1.0f));
		filter_free(f);
	}

	// --- Case B: stopband attenuation ---------------------------------
	{
		struct filter *f = filter_new(BLOCK_LEN, IMPULSE_LEN);
		filter_tune(f, 300.0f / FS, 3000.0f / FS, KAISER_BETA);
		struct tone t;
		tone_init(&t, 6000.0f); // well past the transition from 3000Hz
		float out = steady_state_output(f, &t, 4, 0);
		printf("B. Stopband (6000 Hz, same 300-3000 Hz band): %.2f dB (want <= ~-54 dB, beta=%.1f)\n",
		       to_db(out, 1.0f), KAISER_BETA);
		filter_free(f);
	}

	// --- Case C: sideband separation (the central decision) ----------
	{
		// A wide, SYMMETRIC passband (+-3000Hz) that does NOT by itself
		// distinguish "positive 1000Hz" from "negative 1000Hz" - both
		// sit well inside it. Only the explicit bin-zero step (mimicking
		// real sbitx's "zero out the LSB"/"zero out the USB") should be
		// able to tell them apart.
		struct filter *f = filter_new(BLOCK_LEN, IMPULSE_LEN);
		filter_tune(f, -3000.0f / FS, 3000.0f / FS, KAISER_BETA);

		// Bin-aligned frequencies (exact multiples of Fs/BLOCK_LEN =
		// 93.75 Hz), not a round "1000 Hz": steady_state_output_2tone()
		// measures one tone's amplitude by mixing it to DC and averaging
		// over exactly one BLOCK_LEN-sample window, which is an exact,
		// leakage-free single-bin DFT correlation ONLY when both tones
		// sit on that window's own bin grid (DFT basis functions are
		// orthogonal over an integer number of cycles; anything else
		// leaks between bins). At the >50dB rejection this case is
		// trying to measure, that leakage would swamp the real signal -
		// so 1031.25 Hz (bin 11) stands in for "~1000 Hz" here.
#define C_BIN_HZ (11.0f * FS / BLOCK_LEN)
		struct tone wanted, unwanted;
		tone_init(&wanted, C_BIN_HZ);    // the "USB-side" tone
		tone_init(&unwanted, -C_BIN_HZ); // the "LSB-side" tone that must be rejected

		float wanted_no_zero   = steady_state_output_2tone(f, &wanted, &unwanted, C_BIN_HZ, 4, 0);
		tone_init(&wanted, C_BIN_HZ);
		tone_init(&unwanted, -C_BIN_HZ);
		float unwanted_no_zero = steady_state_output_2tone(f, &unwanted, &wanted, -C_BIN_HZ, 4, 0);

		tone_init(&wanted, C_BIN_HZ);
		tone_init(&unwanted, -C_BIN_HZ);
		float wanted_zeroed   = steady_state_output_2tone(f, &wanted, &unwanted, C_BIN_HZ, 4, 1);
		tone_init(&wanted, C_BIN_HZ);
		tone_init(&unwanted, -C_BIN_HZ);
		float unwanted_zeroed = steady_state_output_2tone(f, &unwanted, &wanted, -C_BIN_HZ, 4, 1);

		printf("C. Sideband separation, +-%.2f Hz tones in a +-3000 Hz passband:\n", C_BIN_HZ);
		printf("   without bin-zero: wanted %.2f dB, unwanted %.2f dB (want: nearly equal - filter alone can't separate them)\n",
		       to_db(wanted_no_zero, 1.0f), to_db(unwanted_no_zero, 1.0f));
		printf("   with    bin-zero: wanted %.2f dB, unwanted %.2f dB (want: wanted ~0 dB, unwanted << B's filter-alone number)\n",
		       to_db(wanted_zeroed, 1.0f), to_db(unwanted_zeroed, 1.0f));
		filter_free(f);
	}

	// --- Case D: transition width sweep -------------------------------
	{
		struct filter *f = filter_new(BLOCK_LEN, IMPULSE_LEN);
		filter_tune(f, 300.0f / FS, 3000.0f / FS, KAISER_BETA);
		printf("D. Transition sweep above the 3000 Hz edge (want ~-54 dB by ~+300 Hz, i.e. ~3300 Hz):\n");
		for (float hz = 3000.0f; hz <= 3500.0f; hz += 50.0f) {
			struct tone t;
			tone_init(&t, hz);
			float out = steady_state_output(f, &t, 4, 0);
			printf("   %6.0f Hz: %7.2f dB\n", hz, to_db(out, 1.0f));
		}
		filter_free(f);
	}

	return 0;
}
