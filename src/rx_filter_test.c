// rx_filter_test.c
//
// Standalone bench harness for rx_filter.c/.h - proves
// docs/ARCHITECTURE.md build order step 6's shared-pipeline RX narrow
// filter numerically before it ever touches rx_audio.c's real, live
// audio path, same "measure before touching hardware" discipline
// fft_filter_test.c (step 2) and tx_pipeline_test.c (step 4) used. NOT
// part of the normal build - see Makefile's separate `test-rx-filter`
// target:
//
//   make test-rx-filter && ./test-rx-filter
//
// What this deliberately does NOT do (mirrors step 4's own scope note -
// rx_audio.c is completely untouched by this step): call anything in
// rx_audio.c. This harness feeds rx_filter.c synthetic tones directly,
// standing in for rx_audio.c stage 2's real demodulated "audio" output.
//
// Case A - passband gain & real-output verification: confirms the
//   filter passes a tone at its own tuned pitch at ~0dB (unlike
//   tx_pipeline.c's one-sided sideband-zero step, this filter's
//   filter_tune_real() passband is mirrored/symmetric, so - unlike
//   tx_pipeline.c - NO x2 gain correction should be needed here; this
//   case checks that directly, by inspecting the pre-crealf() complex
//   output's imaginary part, not just assuming it away).
// Case B - shape/rejection sweep: measures the filter's actual response
//   at a range of offsets from pitch, reporting the -3dB/-20dB/-60dB
//   crossing points (interpolated between measured points, same
//   methodology step 2's transition-width number used) - the real,
//   bench-measured answer to rx_filter.h's derivation comment ("a
//   ~100Hz target transition"), not just trusting the Harris-formula
//   estimate that picked RX_FILTER_IMPULSE_LEN in the first place.
// Case C - live retune sanity check: confirms rx_filter_retune()
//   actually moves the passband at runtime (stronger than
//   tx_pipeline_retune()'s untested status per docs/ARCHITECTURE.md
//   step 4 - this is the first thing to actually call its RX
//   equivalent).
// Case D - group delay: measures (empirically, via an impulse response
//   peak, not a formula) the real added latency this filter's much
//   longer impulse response costs relative to the elliptic IIR it's
//   replacing (rx_audio.c's own header: ~2.6ms) - block-boundary
//   continuity itself is a property of the shared fft_filter.c engine
//   already proven in steps 2/4, not re-tested here.
// Case E - minimum phase: the three things rx_filter_set_min_phase()
//   claims, each measured rather than argued. That the magnitude
//   response really is the same one Case B measured (the whole basis for
//   calling it the same filter); that the group delay and the attack of
//   a keyed CW element actually improve, which is the only reason to
//   want it; and that the minimum-phase impulse still fits inside the
//   M-1 taps overlap-save discards, since unlike a windowed FIR it
//   decays rather than ending, and a tail past that point would wrap
//   into the output instead of being thrown away.

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "rx_filter.h"
#include "cw.h" // CW_PITCH_HZ

#define TEST_FS RX_FILTER_FS_HZ

// ---------------------------------------------------------------------
// Coherent demodulation, integrated across many blocks - same technique
// and same x2 convention as tx_pipeline_test.c's struct demod (see its
// own comment for the derivation); copied here rather than shared
// because these two harnesses stay deliberately independent, same "no
// hardware/harness deps between bench tools" precedent as the rest of
// this project's test code.
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

// Injects a tone at `target_hz` for settle_blocks+measure_blocks blocks,
// discards the first settle_blocks (overlap-save history/filter-edge
// transient - M-1=3072 samples ~= 3 blocks, settle_blocks stays >= 8 at
// every call site below for margin), coherently demodulates the rest at
// that same frequency - i.e. "how much of this exact input tone
// survives", the real, bench-measured frequency response at that point.
static double measure_offset(struct rx_filter *r, double target_hz,
                              int settle_blocks, int measure_blocks)
{
	float in[RX_FILTER_BLOCK_LEN], out[RX_FILTER_BLOCK_LEN];
	double phase = 0, phase_inc = 2.0 * M_PI * target_hz / TEST_FS;
	struct demod d;
	demod_init(&d, target_hz);

	for (int b = 0; b < settle_blocks + measure_blocks; b++) {
		for (int i = 0; i < RX_FILTER_BLOCK_LEN; i++) {
			in[i] = (float)cos(phase);
			phase += phase_inc;
			if (phase > 2.0 * M_PI)
				phase -= 2.0 * M_PI;
		}
		rx_filter_process_block(r, in, out);
		if (b >= settle_blocks)
			demod_feed(&d, out, RX_FILTER_BLOCK_LEN);
	}
	return demod_mag(&d);
}

// Case A's realness check - identical tone/settle bookkeeping to
// measure_offset() above, but reaches into struct rx_filter's (fully
// transparent, same as struct tx_pipeline) own r->filt to inspect the
// complex output BEFORE rx_filter_process_block()'s crealf() discards
// the imaginary part, tracking the largest |real| and |imag| samples
// seen once settled.
static void measure_realness(struct rx_filter *r, double tone_hz, int settle_blocks,
                              int measure_blocks, double *max_abs_real, double *max_abs_imag)
{
	complex float in_c[RX_FILTER_BLOCK_LEN], out_c[RX_FILTER_BLOCK_LEN];
	double phase = 0, phase_inc = 2.0 * M_PI * tone_hz / TEST_FS;
	*max_abs_real = 0;
	*max_abs_imag = 0;

	for (int b = 0; b < settle_blocks + measure_blocks; b++) {
		for (int i = 0; i < RX_FILTER_BLOCK_LEN; i++) {
			in_c[i] = (float)cos(phase);
			phase += phase_inc;
			if (phase > 2.0 * M_PI)
				phase -= 2.0 * M_PI;
		}
		filter_forward(r->filt, in_c);
		filter_inverse(r->filt, out_c);
		if (b >= settle_blocks) {
			for (int i = 0; i < RX_FILTER_BLOCK_LEN; i++) {
				double re = fabs(crealf(out_c[i]));
				double im = fabs(cimagf(out_c[i]));
				if (re > *max_abs_real) *max_abs_real = re;
				if (im > *max_abs_imag) *max_abs_imag = im;
			}
		}
	}
}

// Case D's group-delay measurement: settle to an all-zero steady state,
// feed one unit impulse at the start of a block, then track the global
// sample index of the largest |output| sample across that block and a
// few trailing ones - an empirical group-delay reading (the standard
// way to actually measure a filter's real delay), not the (M-1)/2
// linear-phase formula assumed on paper.
static double measure_group_delay_ms(struct rx_filter *r, int settle_blocks)
{
	enum { TRAIL_BLOCKS = 4 }; // (TRAIL_BLOCKS+1)*BLOCK_LEN=5120 comfortably exceeds
	                           // RX_FILTER_IMPULSE_LEN's ~1536-sample expected peak location
	float in[RX_FILTER_BLOCK_LEN], out[RX_FILTER_BLOCK_LEN];
	long global = 0, impulse_global_index, peak_index = 0;
	double peak_val = 0;

	for (int b = 0; b < settle_blocks; b++) {
		memset(in, 0, sizeof(in));
		rx_filter_process_block(r, in, out);
		global += RX_FILTER_BLOCK_LEN;
	}

	memset(in, 0, sizeof(in));
	in[0] = 1.0f;
	impulse_global_index = global;
	rx_filter_process_block(r, in, out);
	for (int i = 0; i < RX_FILTER_BLOCK_LEN; i++) {
		double v = fabs(out[i]);
		if (v > peak_val) { peak_val = v; peak_index = global + i; }
	}
	global += RX_FILTER_BLOCK_LEN;

	for (int b = 0; b < TRAIL_BLOCKS; b++) {
		memset(in, 0, sizeof(in));
		rx_filter_process_block(r, in, out);
		for (int i = 0; i < RX_FILTER_BLOCK_LEN; i++) {
			double v = fabs(out[i]);
			if (v > peak_val) { peak_val = v; peak_index = global + i; }
		}
		global += RX_FILTER_BLOCK_LEN;
	}

	return (double)(peak_index - impulse_global_index) / TEST_FS * 1000.0;
}

// Case E's attack measurement: settle to silence, then key a tone at the
// filter's own pitch with a 5ms raised-cosine ramp and hold it, tracking
// a 2ms running-RMS envelope. Returns the time from key-down to the
// envelope first reaching -3dB of its own settled level - so this is
// attack shape, independent of the filter's gain.
#define ATTACK_HOLD_BLOCKS 24
static double measure_attack_ms(struct rx_filter *r, double tone_hz, int settle_blocks)
{
	float in[RX_FILTER_BLOCK_LEN], out[RX_FILTER_BLOCK_LEN];
	static double sq[ATTACK_HOLD_BLOCKS * RX_FILTER_BLOCK_LEN];
	static double env[ATTACK_HOLD_BLOCKS * RX_FILTER_BLOCK_LEN];
	int total = ATTACK_HOLD_BLOCKS * RX_FILTER_BLOCK_LEN;
	int window = (int)(0.002 * TEST_FS);
	int ramp = (int)(0.005 * TEST_FS);
	double phase = 0, phase_inc = 2.0 * M_PI * tone_hz / TEST_FS;

	for (int b = 0; b < settle_blocks; b++) {
		memset(in, 0, sizeof(in));
		rx_filter_process_block(r, in, out);
	}

	// First pass: the keyed element, storing each output sample's square.
	for (int b = 0, n = 0; b < ATTACK_HOLD_BLOCKS; b++) {
		for (int i = 0; i < RX_FILTER_BLOCK_LEN; i++, n++) {
			double shape = (n < ramp) ? 0.5 - 0.5 * cos(M_PI * n / (double)ramp) : 1.0;
			in[i] = (float)(shape * cos(phase));
			phase += phase_inc;
		}
		rx_filter_process_block(r, in, out);
		for (int i = 0; i < RX_FILTER_BLOCK_LEN; i++)
			sq[b * RX_FILTER_BLOCK_LEN + i] = (double)out[i] * out[i];
	}

	// Second pass: those squares become the running RMS the threshold
	// search below needs.
	double acc = 0;
	for (int k = 0; k < total; k++) {
		acc += sq[k];
		if (k >= window)
			acc -= sq[k - window];
		env[k] = sqrt(acc / (k < window ? k + 1 : window));
	}

	double settled = env[total - 1];
	for (int k = 0; k < total; k++)
		if (env[k] >= settled * 0.70710678)
			return (double)k / TEST_FS * 1000.0;
	return -1.0;
}

// Case E's fit check: inverse-transforms the tuned response to its own
// impulse and reports how much of that impulse's energy lands past tap
// M-1 - the last tap overlap-save's discard region covers. A windowed FIR
// is zero there by construction; a minimum-phase one has to be made to
// fit (fft_filter.c's fade-and-truncate step), and this is what says
// whether it did.
static double measure_impulse_tail_db(struct rx_filter *r)
{
	struct filter *f = r->filt;
	complex float *buf = fftwf_alloc_complex(f->N);
	fftwf_plan rev = fftwf_plan_dft_1d(f->N, buf, buf, FFTW_BACKWARD, FFTW_ESTIMATE);
	double inside = 0, beyond = 0;

	memcpy(buf, f->fir_coeff, f->N * sizeof(complex float));
	fftwf_execute(rev);
	for (int n = 0; n < f->N; n++) {
		double e = (double)cabsf(buf[n]) * cabsf(buf[n]);
		if (n < f->M)
			inside += e;
		else
			beyond += e;
	}
	fftwf_destroy_plan(rev);
	fftwf_free(buf);
	return 10.0 * log10(beyond / (inside + beyond) + 1e-300);
}

int main(void)
{
	printf("rx_filter.c bench - Fs=%.0f Hz, N=%d, M=%d, CW_PITCH_HZ=%d Hz, default width=%.0f Hz "
	       "(docs/ARCHITECTURE.md step 6)\n\n",
	       (double)TEST_FS, RX_FILTER_N, RX_FILTER_IMPULSE_LEN, CW_PITCH_HZ, RX_FILTER_DEFAULT_WIDTH_HZ);

	// --- Case A: passband gain & real-output verification -------------
	{
		struct rx_filter *r = rx_filter_new((float)CW_PITCH_HZ, RX_FILTER_DEFAULT_WIDTH_HZ);
		double m = measure_offset(r, CW_PITCH_HZ, 8, 32);
		printf("A. Wanted tone at pitch (%d Hz): %.2f dB (want ~0 dB)\n", CW_PITCH_HZ, to_db(m, 1.0));

		double max_re, max_im;
		measure_realness(r, CW_PITCH_HZ, 8, 8, &max_re, &max_im);
		printf("   Real/imag check at steady state: max|real|=%.6f max|imag|=%.6f "
		       "(imag/real=%.2e - want negligible, unlike tx_pipeline.c's deliberate one-sided halving)\n",
		       max_re, max_im, max_im / max_re);
		rx_filter_free(r);
	}

	// --- Case B: shape/rejection sweep ---------------------------------
	{
		struct rx_filter *r = rx_filter_new((float)CW_PITCH_HZ, RX_FILTER_DEFAULT_WIDTH_HZ);
		double ref = measure_offset(r, CW_PITCH_HZ, 8, 16);
		printf("\nB. Shape sweep above pitch (%d Hz), reference (offset=0) = %.2f dB:\n",
		       CW_PITCH_HZ, to_db(ref, 1.0));

		double prev_off = 0, prev_db = to_db(ref, ref);
		double cross_3 = -1, cross_20 = -1, cross_60 = -1;
		for (int step = 1; step <= 60; step++) {
			double off = step * 10.0;
			double m = measure_offset(r, CW_PITCH_HZ + off, 8, 16);
			double db = to_db(m, ref);
			if (cross_3 < 0 && db <= -3.0 && prev_db > -3.0)
				cross_3 = prev_off + (off - prev_off) * (prev_db - (-3.0)) / (prev_db - db);
			if (cross_20 < 0 && db <= -20.0 && prev_db > -20.0)
				cross_20 = prev_off + (off - prev_off) * (prev_db - (-20.0)) / (prev_db - db);
			if (cross_60 < 0 && db <= -60.0 && prev_db > -60.0)
				cross_60 = prev_off + (off - prev_off) * (prev_db - (-60.0)) / (prev_db - db);
			prev_off = off;
			prev_db = db;
		}
		printf("   -3dB crossing:  %s\n", cross_3 >= 0 ? "" : "(not reached in swept range)");
		if (cross_3 >= 0) printf("   -3dB at +%.1f Hz from pitch (~-3dB width %.0f Hz total)\n", cross_3, 2 * cross_3);
		if (cross_20 >= 0) printf("   -20dB at +%.1f Hz from pitch\n", cross_20);
		if (cross_60 >= 0) printf("   -60dB at +%.1f Hz from pitch\n", cross_60);
		if (cross_3 >= 0 && cross_60 >= 0)
			printf("   shape factor (-60dB width : -3dB width) = %.2f : 1 (elliptic being replaced: ~1.9:1 at -60dB:-6dB)\n",
			       cross_60 / cross_3);

		double image_offset_db = to_db(measure_offset(r, CW_PITCH_HZ + 3000.0, 8, 16), ref);
		printf("   rejection at pitch+3000Hz (matches rx_audio.c's own elliptic bench point): %.2f dB\n",
		       image_offset_db);
		rx_filter_free(r);
	}

	// --- Case C: live retune sanity check ------------------------------
	{
		struct rx_filter *r = rx_filter_new((float)CW_PITCH_HZ, RX_FILTER_DEFAULT_WIDTH_HZ);
		double before_700 = to_db(measure_offset(r, 700.0, 8, 16), 1.0);
		double before_800 = to_db(measure_offset(r, 800.0, 8, 16), 1.0);

		int rc = rx_filter_retune(r, 800.0f, 150.0f);
		double after_700 = to_db(measure_offset(r, 700.0, 8, 16), 1.0);
		double after_800 = to_db(measure_offset(r, 800.0, 8, 16), 1.0);

		printf("\nC. Live retune (pitch 700->800 Hz, width 300->150 Hz), rx_filter_retune() rc=%d:\n", rc);
		printf("   700 Hz: %.2f dB -> %.2f dB (want: drops, now near/outside the new edge)\n", before_700, after_700);
		printf("   800 Hz: %.2f dB -> %.2f dB (want: stays ~0 dB, now the tuned pitch)\n", before_800, after_800);
		rx_filter_free(r);
	}

	// --- Case D: group delay --------------------------------------------
	{
		struct rx_filter *r = rx_filter_new((float)CW_PITCH_HZ, RX_FILTER_DEFAULT_WIDTH_HZ);
		double delay_ms = measure_group_delay_ms(r, 8);
		printf("\nD. Measured group delay (impulse-response peak): %.2f ms "
		       "(elliptic IIR being replaced: ~2.6ms - see rx_audio.c)\n", delay_ms);
		rx_filter_free(r);
	}

	// --- Case E: minimum phase ------------------------------------------
	{
		struct rx_filter *lin = rx_filter_new((float)CW_PITCH_HZ, RX_FILTER_DEFAULT_WIDTH_HZ);
		struct rx_filter *min = rx_filter_new((float)CW_PITCH_HZ, RX_FILTER_DEFAULT_WIDTH_HZ);
		int rc = rx_filter_set_min_phase(min, 1);

		printf("\nE. Minimum phase (rx_filter_set_min_phase() rc=%d):\n", rc);

		// Same magnitude? Compared against the linear-phase filter at the
		// same offsets, each normalized to its own gain at pitch, so this
		// is shape, not level.
		double ref_lin = measure_offset(lin, CW_PITCH_HZ, 8, 16);
		double ref_min = measure_offset(min, CW_PITCH_HZ, 8, 16);
		const double offsets[] = { 50, 100, 150, 200, 300, 600, 1400, 3000 };
		double worst = 0, worst_off = 0;
		printf("   offset   linear phase   minimum phase\n");
		for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
			double l = to_db(measure_offset(lin, CW_PITCH_HZ + offsets[i], 8, 16), ref_lin);
			double m = to_db(measure_offset(min, CW_PITCH_HZ + offsets[i], 8, 16), ref_min);
			printf("   %+5.0f Hz %11.2f dB %12.2f dB (%+.2f)\n", offsets[i], l, m, m - l);
			if (fabs(m - l) > worst) { worst = fabs(m - l); worst_off = offsets[i]; }
		}
		printf("   largest deviation %.2f dB, at +%.0f Hz (want: small everywhere -\n"
		       "   'same magnitude' is the claim this case exists to check)\n", worst, worst_off);
		printf("   gain at pitch: %.2f dB linear phase, %.2f dB minimum phase\n",
		       to_db(ref_lin, 1.0), to_db(ref_min, 1.0));

		// What it buys.
		printf("   group delay: %.2f ms -> %.2f ms\n",
		       measure_group_delay_ms(lin, 8), measure_group_delay_ms(min, 8));
		printf("   keyed element, key-down to -3dB of settled: %.1f ms -> %.1f ms\n",
		       measure_attack_ms(lin, CW_PITCH_HZ, 8), measure_attack_ms(min, CW_PITCH_HZ, 8));

		// What it costs, and the thing that would break silently.
		printf("   impulse energy past tap %d (overlap-save discards M-1): %.1f dB "
		       "linear phase, %.1f dB minimum phase\n", RX_FILTER_IMPULSE_LEN - 1,
		       measure_impulse_tail_db(lin), measure_impulse_tail_db(min));

		double max_re, max_im;
		measure_realness(min, CW_PITCH_HZ, 8, 8, &max_re, &max_im);
		printf("   real output preserved: imag/real = %.2e (Case A's check, re-run here -\n"
		       "   the conversion must not make this filter complex)\n", max_im / max_re);

		rx_filter_free(lin);
		rx_filter_free(min);
	}

	return 0;
}
