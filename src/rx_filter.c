// rx_filter.c
//
// See rx_filter.h for what this is and why it needs its own module
// (distinct from tx_pipeline.c) and its own, much larger impulse length
// than TX's. Unlike tx_pipeline.c, there is no sideband-zero step and no
// bin-rotate here - this filter shapes a real signal's spectrum in
// place, it doesn't build an analytic signal or move it to an IF - so
// this file is close to a thin wrapper around fft_filter.c's own
// filter_forward()/filter_inverse(), plus the pitch/width -> low/high
// bookkeeping filter_tune_real() needs.

#include "rx_filter.h"
#include <stdio.h>

struct rx_filter *rx_filter_new(float pitch_hz, float width_hz)
{
	struct rx_filter *r = malloc(sizeof(struct rx_filter));

	// FFTW_ESTIMATE by default (not filter_new()'s FFTW_MEASURE - see
	// this function's header comment in rx_filter.h for why), but
	// overridable at runtime via MAXIBITX_RX_FILTER_FFTW_MEASURE - a
	// deliberate, temporary diagnostic knob added after a real-hardware
	// report (docs/ARCHITECTURE.md §10 step 7's follow-up) of an ALSA
	// playback xrun flood a few seconds into a run on a Pi Zero 2W,
	// AFTER switching to FFTW_ESTIMATE fixed the startup delay it was
	// meant to fix. Two live hypotheses this exists to bisect between:
	// (a) FFTW_ESTIMATE's un-benchmarked algorithm choice happens to run
	// meaningfully slower per block than FFTW_MEASURE's on this specific
	// ARM core (a real, known FFTW characteristic - ESTIMATE trades
	// speed for a fast, un-searched choice, and how much speed varies by
	// machine), independent of anything else changing; or (b) the
	// pre-existing "run both stage-3 filters every block regardless of
	// selection" design (rx_audio.c, step 7) was already right at this
	// board's real-time ceiling and nobody had run it long enough to
	// notice before this session. Setting this environment variable to
	// any non-empty value forces FFTW_MEASURE back (slow startup, like
	// before this fix) so the two can be told apart: if the xrun flood
	// goes away, it's (a) and the real fix is a wisdom-file cache (still
	// not implemented - see §9); if it persists, it's (b) and the real
	// fix is in rx_audio.c's "always run both filters" design, not here.
	const char *force_measure = getenv("MAXIBITX_RX_FILTER_FFTW_MEASURE");
	unsigned flags = (force_measure && *force_measure) ? FFTW_MEASURE : FFTW_ESTIMATE;
	printf("rx_filter: using %s for its FFTW plan (RX_FILTER_N=%d)%s\n",
	       flags == FFTW_MEASURE ? "FFTW_MEASURE" : "FFTW_ESTIMATE", RX_FILTER_N,
	       flags == FFTW_MEASURE ? " - MAXIBITX_RX_FILTER_FFTW_MEASURE set, expect a slower startup" : "");

	r->filt = filter_new_ex(RX_FILTER_BLOCK_LEN, RX_FILTER_IMPULSE_LEN, flags);
	rx_filter_retune(r, pitch_hz, width_hz);
	return r;
}

int rx_filter_retune(struct rx_filter *r, float pitch_hz, float width_hz)
{
	float low = (pitch_hz - width_hz / 2.0f) / RX_FILTER_FS_HZ;
	float high = (pitch_hz + width_hz / 2.0f) / RX_FILTER_FS_HZ;
	return filter_tune_real(r->filt, low, high, RX_FILTER_KAISER_BETA);
}

void rx_filter_process_block(struct rx_filter *r, const float *in, float *out)
{
	struct filter *f = r->filt;
	complex float in_c[RX_FILTER_BLOCK_LEN];
	complex float out_c[RX_FILTER_BLOCK_LEN];

	for (int i = 0; i < RX_FILTER_BLOCK_LEN; i++)
		in_c[i] = in[i];

	filter_forward(f, in_c);
	filter_inverse(f, out_c);

	// filter_tune_real()'s passband is symmetric (real-valued, even in
	// frequency) by construction, so applying it to a real input should
	// leave only a negligible floating-point imaginary residue here, not
	// a real halved-amplitude artifact the way tx_pipeline.c's one-sided
	// sideband-zero step deliberately produces (and corrects for with an
	// explicit x2) - see rx_filter_test.c's Case A, which checks that
	// residual directly rather than just assuming it away.
	for (int i = 0; i < RX_FILTER_BLOCK_LEN; i++)
		out[i] = crealf(out_c[i]);
}

void rx_filter_free(struct rx_filter *r)
{
	filter_free(r->filt);
	free(r);
}
