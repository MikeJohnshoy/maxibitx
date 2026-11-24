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
// Case E: CW vs CWR favour opposite sides of the dial: two equal
//   stations, one each side, measured by their own audio tones, so the
//   reading is the sideband rejection and not the AGC's response to it.
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
// Case I: the mode-level inhibit (radio.c sets it for DIGITAL) bypasses
//   stage 3 however the operator last set it, reads back as off while it
//   holds, refuses a request to re-enable, and hands their setting back
//   when it lifts. The partial failure is the dangerous one - an inhibit
//   that bypasses but doesn't restore silently costs the operator their
//   CW filter on every trip through DIGITAL.
// Case M: the AGC. Its measuring point - a station 40 dB up outside stage
//   1's 3 kHz barely moves the gain, one inside it does - and its speeds:
//   how far the gain climbs in a gap between syllables and in a pause,
//   for each of fast, medium and slow, and that the hang doesn't hold the
//   gain down on noise alone.
// Case N: the volume's gain, which sound.c's transmit monitor follows:
//   the default, full volume, 0% and mute.

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "rx_audio.h"
#include "cw_decoder.h"
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

// The amplitude of a tone at f Hz in out[0..n), from a Hann-windowed
// single-bin DFT.
static double tone_amp(const int32_t *out, int n, double f) {
	double re = 0, im = 0, wsum = 0;
	for (int i = 0; i < n; i++) {
		double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / (n - 1));
		re += w * out[i] * cos(2.0 * M_PI * f * i / TEST_FS);
		im += w * out[i] * sin(2.0 * M_PI * f * i / TEST_FS);
		wsum += w;
	}
	return 2.0 * sqrt(re * re + im * im) / wsum;
}

// Case M's signals: Gaussian noise, a station on +700 Hz (above the dial),
// keyed by the speech-like pattern below or steady, and an optional second
// station other_hz above the dial (below it if negative), keyed
// other_on_s on and the same off from the start of the run. A station d
// Hz above the dial sits at -d in the I/Q (rx_audio.h).
static unsigned long long agc_rng = 88172645463325252ull;
static double agc_gauss(void) {
	double u, v;
	agc_rng ^= agc_rng << 13; agc_rng ^= agc_rng >> 7; agc_rng ^= agc_rng << 17;
	u = (agc_rng >> 11) * (1.0 / 9007199254740992.0) + 1e-12;
	agc_rng ^= agc_rng << 13; agc_rng ^= agc_rng >> 7; agc_rng ^= agc_rng << 17;
	v = (agc_rng >> 11) * (1.0 / 9007199254740992.0);
	return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

// Four 180 ms syllables 300 ms apart, then a 700 ms pause.
#define SPEECH_PERIOD_S 1.9
static int speech_on(double ts) {
	double x = fmod(ts, SPEECH_PERIOD_S);
	return x < 1.2 && fmod(x, 0.3) < 0.18;
}

#define AGC_NOISE_SIGMA 7.07e5   // the codec's floor, about -66 dBFS
#define AGC_WANTED      1.77e6   // 20 dB above the noise in 3 kHz
#define AGC_MAX_BLOCKS  1000

// Runs `seconds` through rx_audio_process() and records the AGC's gain, in
// dB, at the end of each block into gain_db[]; returns the block count. If
// tone_db is non-NULL, the wanted tone's level in each block goes there.
static double gain_db[AGC_MAX_BLOCKS], tone_db_buf[AGC_MAX_BLOCKS];
static long agc_t;
static int agc_run(double seconds, int speech, double wanted, double other_hz,
                   double other_amp, double other_on_s) {
	double i_s[RX_FILTER_BLOCK_LEN], q_s[RX_FILTER_BLOCK_LEN];
	int32_t out[RX_FILTER_BLOCK_LEN];
	int blocks = (int)(seconds * TEST_FS / RX_FILTER_BLOCK_LEN);
	if (blocks > AGC_MAX_BLOCKS)
		blocks = AGC_MAX_BLOCKS;
	long start = agc_t;
	for (int b = 0; b < blocks; b++) {
		for (int k = 0; k < RX_FILTER_BLOCK_LEN; k++, agc_t++) {
			double ts = agc_t / TEST_FS;
			double a = speech ? (speech_on(ts) ? wanted : 0.0) : wanted;
			double ph = -2.0 * M_PI * 700.0 * agc_t / TEST_FS;
			i_s[k] = AGC_NOISE_SIGMA * agc_gauss() + a * cos(ph);
			q_s[k] = AGC_NOISE_SIGMA * agc_gauss() + a * sin(ph);
			if (other_amp > 0 && fmod((agc_t - start) / TEST_FS, 2.0 * other_on_s) < other_on_s) {
				double po = -2.0 * M_PI * other_hz * agc_t / TEST_FS;
				i_s[k] += other_amp * cos(po);
				q_s[k] += other_amp * sin(po);
			}
		}
		rx_audio_process(i_s, q_s, RX_FILTER_BLOCK_LEN, out, NULL);
		gain_db[b] = 20.0 * log10(500000000.0 / rx_audio_debug_agc_envelope());
		tone_db_buf[b] = 20.0 * log10(tone_amp(out, RX_FILTER_BLOCK_LEN, 700.0) + 1e-9);
	}
	return blocks;
}

// Case L: dots at 20 WPM on a carrier d_hz from the dial (+ above it),
// with 5 ms edges, through rx_audio_process() for the given seconds, then
// SPOT's search. Returns what rx_audio_spot() said, its offset in *found.
static enum rx_spot_result spot_run(double d_hz, double seconds, double *found) {
	double i_s[RX_FILTER_BLOCK_LEN], q_s[RX_FILTER_BLOCK_LEN];
	int32_t out[RX_FILTER_BLOCK_LEN];
	static long t = 0;
	const long dot = (long)(0.06 * TEST_FS), rise = TEST_FS / 200;
	unsigned noise = 7;
	for (long b = 0; b < (long)(seconds * TEST_FS); b += RX_FILTER_BLOCK_LEN) {
		for (int k = 0; k < RX_FILTER_BLOCK_LEN; k++, t++) {
			long in = t % (2 * dot);
			double env = in < dot ? 1.0 : 0.0;
			if (in < rise)
				env = 0.5 - 0.5 * cos(M_PI * in / rise);
			else if (in >= dot && in < dot + rise)
				env = 0.5 + 0.5 * cos(M_PI * (in - dot) / rise);
			// a station d Hz above the dial sits at -d in the I/Q (rx_audio.h)
			double ph = -2.0 * M_PI * d_hz * t / TEST_FS;
			noise = noise * 1103515245u + 12345u;
			double nz = ((noise >> 8) / 16777216.0 - 0.5) * 0.01;
			i_s[k] = 0.1 * env * cos(ph) + nz;
			q_s[k] = 0.1 * env * sin(ph) + nz;
		}
		rx_audio_process(i_s, q_s, RX_FILTER_BLOCK_LEN, out, NULL);
	}
	*found = 0;
	return rx_audio_spot(found);
}

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
	// reject the second; CWR the reverse. Two equal stations, 500 Hz above
	// and below, are present together, so the AGC rides both alike and
	// each is measured by its own audio tone: the kept one at the pitch +
	// 500 (1200 Hz), what leaks of the other at |pitch - 500| (200 Hz). Run
	// in bypass (Case D left it there), so this measures the sideband
	// choice rather than stage 3's response.
	{
		const double offset = 500.0;
		double i_s[RX_FILTER_BLOCK_LEN], q_s[RX_FILTER_BLOCK_LEN];
		static int32_t out[RX_FILTER_BLOCK_LEN * 4];
		double kept[2], leak[2];
		for (int m = 0; m < 2; m++) {
			rx_audio_set_demod(m ? RX_DEMOD_CWR : RX_DEMOD_CW);
			for (int b = 0; b < 20; b++) {
				for (int k = 0; k < RX_FILTER_BLOCK_LEN; k++) {
					long t = (long)b * RX_FILTER_BLOCK_LEN + k;
					double p = 2.0 * M_PI * offset * t / TEST_FS;
					// one at -offset (above the dial), one at +offset (below)
					i_s[k] = cos(p) + cos(p);
					q_s[k] = -sin(p) + sin(p);
				}
				rx_audio_process(i_s, q_s, RX_FILTER_BLOCK_LEN,
				                 out + (b >= 16 ? (b - 16) * RX_FILTER_BLOCK_LEN : 0), NULL);
			}
			kept[m] = tone_amp(out, RX_FILTER_BLOCK_LEN * 4, CW_PITCH_HZ + offset);
			leak[m] = tone_amp(out, RX_FILTER_BLOCK_LEN * 4, fabs(CW_PITCH_HZ - offset));
		}
		double rej_cw = 20.0 * log10(kept[0] / leak[0]);
		double rej_cwr = 20.0 * log10(kept[1] / leak[1]);
		printf("\nE. CW vs CWR sideband: equal stations %.0f Hz above and below the dial (bypass)\n",
		       offset);
		printf("   CW keeps the one above, CWR the one below; the other is rejected by:\n");
		printf("   CW %.1f dB, CWR %.1f dB (want over 30)\n", rej_cw, rej_cwr);
		if (!(rej_cw > 30.0) || !(rej_cwr > 30.0)) {
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
		printf("   (rejection grows as the width narrows - the AGC rides stage 1's\n"
		       "    output, ahead of stage 3, so these are absolute levels, not normalized)\n");
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

	// --- Case I: the mode-level inhibit -----------------------------------
	// radio.c's radio_set_mode() sets this for DIGITAL, where stage 3 would
	// hand WSJT-X a 300 Hz slice of a 2.7 kHz mode (rx_audio.h). Three
	// separate claims, because the interesting failure is a partial one:
	// the inhibit really bypasses, the readback tells the truth about it,
	// and the operator's own setting survives it.
	//
	// Measured on a tone 900 Hz off dial center, which reaches stage 3 as
	// 1600 Hz audio - well into the stopband, so bypassing the filter is a
	// large, unambiguous change. The AGC can't confuse the comparison: its
	// gain comes from stage 1's output, ahead of stage 3 and so identical in
	// all three runs (rx_narrow_filter_fft_vs_elliptic.md §15).
	{
		const double off_pitch = -900.0;
		printf("\nI. Mode-level inhibit (DIGITAL), tone %.0f Hz off dial center\n", off_pitch);

		rx_audio_set_narrow_filter(1);
		rx_audio_set_narrow_filter_impl(1);
		rx_audio_inhibit_narrow_filter(0);
		double filtered = run_tone(RX_FILTER_BLOCK_LEN, 16, off_pitch);

		// What a bypass measures, for the inhibit to be compared against.
		rx_audio_set_narrow_filter(0);
		double bypassed = run_tone(RX_FILTER_BLOCK_LEN, 16, off_pitch);

		// The request back ON, then inhibited: this is the DIGITAL case.
		rx_audio_set_narrow_filter(1);
		rx_audio_inhibit_narrow_filter(1);
		double inhibited = run_tone(RX_FILTER_BLOCK_LEN, 16, off_pitch);
		int reads_off = (rx_audio_get_narrow_filter() == 0);
		int reports_inhibited = (rx_audio_narrow_filter_inhibited() == 1);

		// A client asking for it again while inhibited must not get it.
		rx_audio_set_narrow_filter(1);
		double still_inhibited = run_tone(RX_FILTER_BLOCK_LEN, 16, off_pitch);

		// Leaving the mode: the request the operator never withdrew returns.
		rx_audio_inhibit_narrow_filter(0);
		double restored = run_tone(RX_FILTER_BLOCK_LEN, 16, off_pitch);
		int reads_on = (rx_audio_get_narrow_filter() == 1);

		printf("   filter in circuit RMS=%.1f, bypassed RMS=%.1f (%.1f dB of rejection)\n",
		       filtered, bypassed, 20.0 * log10(bypassed / filtered));
		printf("   inhibited RMS=%.1f - wanted the bypass value, %.2f dB away\n",
		       inhibited, 20.0 * log10(inhibited / bypassed));
		printf("   re-requested while inhibited RMS=%.1f (must stay bypassed)\n", still_inhibited);
		printf("   inhibit lifted RMS=%.1f - wanted the in-circuit value, %.2f dB away\n",
		       restored, 20.0 * log10(restored / filtered));
		// Printed from the values captured at the time, not re-read here -
		// the inhibit has since been lifted, and re-reading would print the
		// wrong moment's answer while the assertions below checked the right
		// one.
		printf("   readback while inhibited: narrow=%d inhibited=%d (want 0 and 1),"
		       " after lifting: narrow=%d (want 1)\n",
		       reads_off ? 0 : 1, reports_inhibited, reads_on ? 1 : 0);

		// 0.1 dB, not equality: the filters keep running while bypassed, so
		// the two runs differ only by their AGC/meter envelope histories.
		if (fabs(20.0 * log10(inhibited / bypassed)) > 0.1) {
			fprintf(stderr, "   FAIL: the inhibit didn't bypass stage 3\n");
			return 1;
		}
		if (fabs(20.0 * log10(still_inhibited / bypassed)) > 0.1) {
			fprintf(stderr, "   FAIL: a request while inhibited put the filter back in circuit\n");
			return 1;
		}
		if (fabs(20.0 * log10(restored / filtered)) > 0.1) {
			fprintf(stderr, "   FAIL: lifting the inhibit didn't restore the operator's setting\n");
			return 1;
		}
		// The whole case is vacuous if the filter wasn't doing anything at
		// this offset to begin with.
		if (!(20.0 * log10(bypassed / filtered) > 6.0)) {
			fprintf(stderr, "   FAIL: stage 3 barely attenuated this offset - "
			                "the comparison above proves nothing\n");
			return 1;
		}
		if (!reads_off || !reports_inhibited || !reads_on) {
			fprintf(stderr, "   FAIL: readback disagreed with the inhibit state\n");
			return 1;
		}
	}

	// --- Case K: the CW decoder, fed by rx_audio_process() ---------------
	// A carrier on the dial (0 Hz baseband), keyed with "CQ DE KB2ML K" at
	// 20 WPM with 5 ms edges, in a little noise: the decoder, tapped after
	// stage 2 at the BFO's pitch, has to read it back.
	{
		printf("\nK. The CW decoder, through rx_audio_process()\n");
		rx_audio_set_demod(RX_DEMOD_CW);
		cw_decoder_init();
		char buf[CW_DECODER_TEXT_KEEP + 1];
		uint32_t seq = cw_decoder_read(UINT32_MAX, buf, 1);
		static const char *const code[] = {"-.-.", "--.-", " ", "-..", ".", " ", "-.-", "-...",
		                                   "..---", "--", ".-..", " ", "-.-", NULL};
		const double u = 1.2 / 20 * TEST_FS;           // samples per dot
		static double env[60 * 96000];
		long len = (long)(1.0 * TEST_FS);              // a second of silence first
		for (int c = 0; code[c]; c++) {
			if (code[c][0] == ' ') {
				len += (long)(4 * u);                  // 3 already after the character
				continue;
			}
			for (int e = 0; code[c][e]; e++) {
				long m = (long)((code[c][e] == '.' ? 1 : 3) * u);
				for (long k = 0; k < m; k++)
					env[len + k] = 1.0;
				len += m + (long)u;
			}
			len += (long)(2 * u);
		}
		len += 3 * 96000;                              // and three after, for the pause
		long rise = 96000 / 200;                       // 5 ms raised-cosine edges
		for (long k = 1; k < len; k++)
			if (env[k] != env[k - 1]) {
				int up = env[k] > env[k - 1];
				for (long r = 0; r < rise && k + r < len; r++)
					env[k + r] = up ? 0.5 - 0.5 * cos(M_PI * r / rise)
					                : 0.5 + 0.5 * cos(M_PI * r / rise);
				k += rise;
			}
		double i_s[RX_FILTER_BLOCK_LEN], q_s[RX_FILTER_BLOCK_LEN];
		int32_t out[RX_FILTER_BLOCK_LEN];
		char text[256] = "";
		unsigned noise = 1;
		for (long b = 0; b + RX_FILTER_BLOCK_LEN <= len; b += RX_FILTER_BLOCK_LEN) {
			for (int k = 0; k < RX_FILTER_BLOCK_LEN; k++) {
				noise = noise * 1103515245u + 12345u;
				double nz = ((noise >> 8) / 16777216.0 - 0.5) * 0.02;
				i_s[k] = 0.1 * env[b + k] + nz;
				q_s[k] = nz;
			}
			rx_audio_process(i_s, q_s, RX_FILTER_BLOCK_LEN, out, NULL);
			seq = cw_decoder_read(seq, buf, sizeof(buf));
			if (strlen(text) + strlen(buf) < sizeof(text))
				strcat(text, buf);
		}
		printf("   decoded: \"%s\"\n", text);
		if (strncmp(text, "CQ DE KB2ML K", 13) != 0) {
			fprintf(stderr, "   FAIL: wanted \"CQ DE KB2ML K\"\n");
			return 1;
		}
	}

	// --- Case L: SPOT, through rx_audio_process() -------------------------
	{
		printf("\nL. SPOT: the offset of a keyed station, through rx_audio_process()\n");
		double f;
		struct { enum rx_demod demod; int pitch, narrow, width; double d, secs;
		         enum rx_spot_result want; const char *what; } c[] = {
			{ RX_DEMOD_CW,  700, 1, 300,  45.3, 2.0, RX_SPOT_OK, "CW, 45.3 Hz above" },
			{ RX_DEMOD_CW,  700, 1, 300, -88.7, 2.0, RX_SPOT_OK, "CW, 88.7 Hz below" },
			{ RX_DEMOD_CWR, 700, 1, 300, -45.3, 2.0, RX_SPOT_OK, "CWR, 45.3 Hz below" },
			{ RX_DEMOD_CWR, 700, 1, 300,  61.0, 2.0, RX_SPOT_OK, "CWR, 61 Hz above" },
			{ RX_DEMOD_CW,  600, 1, 300,  30.0, 2.0, RX_SPOT_OK, "CW at a 600 Hz pitch, 30 Hz above" },
			{ RX_DEMOD_CW,  700, 1, 150, 100.0, 2.0, RX_SPOT_NO_SIGNAL,
			  "150 Hz filter, station 100 Hz above: outside it" },
			{ RX_DEMOD_CW,  700, 0, 150, 100.0, 2.0, RX_SPOT_OK, "the filter off: found" },
			{ RX_DEMOD_CW,  700, 1, 300,  20.0, 1.0, RX_SPOT_NOT_YET, "only 1 s since a change" },
		};
		for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
			rx_audio_set_demod(c[i].demod);
			rx_audio_set_narrow_pitch(c[i].pitch);
			rx_audio_set_narrow_width(c[i].width);
			rx_audio_set_narrow_filter(c[i].narrow);
			rx_audio_spot_reset();
			enum rx_spot_result r = spot_run(c[i].d, c[i].secs, &f);
			int ok = r == c[i].want && (r != RX_SPOT_OK || fabs(f - c[i].d) < 1.0);
			printf("   %s %-48s result %d, offset %+.2f Hz\n", ok ? "ok  " : "FAIL", c[i].what,
			       (int)r, f);
			if (!ok) {
				fprintf(stderr, "   FAIL: wanted result %d, offset %+.1f\n", (int)c[i].want, c[i].d);
				return 1;
			}
		}
		rx_audio_set_demod(RX_DEMOD_USB);
		{
			enum rx_spot_result r = rx_audio_spot(&f);
			printf("   %s USB: result %d\n", r == RX_SPOT_NOT_CW ? "ok  " : "FAIL", (int)r);
			if (r != RX_SPOT_NOT_CW)
				return 1;
		}
		rx_audio_set_demod(RX_DEMOD_CW);
		rx_audio_set_narrow_pitch(CW_PITCH_HZ);
		rx_audio_set_narrow_width(300);
		rx_audio_set_narrow_filter(1);
	}

	// --- Case M: the AGC -------------------------------------------------
	// USB, bypass, noise at the codec's floor and a station on +700 Hz 20 dB
	// above it in 3 kHz.
	{
		rx_audio_set_demod(RX_DEMOD_USB);
		rx_audio_set_narrow_filter(0);
		int fails = 0;
		const double per_block = RX_FILTER_BLOCK_LEN / TEST_FS;

		printf("\nM. The AGC\n");
		// Its measuring point: a station 40 dB above the wanted one, keyed
		// 1 s on and 1 s off, outside stage 1's 3 kHz and inside it. The
		// wanted tone while it's on, against the wanted tone alone.
		rx_audio_set_agc(RX_AGC_FAST);
		int nb = agc_run(3.0, 0, AGC_WANTED, 0, 0, 1.0);
		double alone = 0;
		for (int b = nb - 20; b < nb; b++)
			alone += tone_db_buf[b] / 20.0;
		const double others[] = {8000.0, -1500.0, 2000.0};
		const double max_duck[] = {6.0, 6.0, 99.0}, min_duck[] = {0.0, 0.0, 30.0};
		for (int i = 0; i < 3; i++) {
			nb = agc_run(4.0, 0, AGC_WANTED, others[i], AGC_WANTED * 100.0, 1.0);
			// the other station is on in [2, 3) s of the run
			double on = 0;
			int n_on = 0;
			for (int b = 0; b < nb; b++) {
				double ts = (b + 0.5) * per_block;
				if (ts > 2.3 && ts < 2.9) {
					on += tone_db_buf[b];
					n_on++;
				}
			}
			double duck = alone - on / n_on;
			int ok = duck <= max_duck[i] && duck >= min_duck[i];
			printf("   %s a station 40 dB up, %+5.0f Hz from the dial (%s): "
			       "the wanted one down %.1f dB\n", ok ? "ok  " : "FAIL", others[i],
			       i < 2 ? "outside the 3 kHz" : "inside it", duck);
			fails += !ok;
		}

		// Its speeds, on the speech-like pattern: the gain's rise, from its
		// average inside the syllables, at its highest in the 120 ms gaps
		// and in the 700 ms pauses; and its average on noise alone, which
		// a slower release sits a few dB lower, nearer the noise's peaks.
		const char *names[] = {"fast", "medium", "slow"};
		const double gap_max[] = {99, 1.0, 1.0}, pause_min[] = {10, 3, -99}, pause_max[] = {99, 12, 1.0};
		double noise_gain[3];
		for (int sp = 0; sp < 3; sp++) {
			rx_audio_set_agc((enum rx_agc)sp);
			nb = agc_run(3.0, 0, 0.0, 0, 0, 1.0);
			noise_gain[sp] = 0;
			for (int b = nb - 100; b < nb; b++)
				noise_gain[sp] += gain_db[b] / 100.0;
			long t0 = agc_t;
			nb = agc_run(4 * SPEECH_PERIOD_S, 1, AGC_WANTED, 0, 0, 1.0);
			double syl = 0, gap = -1e9, pause = -1e9;
			int n_syl = 0;
			for (int b = 0; b < nb; b++) {
				double ts = (t0 + (b + 1.0) * RX_FILTER_BLOCK_LEN) / TEST_FS;
				if (ts - t0 / TEST_FS < SPEECH_PERIOD_S)
					continue; // the first phrase settles it
				double x = fmod(ts, SPEECH_PERIOD_S), y = fmod(x, 0.3);
				if (x < 1.2 && y > 0.02 && y < 0.17) {
					syl += gain_db[b];
					n_syl++;
				} else if (x < 1.2 && y > 0.28 && gain_db[b] > gap) {
					gap = gain_db[b];
				} else if (x > 1.85 && gain_db[b] > pause) {
					pause = gain_db[b];
				}
			}
			syl /= n_syl;
			int ok = gap - syl <= gap_max[sp] && pause - syl >= pause_min[sp] &&
			         pause - syl <= pause_max[sp] && noise_gain[sp] > noise_gain[0] - 6.0;
			printf("   %s %-6s the gain climbs %+5.1f dB in a gap between syllables, %+5.1f dB in "
			       "a pause; on noise alone %+.1f dB from fast's\n", ok ? "ok  " : "FAIL",
			       names[sp], gap - syl, pause - syl, noise_gain[sp] - noise_gain[0]);
			fails += !ok;
		}
		if (fails) {
			fprintf(stderr, "   FAIL: %d AGC case(s) out of range\n", fails);
			return 1;
		}
		rx_audio_set_agc(RX_AGC_FAST);
		rx_audio_set_demod(RX_DEMOD_CW);
		rx_audio_set_narrow_filter(1);
	}

	// --- Case N: the volume's gain, for the transmit monitor --------------
	{
		printf("\nN. The volume's gain (sound.c's transmit monitor follows it)\n");
		double at_default = rx_audio_volume_gain();
		rx_audio_set_volume(100);
		double full = rx_audio_volume_gain();
		rx_audio_set_mute(1);
		double muted = rx_audio_volume_gain();
		rx_audio_set_mute(0);
		double unmuted = rx_audio_volume_gain();
		rx_audio_set_volume(0);
		double zero = rx_audio_volume_gain();
		rx_audio_set_volume(40);
		int ok = fabs(at_default - 0.2 * pow(10.0, -30.0 / 20.0)) < 1e-9 &&
		         fabs(full - 0.2) < 1e-12 && muted == 0.0 && unmuted == full && zero == 0.0;
		printf("   %s default %.5f, 100%% %.3f, muted %.0f, unmuted %.3f, 0%% %.0f\n",
		       ok ? "ok  " : "FAIL", at_default, full, muted, unmuted, zero);
		if (!ok)
			return 1;
	}

	printf("\nAll cases completed without a crash or out-of-range sample.\n");
	return 0;
}
