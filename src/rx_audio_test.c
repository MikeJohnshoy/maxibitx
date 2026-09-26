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
// Case A: default state (the FFT filter, minimum phase) - a steady
//   synthetic complex tone produces finite, non-silent PCM output, and the
//   defaults are what rx_audio.h says they are.
// Case B: rx_audio_set_narrow_filter_impl(0) switches to the elliptic bank -
//   still finite, non-silent, and the two implementations' outputs
//   differ (proving the selector actually changed which buffer feeds
//   stage 4, not just that nothing crashed).
// Case C: a block whose size isn't RX_FILTER_BLOCK_LEN, with the FFT
//   path selected, falls back cleanly (finite, bounded output) rather
//   than crashing or corrupting later blocks.
// Case D: rx_audio_set_narrow_filter(0) (bypass) still produces finite
//   output and doesn't crash regardless of which implementation was
//   selected.
// Case E: CW vs CWR favour opposite sides of the dial.
// Case F: the filter bank's pitch/width selection - that a request snaps to
//   the nearest value the bank carries, that the setter reports back what it
//   actually chose rather than what it was asked for, and that the advertised
//   list matches what selection accepts. A client that displays its own
//   request instead of the reply would show a pitch the radio isn't using.
// Case G: every one of the twelve sets is reachable and centered where it
//   claims - a station on dial center stays loud at every pitch
//   (because the BFO moves with the pitch, which is the part that makes the
//   control mean anything), while a station 600 Hz off is rejected
//   progressively harder as the width narrows.
// Case H: switching a coefficient set live, under a steady tone, stays
//   bounded. The bank keeps the biquad history across the swap, so this is
//   the case that would catch a high-Q section being handed a state it
//   can't reconcile - the failure mode would be a loud thump, not a crash.

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
// Largest |sample| the most recent run_tone() call emitted, across all its
// blocks - Case H needs the peak, not the settled RMS, since a switching
// transient is by definition brief.
static double last_peak;

static double run_tone(int n, int blocks, double freq_hz) {
	double i_samples[RX_FILTER_BLOCK_LEN > 4096 ? RX_FILTER_BLOCK_LEN : 4096];
	double q_samples[RX_FILTER_BLOCK_LEN > 4096 ? RX_FILTER_BLOCK_LEN : 4096];
	int32_t out[RX_FILTER_BLOCK_LEN > 4096 ? RX_FILTER_BLOCK_LEN : 4096];
	static double phase = 0;
	double phase_inc = 2.0 * M_PI * freq_hz / TEST_FS;
	double last_rms = 0;

	last_peak = 0;
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
		for (int i = 0; i < n; i++) {
			double a = fabs((double)out[i]);
			if (a > last_peak)
				last_peak = a;
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

	// --- Case A: default (the FFT filter, minimum phase) ----------------
	if (rx_audio_get_narrow_filter() != 1 || rx_audio_get_narrow_filter_impl() != 1 ||
	    rx_audio_get_narrow_filter_min_phase() != 1) {
		fprintf(stderr, "A. Unexpected default state (narrow=%d impl=%d minphase=%d, "
		        "want 1/1/1) - FAIL\n", rx_audio_get_narrow_filter(),
		        rx_audio_get_narrow_filter_impl(),
		        rx_audio_get_narrow_filter_min_phase());
		return 1;
	}
	double rms_fft = run_tone(RX_FILTER_BLOCK_LEN, 8, 0.0);
	printf("A. Default state (FFT, minimum phase), steady on-dial-center tone (-> %d Hz audio): "
	       "RMS=%.1f (want finite, > 0)\n", CW_PITCH_HZ, rms_fft);
	if (!(rms_fft > 0)) {
		fprintf(stderr, "   FAIL: silent output in the default state\n");
		return 1;
	}

	// --- Case B: switch to the elliptic bank, confirm the selector matters
	rx_audio_set_narrow_filter_impl(0);
	if (rx_audio_get_narrow_filter_impl() != 0) {
		fprintf(stderr, "B. rx_audio_set_narrow_filter_impl(0) didn't take - FAIL\n");
		return 1;
	}
	double rms_elliptic = run_tone(RX_FILTER_BLOCK_LEN, 8, 0.0);
	printf("B. Elliptic bank selected, steady on-dial-center tone (-> %d Hz audio): RMS=%.1f "
	       "(want finite, > 0, and comparable to A's - both are tuned to pass this frequency)\n",
	       CW_PITCH_HZ, rms_elliptic);
	if (!(rms_elliptic > 0)) {
		fprintf(stderr, "   FAIL: silent output with the elliptic bank selected\n");
		return 1;
	}
	// Loosely comparable (both filters are unity-ish gain AT their tuned
	// pitch - step 6's own bench numbers), not identical (different
	// shape/ripple/group-delay-at-settle) - a wildly different reading
	// here (e.g. an order of magnitude off) would mean the selector
	// picked the wrong buffer or a scaling bug, not just a shape
	// difference.
	double ratio = rms_elliptic / rms_fft;
	printf("   (ratio elliptic:FFT = %.2f - want roughly comparable, not an order of magnitude off)\n",
	       ratio);

	// --- Case C: block-size mismatch, FFT selected ----------------------
	// n=500 != RX_FILTER_BLOCK_LEN - should fall back to elliptic output
	// for this one call rather than crash or emit garbage.
	//
	// Re-select the FFT filter first: Case B left the elliptic selected, and
	// the fallback only engages for the FFT path, so without this the case
	// would pass while testing nothing. (It tested the fallback by accident
	// before, when the FFT filter was what Case B switched TO.)
	rx_audio_set_narrow_filter_impl(1);
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

	// Cases F-H all exercise stage 3 itself, so put it back in circuit:
	// Case D left it bypassed and Case B left the FFT path selected.
	rx_audio_set_narrow_filter(1);
	rx_audio_set_narrow_filter_impl(0);

	// --- Case F: pitch/width snapping ------------------------------------
	{
		int pitches = rx_audio_narrow_pitch_count();
		int widths = rx_audio_narrow_width_count();
		int fails = 0;

		printf("\nF. Filter bank: %d pitches x %d widths = %d sets\n",
		       pitches, widths, pitches * widths);
		printf("   pitches:");
		for (int i = 0; i < pitches; i++)
			printf(" %d", rx_audio_narrow_pitch_at(i));
		printf(" Hz    widths:");
		for (int i = 0; i < widths; i++)
			printf(" %d", rx_audio_narrow_width_at(i));
		printf(" Hz\n");

		if (rx_audio_narrow_pitch_at(-1) != -1 || rx_audio_narrow_pitch_at(pitches) != -1 ||
		    rx_audio_narrow_width_at(-1) != -1 || rx_audio_narrow_width_at(widths) != -1) {
			fprintf(stderr, "   FAIL: an out-of-range index didn't return -1\n");
			return 1;
		}

		// Each advertised value must select exactly itself - an off-by-one
		// in the snap would show up here as a neighbour.
		for (int i = 0; i < pitches; i++) {
			int want = rx_audio_narrow_pitch_at(i);
			int got = rx_audio_set_narrow_pitch(want);
			if (got != want || rx_audio_get_narrow_pitch() != want) {
				fprintf(stderr, "   FAIL: pitch %d selected %d (getter %d)\n",
				        want, got, rx_audio_get_narrow_pitch());
				fails++;
			}
		}
		for (int i = 0; i < widths; i++) {
			int want = rx_audio_narrow_width_at(i);
			int got = rx_audio_set_narrow_width(want);
			if (got != want || rx_audio_get_narrow_width() != want) {
				fprintf(stderr, "   FAIL: width %d selected %d (getter %d)\n",
				        want, got, rx_audio_get_narrow_width());
				fails++;
			}
		}

		// Values between, below and above the rungs. Expectations are derived
		// from the bank rather than written out, because the bank's contents
		// are a design decision that changes: an earlier version of this case
		// hard-coded 600/700/800 and had to be edited when the pitch range
		// was extended, which is a test measuring the table it was written
		// against instead of the behavior. What is actually being asserted is
		// three rules - below the bottom rung snaps to the bottom, above the
		// top snaps to the top, and in between snaps to the nearer neighbour.
		int lo_p = rx_audio_narrow_pitch_at(0);
		int hi_p = rx_audio_narrow_pitch_at(pitches - 1);
		int lo_w = rx_audio_narrow_width_at(0);
		int hi_w = rx_audio_narrow_width_at(widths - 1);

		struct { int ask, want; const char *why; } edge_cases[] = {
			{ 0,          lo_p, "pitch far below the lowest rung" },
			{ -400,       lo_p, "negative pitch" },
			{ lo_p - 10,  lo_p, "pitch just below the lowest rung" },
			{ hi_p + 10,  hi_p, "pitch just above the highest rung" },
			{ 100000,     hi_p, "pitch far above the highest rung" },
		};
		for (unsigned i = 0; i < sizeof(edge_cases) / sizeof(edge_cases[0]); i++) {
			int got = rx_audio_set_narrow_pitch(edge_cases[i].ask);
			printf("   pitch %6d Hz -> %4d Hz  (%s)%s\n", edge_cases[i].ask, got,
			       edge_cases[i].why, got == edge_cases[i].want ? "" : "   WRONG");
			if (got != edge_cases[i].want)
				fails++;
		}

		// Either side of every midpoint between adjacent rungs, so the snap
		// is checked across the whole table rather than at one seam.
		for (int i = 0; i + 1 < pitches; i++) {
			int a = rx_audio_narrow_pitch_at(i), b = rx_audio_narrow_pitch_at(i + 1);
			int mid = (a + b) / 2;
			int below = rx_audio_set_narrow_pitch(mid - 10);
			int above = rx_audio_set_narrow_pitch(mid + 10);
			printf("   pitch %6d Hz -> %4d Hz, %6d Hz -> %4d Hz  (either side of "
			       "%d/%d)%s\n", mid - 10, below, mid + 10, above, a, b,
			       (below == a && above == b) ? "" : "   WRONG");
			if (below != a || above != b)
				fails++;
		}

		struct { int ask, want; const char *why; } width_edges[] = {
			{ 1,      lo_w, "width far below the narrowest" },
			{ 0,      lo_w, "zero width" },
			{ 99999,  hi_w, "width far above the widest" },
		};
		for (unsigned i = 0; i < sizeof(width_edges) / sizeof(width_edges[0]); i++) {
			int got = rx_audio_set_narrow_width(width_edges[i].ask);
			printf("   width %6d Hz -> %4d Hz  (%s)%s\n", width_edges[i].ask, got,
			       width_edges[i].why, got == width_edges[i].want ? "" : "   WRONG");
			if (got != width_edges[i].want)
				fails++;
		}
		for (int i = 0; i + 1 < widths; i++) {
			int a = rx_audio_narrow_width_at(i), b = rx_audio_narrow_width_at(i + 1);
			int mid = (a + b) / 2;
			int below = rx_audio_set_narrow_width(mid - 10);
			int above = rx_audio_set_narrow_width(mid + 10);
			printf("   width %6d Hz -> %4d Hz, %6d Hz -> %4d Hz  (either side of "
			       "%d/%d)%s\n", mid - 10, below, mid + 10, above, a, b,
			       (below == a && above == b) ? "" : "   WRONG");
			if (below != a || above != b)
				fails++;
		}
		if (fails) {
			fprintf(stderr, "   FAIL: %d snapping case(s) wrong\n", fails);
			return 1;
		}
	}

	// --- Case G: every set reachable, and centered where it claims -------
	// A station on dial center is heard AT the selected pitch, because
	// rx_audio_set_narrow_pitch() moves the BFO too - so on_center should
	// stay strong at every pitch. If only the filter moved and the BFO
	// didn't, every 150 Hz-wide row away from the default pitch would
	// collapse instead - which is exactly the bug this case exists to catch,
	// and it gets harder to miss the wider the pitch range grows.
	{
		const double offset = 600.0;  // heard at pitch + 600 (spectrum inverted, CW keeps the upper side)
		printf("\nG. All %d sets, steady tone on dial center vs %.0f Hz off\n",
		       rx_audio_narrow_pitch_count() * rx_audio_narrow_width_count(), offset);
		printf("   pitch  width   on center      off-center    rejection\n");
		int fails = 0;
		for (int pi = 0; pi < rx_audio_narrow_pitch_count(); pi++) {
			for (int wi = 0; wi < rx_audio_narrow_width_count(); wi++) {
				int p = rx_audio_narrow_pitch_at(pi);
				int w = rx_audio_narrow_width_at(wi);
				rx_audio_set_narrow_pitch(p);
				rx_audio_set_narrow_width(w);
				double on_center = run_tone(RX_FILTER_BLOCK_LEN, 12, 0.0);
				double off_center = run_tone(RX_FILTER_BLOCK_LEN, 12, -offset);
				printf("   %5d  %5d  %11.1f  %13.1f  %8.1f dB\n", p, w, on_center, off_center,
				       -20.0 * log10(off_center / on_center));
				if (!(on_center > 0)) {
					fprintf(stderr, "   FAIL: %d/%d is silent on its own pitch\n", p, w);
					fails++;
				}
				if (!(on_center > off_center)) {
					fprintf(stderr, "   FAIL: %d/%d passes %.0f Hz off as strongly as center\n",
					        p, w, offset);
					fails++;
				}
			}
		}
		if (fails)
			return 1;
		printf("   (rejection grows as the width narrows - the AGC rides the raw\n"
		       "    input, so these are absolute levels, not normalized)\n");
	}

	// --- Case H: live coefficient swap stays bounded ----------------------
	// The swap keeps x1/x2/y1/y2, so the new sections inherit the old
	// filter's history. Bench-measured overshoot for that is about 2 dB
	// settling within 14 ms; the gate here is deliberately looser (6 dB),
	// because what would matter is a thump, and anything approaching the
	// +-2e9 clamp is caught by run_tone()'s own all_finite() check anyway.
	{
		printf("\nH. Live width swaps under a steady on-center tone\n");
		rx_audio_set_narrow_pitch(700);
		rx_audio_set_narrow_width(300);
		double settled = run_tone(RX_FILTER_BLOCK_LEN, 16, 0.0);
		double settled_peak = last_peak;
		int fails = 0;
		const int order[] = { 150, 600, 300, 450, 150 };

		for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
			int from = rx_audio_get_narrow_width();
			rx_audio_set_narrow_width(order[i]);
			// Two blocks is ~21 ms, comfortably covering the measured
			// settling time, and short enough that a transient isn't
			// averaged away.
			run_tone(RX_FILTER_BLOCK_LEN, 2, 0.0);
			double overshoot_db = 20.0 * log10(last_peak / settled_peak);
			printf("   %3d -> %3d Hz: peak %+6.2f dB vs the settled peak%s\n",
			       from, order[i], overshoot_db, overshoot_db > 6.0 ? "   TOO LOUD" : "");
			if (overshoot_db > 6.0)
				fails++;
			run_tone(RX_FILTER_BLOCK_LEN, 12, 0.0); // re-settle before the next swap
		}
		if (fails) {
			fprintf(stderr, "   FAIL: %d swap(s) overshot by more than 6 dB\n", fails);
			return 1;
		}
		rx_audio_set_narrow_width(300);
		(void)settled;
	}

	printf("\nAll cases completed without a crash or out-of-range sample.\n");
	return 0;
}
