// tx_pipeline_test.c
//
// Standalone bench harness for tx_pipeline.c/.h - proves
// docs/ARCHITECTURE.md build order step 4's shared-pipeline CW TX path
// numerically, the same "measure before touching hardware" discipline
// fft_filter_test.c (step 2) used. NOT part of the normal build - see
// Makefile's separate `test-tx-pipeline` target:
//
//   make test-tx-pipeline && ./test-tx-pipeline
//
// What this deliberately does NOT do (see docs/ARCHITECTURE.md's step 4
// scope note - cw.c/radio.c/sound.c are all untouched by this step):
// call anything in cw.c, radio.c, radio_hw.c, or sound.c. This harness
// stands in for cw.c's real sidetone with its own synthetic, continuous
// CW_PITCH_HZ tone (cw.h's #define - the one real constant this harness
// shares with cw.c) rather than linking cw.c itself, which would pull in
// radio.c/radio_hw.c's GPIO/I2C/ALSA link requirements a bench build has
// no business needing - same "no hardware deps in a bench test"
// precedent fft_filter_test.c already set.
//
// Case A - IF placement accuracy: confirms the shared bin-rotate lands
//   CW's tone at the frequency tx_pipeline.h's TX_IF_SHIFT_CW_BINS
//   derivation predicts, and reports the rounding residual against the
//   *old* direct-NCO scheme's fixed, much larger 700Hz one (cw.c's
//   TX_IF_OFFSET_HZ comment / docs/03_tx_processing_pipeline.md's "Known
//   limitations").
// Case B - image rejection at the FFT-domain sideband-zero step: the
//   central docs/ARCHITECTURE.md §4 claim, CW's own slice of it - same
//   "without the zero, both sides survive; with it, one drops hard"
//   structure as fft_filter_test.c's case C, but using CW_PITCH_HZ's
//   real, non-bin-aligned 700Hz rather than a substituted bin-aligned
//   stand-in - see measure_tone()'s comment for how leakage is handled
//   instead (long coherent integration, not a fudged test frequency).
// Case C - block-boundary sanity check for a keying transition (§9):
//   confirms a key-up transition landing mid-block, spanning an internal
//   1024-sample block edge, doesn't introduce a processing-block-sized
//   glitch on top of the envelope's own (expected) slope - overlap-save's
//   history carry-over (fft_filter.c) was already proven not to distort
//   a *steady* tone across a block boundary in step 2's bench; this
//   checks the same claim holds for a tone whose amplitude is actively
//   changing across that boundary too, which step 2 never exercised.
//   Uses a synthetic linear ramp standing in for cw.c's real
//   Blackman-Harris envelope table (private to cw.c, not exported) -
//   close enough to prove the pipeline itself adds no discontinuity; a
//   real on-air/scope check with the actual envelope table remains open
//   per §9.

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "tx_pipeline.h"
#include "cw.h" // CW_PITCH_HZ
#include "tone_gen.h"

#define TEST_FS TX_PIPELINE_FS_HZ

// ---------------------------------------------------------------------
// Coherent demodulation, integrated across many blocks rather than one.
//
// fft_filter_test.c's case C sidesteps CW_PITCH_HZ-style leakage by
// picking test frequencies that are exact multiples of its own
// measurement window's bin spacing. That trick isn't available here:
// CW_PITCH_HZ (700Hz) is a real, fixed, non-adjustable constant already
// baked into cw.h, and this harness is trying to prove the real system's
// behavior, not a bin-aligned stand-in for it. Integrating the
// demodulation coherently over many consecutive blocks instead sharpens
// the *effective* frequency resolution of the measurement itself
// (~1/(total seconds) - tens of Hz for a handful of blocks, under 1Hz
// over a second's worth) far past what's needed to resolve this test's
// wanted/image tones, which sit 2*CW_PITCH_HZ = 1400Hz apart - no need
// to fake a different CW_PITCH_HZ to get a clean number.
struct demod {
	double phase;
	double phase_inc;
	double acc_re, acc_im;
	long n;
};

static void demod_init(struct demod *d, double target_hz)
{
	d->phase = 0;
	d->phase_inc = -2.0 * M_PI * target_hz / TEST_FS;
	d->acc_re = d->acc_im = 0;
	d->n = 0;
}

static void demod_feed(struct demod *d, const float *x, int n)
{
	for (int i = 0; i < n; i++) {
		d->acc_re += x[i] * cos(d->phase);
		d->acc_im += x[i] * sin(d->phase);
		d->phase += d->phase_inc;
		if (d->phase < -2.0 * M_PI)
			d->phase += 2.0 * M_PI;
		d->n++;
	}
}

// Factor of 2: a real signal A*cos(2*pi*f*t) has amplitude A/2 at each
// of +f and -f in its (complex) spectrum - correlating against
// exp(-j2*pi*f*t) alone only picks up the +f half, so a bare
// |sum|/n reads A/2 for a unit-amplitude (A=1) real tone, not A. Doubling
// here keeps this function's output directly comparable to a unit input
// amplitude (0dB), matching to_db()'s convention below.
static double demod_mag(const struct demod *d)
{
	return 2.0 * sqrt(d->acc_re * d->acc_re + d->acc_im * d->acc_im) / (double)d->n;
}

static double to_db(double measured, double reference)
{
	if (measured <= 0)
		return -300.0;
	return 20.0 * log10(measured / reference);
}

// Runs a continuous CW_PITCH_HZ tone (steady key-down, envelope at max -
// same "settle, then measure" idea as fft_filter_test.c's
// steady_state_output()) through the real tx_pipeline for
// settle_blocks+measure_blocks blocks, discarding the first
// settle_blocks (overlap-save history/filter-edge transient) and
// coherently demodulating the rest at measure_hz.
static double measure_tone(struct tx_pipeline *p, enum tx_pipeline_signal sig,
                            double measure_hz, int settle_blocks, int measure_blocks)
{
	float in[TX_PIPELINE_BLOCK_LEN], out[TX_PIPELINE_BLOCK_LEN];
	double phase = 0;
	double phase_inc = 2.0 * M_PI * CW_PITCH_HZ / TEST_FS;
	struct demod d;
	demod_init(&d, measure_hz);

	for (int b = 0; b < settle_blocks + measure_blocks; b++) {
		for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++) {
			in[i] = (float)cos(phase);
			phase += phase_inc;
			if (phase > 2.0 * M_PI)
				phase -= 2.0 * M_PI;
		}
		tx_pipeline_process_block(p, sig, in, out);
		if (b >= settle_blocks)
			demod_feed(&d, out, TX_PIPELINE_BLOCK_LEN);
	}
	return demod_mag(&d);
}

// Case B's "without the zero" control path - built directly on
// fft_filter.h's struct filter (bypassing tx_pipeline.c entirely, same
// way fft_filter_test.c's own case C worked directly with struct filter
// rather than needing a production module to expose a "skip this step"
// backdoor real callers will never want).
static void raw_zero_negative(complex float *freq, int N)
{
	for (int i = N / 2 + 1; i < N; i++)
		freq[i] = 0;
}

static void raw_rotate(complex float *freq, int N, int shift_bins, complex float *scratch)
{
	// See tx_pipeline.c's rotate_bins() comment for why this guard is
	// here (a GCC -Wstringop-overflow false positive, not a real bounds
	// issue - N is always TX_PIPELINE_N at this function's call sites).
	if (N <= 0)
		return;
	memcpy(scratch, freq, (size_t)N * sizeof(complex float));
	for (int i = 0; i < N; i++)
		freq[(i + shift_bins + N) % N] = scratch[i];
}

static double measure_raw(struct filter *f, int apply_zero, double measure_hz,
                           int settle_blocks, int measure_blocks)
{
	complex float in_c[TX_PIPELINE_BLOCK_LEN], out_c[TX_PIPELINE_BLOCK_LEN];
	complex float scratch[TX_PIPELINE_N];
	float out_r[TX_PIPELINE_BLOCK_LEN];
	double phase = 0;
	double phase_inc = 2.0 * M_PI * CW_PITCH_HZ / TEST_FS;
	struct demod d;
	demod_init(&d, measure_hz);

	for (int b = 0; b < settle_blocks + measure_blocks; b++) {
		for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++) {
			in_c[i] = (float)cos(phase);
			phase += phase_inc;
			if (phase > 2.0 * M_PI)
				phase -= 2.0 * M_PI;
		}
		filter_forward(f, in_c);
		if (apply_zero)
			raw_zero_negative(f->freq, f->N);
		raw_rotate(f->freq, f->N, TX_IF_SHIFT_CW_BINS, scratch);
		filter_inverse(f, out_c);
		// Same per-block phase-continuity correction
		// tx_pipeline_process_block() applies (tx_pipeline.c) - without
		// it this control path has the identical -97.75dB-style bug,
		// which would corrupt this case's "wanted" measurement the same
		// way it corrupted Case A before that fix, not just the
		// "image"/rejection numbers this case is actually about.
		int flip = (TX_IF_SHIFT_CW_BINS % 2 != 0) && (b % 2 != 0);
		// Same x2 "recover the discarded sideband's amplitude" factor
		// tx_pipeline_process_block() applies (tx_pipeline.c) - without
		// it this case's own "wanted, with zero" reading would show the
		// same -6.02dB tx_pipeline.c's comment describes, not the ~0dB
		// this case expects to match Case A.
		for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++)
			out_r[i] = (flip ? -2.0f : 2.0f) * crealf(out_c[i]);
		if (b >= settle_blocks)
			demod_feed(&d, out_r, TX_PIPELINE_BLOCK_LEN);
	}
	return demod_mag(&d);
}

// Case E: runs tone_gen.c's output in mode m through the pipeline (upper
// sideband), then reports the level at each of the n IF frequencies in
// hz[] (into mags[], unit = a full-scale tone) and the output's peak
// absolute sample over the measured blocks. The level uses a
// Hann-windowed DFT rather than demod_mag(): the IMD check looks for
// something ~1200 Hz from a strong tone, and a rectangular window's
// leakage at that spacing (~-56 dB over these 16 blocks) would read as
// fake IMD.
#define TONE_GEN_SETTLE_BLOCKS 8
#define TONE_GEN_MEASURE_BLOCKS 16
static float tone_gen_capture[TONE_GEN_MEASURE_BLOCKS * TX_PIPELINE_BLOCK_LEN];

static void measure_tone_gen(enum tone_gen_mode m, enum tx_pipeline_signal sig,
                             const double *hz, double *mags, int n, double *peak)
{
	struct tx_pipeline *p = tx_pipeline_new();
	float in[TX_PIPELINE_BLOCK_LEN], out[TX_PIPELINE_BLOCK_LEN];
	const int len = TONE_GEN_MEASURE_BLOCKS * TX_PIPELINE_BLOCK_LEN;

	*peak = 0.0;
	tone_gen_set_mode(m);
	for (int b = 0; b < TONE_GEN_SETTLE_BLOCKS + TONE_GEN_MEASURE_BLOCKS; b++) {
		for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++)
			in[i] = (float)tone_gen_sample();
		tx_pipeline_process_block(p, sig, in, out);
		if (b < TONE_GEN_SETTLE_BLOCKS)
			continue;
		memcpy(&tone_gen_capture[(b - TONE_GEN_SETTLE_BLOCKS) * TX_PIPELINE_BLOCK_LEN], out,
		       sizeof(out));
		for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++)
			if (fabs(out[i]) > *peak)
				*peak = fabs(out[i]);
	}
	tone_gen_set_mode(TONE_GEN_OFF);
	tx_pipeline_free(p);

	for (int k = 0; k < n; k++) {
		double re = 0, im = 0, wsum = 0;
		for (int i = 0; i < len; i++) {
			double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / (len - 1));
			double ph = 2.0 * M_PI * hz[k] * i / TEST_FS;
			re += w * tone_gen_capture[i] * cos(ph);
			im -= w * tone_gen_capture[i] * sin(ph);
			wsum += w;
		}
		mags[k] = 2.0 * sqrt(re * re + im * im) / wsum; // real tone amplitude, as demod_mag()
	}
}

int main(void)
{
	float shift_actual_hz = TX_IF_SHIFT_CW_BINS * TX_PIPELINE_BIN_HZ;
	double target_wanted = CW_PITCH_HZ + shift_actual_hz;
	double target_image  = shift_actual_hz - CW_PITCH_HZ;
	float residual_hz = TX_IF_SHIFT_CW_HZ - shift_actual_hz;

	printf("tx_pipeline.c bench - Fs=%.0f Hz, N=%d, CW_PITCH_HZ=%d Hz (docs/ARCHITECTURE.md step 4)\n\n",
	       (double)TEST_FS, TX_PIPELINE_N, CW_PITCH_HZ);
	printf("TX_IF_SHIFT (CW, tone on dial): ideal %.3f Hz -> %d bins (%.4f Hz/bin) -> actual %.3f Hz, residual %.3f Hz\n",
	       TX_IF_SHIFT_CW_HZ, TX_IF_SHIFT_CW_BINS, TX_PIPELINE_BIN_HZ, shift_actual_hz, residual_hz);
	printf("TX_IF_SHIFT (SSB, carrier on dial): ideal %.3f Hz -> %d bins -> actual %.3f Hz, residual %.3f Hz\n",
	       TX_IF_SHIFT_SSB_HZ, TX_IF_SHIFT_SSB_BINS, TX_IF_SHIFT_SSB_BINS * TX_PIPELINE_BIN_HZ,
	       TX_IF_SHIFT_SSB_BINS * TX_PIPELINE_BIN_HZ - TX_IF_SHIFT_SSB_HZ);
	printf("(compare: the old direct-NCO scheme's fixed residual was %d Hz - cw.c's TX_IF_OFFSET_HZ,\n", CW_PITCH_HZ);
	printf(" corrected for explicitly in radio_tx_apply()'s clk2 formula - see docs/03_tx_processing_pipeline.md)\n\n");

	// --- Case A: IF placement accuracy --------------------------------
	{
		struct tx_pipeline *p = tx_pipeline_new();
		double m = measure_tone(p, TX_PIPELINE_CW, target_wanted, 8, 8);
		printf("A. Wanted tone at %.3f Hz (predicted placement): %.2f dB (want ~0 dB)\n",
		       target_wanted, to_db(m, 1.0));
		tx_pipeline_free(p);
	}

	// --- Case B: image rejection at the sideband-zero step ------------
	// A wide, SYMMETRIC +-3000Hz passband here, deliberately NOT the
	// real 300-3000Hz CW/USB one Case A/tx_pipeline_new() use: the real
	// passband is one-sided by construction, so it would already
	// suppress the -700Hz content mostly by itself, the same way
	// fft_filter_test.c's own case C picked a symmetric passband
	// specifically so the filter alone genuinely can't distinguish
	// +700Hz from -700Hz - only the explicit zero step can. Testing
	// "what the zero step buys you" against the asymmetric production
	// passband would just be re-measuring the passband's own asymmetry.
	{
		struct filter *f = filter_new(TX_PIPELINE_BLOCK_LEN, TX_PIPELINE_IMPULSE_LEN);
		filter_tune(f, -3000.0f / TEST_FS, 3000.0f / TEST_FS, TX_PIPELINE_KAISER_BETA);

		double wanted_no_zero = measure_raw(f, 0, target_wanted, 8, 64);
		double image_no_zero  = measure_raw(f, 0, target_image, 8, 64);
		double wanted_zeroed  = measure_raw(f, 1, target_wanted, 8, 64);
		double image_zeroed   = measure_raw(f, 1, target_image, 8, 64);

		printf("\nB. Image rejection - wanted %.3f Hz vs. the %.3f Hz mirror location the\n",
		       target_wanted, target_image);
		printf("   old un-zeroed spectrum would also put energy at:\n");
		printf("   without bin-zero: wanted %.2f dB, image %.2f dB (want: comparable - filter alone can't separate them)\n",
		       to_db(wanted_no_zero, 1.0), to_db(image_no_zero, 1.0));
		printf("   with    bin-zero: wanted %.2f dB, image %.2f dB (want: wanted ~0dB, image very deep)\n",
		       to_db(wanted_zeroed, 1.0), to_db(image_zeroed, 1.0));
		filter_free(f);
	}

	// --- Case C: block-boundary sanity check for a keying transition --
	{
		struct tx_pipeline *p = tx_pipeline_new();
		// 6 calls to tx_pipeline_process_block(), each one
		// TX_PIPELINE_BLOCK_LEN samples - the actual internal seam this
		// case needs to straddle is between two separate *calls*, not
		// just some index inside one big buffer. RAMP_START is chosen
		// so the ramp (480 samples - the same span cw.c's real
		// CW_ENVELOPE_LEN uses, ~5ms at 96kHz) is centered exactly on
		// the boundary between call 2's block and call 3's
		// (TX_PIPELINE_BLOCK_LEN*3 = sample 3072), with steady key-down
		// before it and steady key-up (silence) after.
		enum {
			NBLK = 6,
			RAMP_LEN = 480,
			RAMP_START = TX_PIPELINE_BLOCK_LEN * 3 - RAMP_LEN / 2,
		};
		static float in_env[TX_PIPELINE_BLOCK_LEN * NBLK];
		static float out_all[TX_PIPELINE_BLOCK_LEN * NBLK];
		double phase = 0, phase_inc = 2.0 * M_PI * CW_PITCH_HZ / TEST_FS;

		for (int i = 0; i < TX_PIPELINE_BLOCK_LEN * NBLK; i++) {
			double env;
			if (i < RAMP_START)
				env = 1.0;
			else if (i < RAMP_START + RAMP_LEN)
				env = 1.0 - (double)(i - RAMP_START) / RAMP_LEN;
			else
				env = 0.0;
			in_env[i] = (float)(env * cos(phase));
			phase += phase_inc;
			if (phase > 2.0 * M_PI)
				phase -= 2.0 * M_PI;
		}
		for (int b = 0; b < NBLK; b++)
			tx_pipeline_process_block(p, TX_PIPELINE_CW,
			                           in_env + b * TX_PIPELINE_BLOCK_LEN,
			                           out_all + b * TX_PIPELINE_BLOCK_LEN);

		// Second-derivative (delta-of-delta) magnitude, sample by sample,
		// over the ramp region only (where change is actually expected) -
		// flags any localized spike, not the expected slope itself.
		int ramp_lo = RAMP_START - 4, ramp_hi = RAMP_START + RAMP_LEN + 4;
		int block_edge = TX_PIPELINE_BLOCK_LEN * 3; // where block 3 -> 4 seam falls, inside the ramp
		double max_d2_overall = 0, max_d2_near_edge = 0;
		for (int i = ramp_lo + 1; i < ramp_hi - 1; i++) {
			double d2 = fabs((double)out_all[i + 1] - 2.0 * out_all[i] + out_all[i - 1]);
			if (d2 > max_d2_overall)
				max_d2_overall = d2;
			if (i >= block_edge - 8 && i <= block_edge + 8 && d2 > max_d2_near_edge)
				max_d2_near_edge = d2;
		}
		printf("\nC. Block-boundary check, key-up ramp crossing the internal %d-sample block seam:\n",
		       TX_PIPELINE_BLOCK_LEN);
		printf("   max |2nd derivative| across the whole ramp: %.6f\n", max_d2_overall);
		printf("   max |2nd derivative| within +-8 samples of the seam: %.6f (want: not >> the whole-ramp max)\n",
		       max_d2_near_edge);
		tx_pipeline_free(p);
	}

	// --- Case D: LSB's own placement -----------------------------------
	// Both sidebands share the SSB shift (tx_pipeline.h): LSB's kept
	// negative half lands the carrier on the same point USB's positive
	// half does, each band extending from it in its own direction. A
	// 1000Hz tone should therefore appear 1000Hz BELOW that carrier point,
	// and nothing 1000Hz above it - which is what separates LSB from USB.
	{
		float shift_ssb_hz = TX_IF_SHIFT_SSB_BINS * TX_PIPELINE_BIN_HZ;
		double lsb_wanted = shift_ssb_hz - TONE_GEN_SINGLE_HZ;
		double lsb_wrong_side = shift_ssb_hz + TONE_GEN_SINGLE_HZ;

		printf("\nD. LSB placement (same SSB shift as USB, mirrored content)\n");

		double hz[2] = { lsb_wanted, lsb_wrong_side };
		double m[2], peak;
		measure_tone_gen(TONE_GEN_SINGLE, TX_PIPELINE_LSB, hz, m, 2, &peak);
		printf("   1000 Hz tone at IF %.3f Hz (carrier - 1000): %.2f dB (want ~0 dB)\n",
		       lsb_wanted, to_db(m[0], 1.0));
		printf("   Same signal at IF %.3f Hz (carrier + 1000, the USB side): %.1f dB (want: very negative)\n",
		       lsb_wrong_side, to_db(m[1], 1.0));
	}

	// --- Case E: the test-tone generator (tone_gen.c) ---------------------
	// Single tone: full scale, so ~0 dB at its IF. Two-tone: 0.5 each, so
	// each tone ~-6.02 dB, the combined peak equal to the single tone's
	// (same PEP), and nothing at the third-order IMD locations - the
	// pipeline is linear, so IMD seen on air comes from the analog chain.
	// docs/dsp_design_notes/tx_test_tones_and_alc.md.
	{
		float shift_ssb = TX_IF_SHIFT_SSB_BINS * TX_PIPELINE_BIN_HZ;
		double hz1[1] = { shift_ssb + TONE_GEN_SINGLE_HZ };
		double m1[1], peak1;
		measure_tone_gen(TONE_GEN_SINGLE, TX_PIPELINE_USB, hz1, m1, 1, &peak1);

		double lo = TONE_GEN_TWO_LOW_HZ, hi = TONE_GEN_TWO_HIGH_HZ;
		double hz2[4] = { shift_ssb + lo, shift_ssb + hi,
		                  shift_ssb + 2 * lo - hi, shift_ssb + 2 * hi - lo };
		double m2[4], peak2;
		measure_tone_gen(TONE_GEN_TWO, TX_PIPELINE_USB, hz2, m2, 4, &peak2);

		printf("\nE. Test-tone generator (tone_gen.c), upper sideband\n");
		printf("   Single %.0f Hz at IF %.3f Hz: %.2f dB (want ~0 dB), peak %.4f\n",
		       TONE_GEN_SINGLE_HZ, hz1[0], to_db(m1[0], 1.0), peak1);
		printf("   Two-tone %.0f + %.0f Hz: %.2f / %.2f dB (want ~-6.02 dB each), peak %.4f\n",
		       lo, hi, to_db(m2[0], 1.0), to_db(m2[1], 1.0), peak2);
		printf("   Two-tone peak vs single-tone peak: %.2f dB (want ~0 dB - same PEP)\n",
		       to_db(peak2, peak1));
		printf("   IMD3 at carrier %+.0f Hz: %.1f dB, at carrier %+.0f Hz: %.1f dB (want: numerical floor)\n",
		       2 * lo - hi, to_db(m2[2], 1.0), 2 * hi - lo, to_db(m2[3], 1.0));
	}

	return 0;
}
