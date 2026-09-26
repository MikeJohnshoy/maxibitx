// rx_audio_impulse_test.c
//
// Bench harness for how stage 3 and stage 4 interact - the narrow filter and
// the AGC together, through the real rx_audio_process(), rather than either
// one in isolation. Not part of the normal build:
//
//   make test-rx-audio-impulse && ./test-rx-audio-impulse
//
// What it can see that the others cannot: rx_audio.c computes its AGC gain
// from the RAW undelayed I/Q and applies it to stage 3's output, which carries
// input from one group delay earlier - 3.6ms for the elliptic bank, 4.5ms for
// the minimum-phase FFT filter, 16.0ms for the linear-phase one. The gain
// therefore runs ahead of the audio it modulates, by a different amount per
// realization. rx_filter_test.c feeds a filter directly and rx_audio_test.c
// checks which buffer reaches stage 4; only a harness with both stages in
// circuit can measure what their interaction costs.
//
// Case A - an interferer the filter removes: a steady wanted tone on the dial
//   plus strong bursts 2kHz off, deep in every stopband. The filter takes the
//   burst out of the audio, the AGC still sees it. Measures how far a signal
//   that cannot be heard ducks one that can, and asserts the duck is equal
//   for every filter: the AGC sits upstream of the selection, so it cannot
//   depend on which filter is chosen, and the assertion is what keeps that
//   true.
// Case B - the wanted signal IS the transient: keyed CW at 20 WPM. The AGC
//   sees each element rise undelayed and starts ducking before the element's
//   audio emerges. Measures how much of its onset transient survives, which
//   is where the realizations differ.
//
// Results and what they mean:
// docs/dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md §15.
//
// Links rx_audio.c/vfo.c/fft_filter.c/rx_filter.c, no hardware deps, same
// precedent as this project's other harnesses.

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "rx_audio.h"
#include "rx_filter.h"  // RX_FILTER_BLOCK_LEN
#include "cw.h"         // CW_PITCH_HZ
#include "vfo.h"        // vfo_init_phase_table()

#define FS 96000.0
#define BLK RX_FILTER_BLOCK_LEN
#define NBLK 700
#define NS (NBLK * BLK)

#define WANTED_AMP 0.010    // a modest on-dial signal
#define NOISE_AMP  0.0004   // a floor, so the AGC has something to ride

// Bursts 1.5s apart - five times the AGC's 300ms release, so the gain is
// genuinely settled before each one and the baseline measures a settled
// level rather than a recovery curve. A period shorter than the release
// silently measures the AGC's own recovery instead of the effect under test;
// see the design note's §15 for what that looked like.
#define BURST_PERIOD_MS 1500.0
#define BURST_MS           2.0
#define BURST_AMP         0.320   // ~30dB above the wanted signal
#define BURST_OFFSET_HZ 2000.0    // heard at pitch+2000: deep in every stopband

#define ELEM_MS  60.0             // 20 WPM
#define GAP_MS   60.0
#define REST_MS 700.0
#define GROUP      4              // elements per group, then a rest

static double in_i[NS], in_q[NS];
static double env[NS];
static int event_at[64], n_events;
static int out_of_range;

static double frand(void) { return 2.0 * ((double)rand() / RAND_MAX) - 1.0; }

// Feeds everything through the real rx_audio_process() and keeps a 2ms
// running-RMS envelope of the speaker output.
static void run(void)
{
	static int32_t out[BLK];
	static double sq[NS];

	out_of_range = 0;
	for (int b = 0; b < NBLK; b++) {
		rx_audio_process(&in_i[b * BLK], &in_q[b * BLK], BLK, &out[0], NULL);
		for (int i = 0; i < BLK; i++) {
			if (out[i] > 2000000000 || out[i] < -2000000000)
				out_of_range++;
			double v = (double)out[i];
			sq[b * BLK + i] = v * v;
		}
	}
	int w = (int)(0.002 * FS);
	double acc = 0;
	for (int n = 0; n < NS; n++) {
		acc += sq[n];
		if (n >= w)
			acc -= sq[n - w];
		env[n] = sqrt(acc / (n < w ? n + 1 : w));
	}
}

// ---------------------------------------------------------------------
// Case A: a burst the filter removes

static void build_interferer(void)
{
	double ph = 0, inc = -2.0 * M_PI * BURST_OFFSET_HZ / FS; // negative baseband = above the dial
	int period = (int)(BURST_PERIOD_MS / 1000.0 * FS);
	int blen = (int)(BURST_MS / 1000.0 * FS);

	n_events = 0;
	for (int n = 0; n < NS; n++) {
		// Baseband DC is a station exactly on the dial, which stage 2 mixes
		// up to the pitch - same convention as rx_audio_test.c.
		in_i[n] = WANTED_AMP + NOISE_AMP * frand();
		in_q[n] = NOISE_AMP * frand();

		int phase = n % period;
		if (n >= period && phase < blen) {
			if (phase == 0 && n_events < 64)
				event_at[n_events++] = n;
			double e = 0.5 - 0.5 * cos(2.0 * M_PI * phase / blen); // no hard edges
			in_i[n] += BURST_AMP * e * cos(ph);
			in_q[n] += BURST_AMP * e * sin(ph);
		}
		ph += inc;
	}
}

// Mean envelope over a window relative to each burst, in dB against the
// settled level, averaged over bursts.
static double burst_win(double a_ms, double b_ms, double base)
{
	double acc = 0;
	int cnt = 0;
	for (int k = 1; k < n_events; k++) { // skip the first: AGC still settling
		int lo = event_at[k] + (int)(a_ms / 1000.0 * FS);
		int hi = event_at[k] + (int)(b_ms / 1000.0 * FS);
		if (lo < 0 || hi >= NS)
			continue;
		for (int n = lo; n < hi; n++, cnt++)
			acc += env[n];
	}
	return cnt ? 20.0 * log10((acc / cnt) / base) : 0.0;
}

static double burst_baseline(void)
{
	double acc = 0;
	int cnt = 0;
	for (int k = 1; k < n_events; k++) {
		int lo = event_at[k] - (int)(0.400 * FS);
		int hi = event_at[k] - (int)(0.200 * FS);
		if (lo < 0)
			continue;
		for (int n = lo; n < hi; n++, cnt++)
			acc += env[n];
	}
	return cnt ? acc / cnt : 1.0;
}

// ---------------------------------------------------------------------
// Case B: keyed CW, where the wanted signal is the transient

static void build_keying(void)
{
	int elem = (int)(ELEM_MS / 1000.0 * FS);
	int gap = (int)(GAP_MS / 1000.0 * FS);
	int rest = (int)(REST_MS / 1000.0 * FS);
	int ramp = (int)(0.005 * FS); // 5ms, matching cw.c's own envelope

	for (int n = 0; n < NS; n++) {
		in_i[n] = NOISE_AMP * frand();
		in_q[n] = NOISE_AMP * frand();
	}

	n_events = 0;
	int n = rest;
	while (n + elem + gap < NS) {
		for (int g = 0; g < GROUP && n + elem + gap < NS; g++) {
			if (n_events < 64)
				event_at[n_events++] = n;
			for (int i = 0; i < elem; i++) {
				double e = 1.0;
				if (i < ramp)
					e = 0.5 - 0.5 * cos(M_PI * i / ramp);
				else if (i > elem - ramp)
					e = 0.5 - 0.5 * cos(M_PI * (elem - i) / ramp);
				in_i[n + i] += WANTED_AMP * e;
			}
			n += elem + gap;
		}
		n += rest;
	}
}

// Attack and overshoot of the element AS HEARD. gd_ms shifts the measurement
// window by the filter's group delay, so this measures the shape of the onset
// and not the delay before it.
static void keying_metrics(double gd_ms, double *attack_ms, double *overshoot_db)
{
	int elem = (int)(ELEM_MS / 1000.0 * FS);
	int gd = (int)(gd_ms / 1000.0 * FS);
	double t3_acc = 0, pk_acc = 0;
	int cnt = 0;

	for (int k = 0; k < n_events; k++) {
		int start = event_at[k] + gd;
		if (start + elem >= NS)
			continue;
		// Settled level: the last third of the element as heard.
		double s = 0;
		int sc = 0;
		for (int i = elem * 2 / 3; i < elem; i++, sc++)
			s += env[start + i];
		if (!sc || s <= 0)
			continue;
		s /= sc;

		double pk = 0;
		for (int i = 0; i < elem; i++)
			if (env[start + i] > pk)
				pk = env[start + i];
		int t3 = -1;
		for (int i = 0; i < elem; i++)
			if (env[start + i] >= s * 0.70710678) {
				t3 = i;
				break;
			}
		if (t3 < 0)
			continue;
		t3_acc += t3 / FS * 1000.0;
		pk_acc += 20.0 * log10(pk / s);
		cnt++;
	}
	*attack_ms = cnt ? t3_acc / cnt : -1.0;
	*overshoot_db = cnt ? pk_acc / cnt : 0.0;
}

// ---------------------------------------------------------------------

struct config {
	const char *name;
	int use_fft;
	int min_phase;
	double gd_ms;   // measured in rx_filter_test.c Case E / the design note
};

int main(void)
{
	static const struct config configs[] = {
		{ "elliptic",   0, 0,  3.6 },
		{ "FFT min",    1, 1,  4.5 },
		{ "FFT linear", 1, 0, 16.0 },
	};
	const int n_cfg = (int)(sizeof(configs) / sizeof(configs[0]));
	double duck[8], attack[8], overshoot[8];
	int fails = 0;

	vfo_init_phase_table();
	rx_audio_init();

	printf("rx_audio.c stage 3 + stage 4 interaction - the narrow filter and the AGC together\n");
	printf("(the AGC's gain comes from the RAW undelayed I/Q; see this file's header)\n\n");

	for (int c = 0; c < n_cfg; c++) {
		srand(20260926); // same noise for every configuration
		rx_audio_set_narrow_filter(1);
		rx_audio_set_narrow_width(300);
		rx_audio_set_narrow_pitch(CW_PITCH_HZ);
		rx_audio_set_narrow_filter_impl(configs[c].use_fft);
		rx_audio_set_narrow_filter_min_phase(configs[c].min_phase);

		build_interferer();
		run();
		if (out_of_range) {
			fprintf(stderr, "   FAIL: %s produced %d out-of-range samples\n",
			        configs[c].name, out_of_range);
			fails++;
		}
		double base = burst_baseline();
		duck[c] = burst_win(20, 25, base);

		build_keying();
		run();
		if (out_of_range) {
			fprintf(stderr, "   FAIL: %s produced %d out-of-range samples (keying)\n",
			        configs[c].name, out_of_range);
			fails++;
		}
		keying_metrics(configs[c].gd_ms, &attack[c], &overshoot[c]);
	}

	printf("A. A 30dB burst 2kHz off the dial - removed from the audio, but not from the AGC\n");
	printf("   filter       group delay   wanted signal ducked by\n");
	for (int c = 0; c < n_cfg; c++)
		printf("   %-11s %8.1f ms   %8.2f dB\n", configs[c].name, configs[c].gd_ms, duck[c]);
	printf("   The duck must be the same for all three: the AGC is upstream of the filter\n"
	       "   selection, so it cannot depend on which filter is chosen. A signal you\n"
	       "   cannot hear still sets your volume - that is the design's accepted cost\n"
	       "   (rx_audio_demod_design.md 8.8), not a defect.\n");

	double duck_spread = 0;
	for (int c = 1; c < n_cfg; c++) {
		double d = fabs(duck[c] - duck[0]);
		if (d > duck_spread)
			duck_spread = d;
	}
	printf("   spread across filters: %.2f dB (want < 0.50 - larger means the AGC has\n"
	       "   somehow become filter-dependent, which would be a real bug)\n", duck_spread);
	if (duck_spread >= 0.50) {
		fprintf(stderr, "   FAIL: the AGC duck depends on which filter is selected\n");
		fails++;
	}

	printf("\nB. Keyed CW at %.0f WPM - now the wanted signal is the transient\n", 1200.0 / ELEM_MS);
	printf("   filter       group delay   attack to -3dB   onset transient\n");
	for (int c = 0; c < n_cfg; c++)
		printf("   %-11s %8.1f ms   %10.2f ms   %+9.2f dB\n",
		       configs[c].name, configs[c].gd_ms, attack[c], overshoot[c]);
	printf("   The onset transient shrinks as the group delay grows. That is the AGC\n"
	       "   reacting to an element before it can be heard: the longer the filter\n"
	       "   holds the audio, the further the gain has already fallen by the time the\n"
	       "   element emerges, so less of its natural attack survives. Compare the\n"
	       "   filters' own attacks with no AGC in circuit - 5.2 / 8.8 / 20.3 ms\n"
	       "   (rx_filter_test.c Case E): through the full chain they land within about\n"
	       "   1.4 ms of each other, because the AGC has flattened the differences into\n"
	       "   level instead of time.\n");

	for (int c = 0; c < n_cfg; c++) {
		if (!(attack[c] > 0.0 && attack[c] < 15.0)) {
			fprintf(stderr, "   FAIL: %s attack %.2f ms is outside 0-15 ms\n",
			        configs[c].name, attack[c]);
			fails++;
		}
		if (!(overshoot[c] >= 0.0 && overshoot[c] < 6.0)) {
			fprintf(stderr, "   FAIL: %s onset transient %+.2f dB is outside 0-6 dB\n",
			        configs[c].name, overshoot[c]);
			fails++;
		}
	}

	if (fails) {
		fprintf(stderr, "\n%d check(s) failed\n", fails);
		return 1;
	}
	printf("\nAll checks passed.\n");
	return 0;
}
