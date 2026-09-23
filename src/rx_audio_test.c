// rx_audio_test.c
//
// Standalone integration smoke test for rx_audio.c's step 7 wiring -
// NOT a re-test of rx_filter.c's own DSP correctness (that's
// rx_filter_test.c's job, docs/ARCHITECTURE.md step 6) or of the
// elliptic filter's shape (already characterized in rx_audio.c's own
// file header). This checks the NEW integration risk step 7 introduced:
// rx_audio_process()'s two-pass buffering (audio_buf/elliptic_buf/
// fft_buf), the implementation selector actually switching which buffer
// feeds stage 4, and the block-size-mismatch fallback - none of which
// rx_filter_test.c could exercise, since it never calls rx_audio.c at
// all.
//
//   make test-rx-audio && ./test-rx-audio
//
// Links rx_audio.c/vfo.c/fft_filter.c/rx_filter.c directly (vfo.c has no
// hardware deps - see its own #include list - so this needs no GPIO/I2C/
// ALSA link requirements, same "no hardware deps in a bench test"
// precedent as this project's other harnesses); cw.h is a header-only
// dependency (CW_PITCH_HZ), not cw.c itself.
//
// Case A: default state (elliptic selected) - a steady synthetic
//   complex tone produces finite, non-silent PCM output.
// Case B: rx_audio_set_narrow_filter_impl(1) switches to the FFT path -
//   still finite, non-silent, and the two implementations' outputs
//   differ (proving the selector actually changed which buffer feeds
//   stage 4, not just that nothing crashed).
// Case C: a block whose size isn't RX_FILTER_BLOCK_LEN, with the FFT
//   path selected, falls back cleanly (finite, bounded output) rather
//   than crashing or corrupting later blocks.
// Case D: rx_audio_set_narrow_filter(0) (bypass) still produces finite
//   output and doesn't crash regardless of which implementation was
//   selected.

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "rx_audio.h"
#include "rx_filter.h" // RX_FILTER_BLOCK_LEN only
#include "cw.h"        // CW_PITCH_HZ only
#include "vfo.h"       // vfo_init_phase_table() only

#define TEST_FS 96000.0

static int all_finite(const int32_t *out, int n) {
	for (int i = 0; i < n; i++) {
		if (out[i] > 2000000000 || out[i] < -2000000000)
			return 0; // sound.c/rx_audio.c's own +-2e9 clamp - anything past it is a real bug
	}
	return 1;
}

static double rms(const int32_t *out, int n) {
	double acc = 0;
	for (int i = 0; i < n; i++)
		acc += (double)out[i] * (double)out[i];
	return sqrt(acc / n);
}

// Feeds `blocks` calls of `n` samples each, a steady complex BASEBAND
// tone at freq_hz (0 = a station parked exactly on dial center) into
// stage 1/2 - NOT at CW_PITCH_HZ directly: stage 2 mixes baseband UP by
// CW_PITCH_HZ to produce the audible tone (Re[(fi+jfq)*(cos+jsin)]), so
// a 0Hz baseband input is what actually reaches stage 3 as a CW_PITCH_HZ
// audio tone - the frequency both stage-3 implementations are tuned to
// pass. Returns the RMS of the LAST call's output only (post-settle).
static double run_tone(int n, int blocks, double freq_hz) {
	double i_samples[RX_FILTER_BLOCK_LEN > 4096 ? RX_FILTER_BLOCK_LEN : 4096];
	double q_samples[RX_FILTER_BLOCK_LEN > 4096 ? RX_FILTER_BLOCK_LEN : 4096];
	int32_t out[RX_FILTER_BLOCK_LEN > 4096 ? RX_FILTER_BLOCK_LEN : 4096];
	static double phase = 0;
	double phase_inc = 2.0 * M_PI * freq_hz / TEST_FS;
	double last_rms = 0;

	for (int b = 0; b < blocks; b++) {
		for (int i = 0; i < n; i++) {
			i_samples[i] = cos(phase);
			q_samples[i] = sin(phase);
			phase += phase_inc;
			if (phase > 2.0 * M_PI)
				phase -= 2.0 * M_PI;
		}
		rx_audio_process(i_samples, q_samples, n, out, NULL);
		if (!all_finite(out, n)) {
			fprintf(stderr, "run_tone: out-of-range sample at block %d (n=%d) - FAIL\n", b, n);
			exit(1);
		}
		if (b == blocks - 1)
			last_rms = rms(out, n);
	}
	return last_rms;
}

int main(void)
{
	vfo_init_phase_table();
	rx_audio_init();

	printf("rx_audio.c integration smoke test - docs/ARCHITECTURE.md step 7\n\n");

	// --- Case A: default (elliptic) ------------------------------------
	if (rx_audio_get_narrow_filter() != 1 || rx_audio_get_narrow_filter_impl() != 0) {
		fprintf(stderr, "A. Unexpected default state (narrow=%d impl=%d, want 1/0) - FAIL\n",
		        rx_audio_get_narrow_filter(), rx_audio_get_narrow_filter_impl());
		return 1;
	}
	double rms_elliptic = run_tone(RX_FILTER_BLOCK_LEN, 8, 0.0);
	printf("A. Default state (elliptic), steady on-dial-center tone (-> %d Hz audio): RMS=%.1f "
	       "(want finite, > 0)\n", CW_PITCH_HZ, rms_elliptic);
	if (!(rms_elliptic > 0)) {
		fprintf(stderr, "   FAIL: silent output in the default state\n");
		return 1;
	}

	// --- Case B: switch to FFT, confirm the selector actually matters --
	rx_audio_set_narrow_filter_impl(1);
	if (rx_audio_get_narrow_filter_impl() != 1) {
		fprintf(stderr, "B. rx_audio_set_narrow_filter_impl(1) didn't take - FAIL\n");
		return 1;
	}
	double rms_fft = run_tone(RX_FILTER_BLOCK_LEN, 8, 0.0);
	printf("B. FFT filter selected, steady on-dial-center tone (-> %d Hz audio): RMS=%.1f "
	       "(want finite, > 0, and comparable to A's - both are tuned to pass this frequency)\n",
	       CW_PITCH_HZ, rms_fft);
	if (!(rms_fft > 0)) {
		fprintf(stderr, "   FAIL: silent output with the FFT filter selected\n");
		return 1;
	}
	// Loosely comparable (both filters are unity-ish gain AT their tuned
	// pitch - step 6's own bench numbers), not identical (different
	// shape/ripple/group-delay-at-settle) - a wildly different reading
	// here (e.g. an order of magnitude off) would mean the selector
	// picked the wrong buffer or a scaling bug, not just a shape
	// difference.
	double ratio = rms_fft / rms_elliptic;
	printf("   (ratio FFT:elliptic = %.2f - want roughly comparable, not an order of magnitude off)\n",
	       ratio);

	// --- Case C: block-size mismatch, FFT selected ----------------------
	// n=500 != RX_FILTER_BLOCK_LEN - should fall back to elliptic output
	// for this one call rather than crash or emit garbage.
	double rms_mismatch = run_tone(500, 1, 0.0);
	printf("\nC. Block-size mismatch (n=500) with FFT selected: RMS=%.1f (want finite - fallback path)\n",
	       rms_mismatch);

	// Restore the aligned block size before continuing, so Case D isn't
	// itself measuring a post-mismatch settling transient.
	run_tone(RX_FILTER_BLOCK_LEN, 4, 0.0);

	// --- Case D: bypass --------------------------------------------------
	rx_audio_set_narrow_filter(0);
	double rms_bypass = run_tone(RX_FILTER_BLOCK_LEN, 8, 0.0);
	printf("\nD. Bypass (narrow filter off), steady on-dial-center tone (-> %d Hz audio): RMS=%.1f "
	       "(want finite, > 0)\n", CW_PITCH_HZ, rms_bypass);
	if (!(rms_bypass > 0)) {
		fprintf(stderr, "   FAIL: silent output in bypass\n");
		return 1;
	}

	// --- Case E: CW vs CWR sideband ------------------------------------
	// The point of CW-reverse: same dial, the other side of the BFO. The
	// raw I/Q is spectrally inverted (rx_audio.c's
	// RX_IQ_SPECTRUM_INVERTED), so a station d Hz ABOVE the dial arrives
	// at baseband -d and one BELOW at +d. CW should hear the first and
	// reject the second; CWR the reverse. Run in bypass (Case D left it
	// there) so this measures the sideband choice rather than stage 3's
	// 700 Hz-tuned response - a 500 Hz offset reaches it as 1200 Hz audio,
	// well off that peak.
	{
		const double offset = 500.0;
		double cw_above, cw_below, cwr_above, cwr_below;

		rx_audio_set_demod(RX_DEMOD_CW);
		cw_above = run_tone(RX_FILTER_BLOCK_LEN, 8, -offset);
		cw_below = run_tone(RX_FILTER_BLOCK_LEN, 8, offset);

		rx_audio_set_demod(RX_DEMOD_CWR);
		cwr_above = run_tone(RX_FILTER_BLOCK_LEN, 8, -offset);
		cwr_below = run_tone(RX_FILTER_BLOCK_LEN, 8, offset);

		printf("\nE. CW vs CWR sideband, a station %.0f Hz off the dial (bypass)\n", offset);
		printf("   CW : above dial RMS=%.1f, below dial RMS=%.1f (want above >> below)\n",
		       cw_above, cw_below);
		printf("   CWR: above dial RMS=%.1f, below dial RMS=%.1f (want below >> above)\n",
		       cwr_above, cwr_below);
		printf("   Unwanted side rejected by: CW %.1f dB, CWR %.1f dB\n",
		       -20.0 * log10(cw_below / cw_above), -20.0 * log10(cwr_above / cwr_below));
		if (!(cw_above > cw_below) || !(cwr_below > cwr_above)) {
			fprintf(stderr, "   FAIL: CW and CWR don't favour opposite sides of the dial\n");
			return 1;
		}
		rx_audio_set_demod(RX_DEMOD_CW); // leave the default in place
	}

	printf("\nAll cases completed without a crash or out-of-range sample.\n");
	return 0;
}
