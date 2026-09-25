// tx_pipeline.c
//
// See tx_pipeline.h for what this is and the IF-shift derivation. This
// file adds the two steps fft_filter.c doesn't provide: the sideband
// zero and the bin rotate.

#include <math.h>
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

	// This board's values replace these once sound.c has read
	// hw_settings.ini - see tx_pipeline_set_if_placement().
	p->cw_shift_bins = TX_IF_SHIFT_CW_BINS;
	p->ssb_shift_bins = TX_IF_SHIFT_SSB_BINS;

	// Limiter at unity until someone sets a ceiling: output is the
	// delayed signal, unchanged.
	p->ceiling = 1.0f;
	p->gain = 1.0f;
	memset(p->delay, 0, sizeof(p->delay));
	p->delay_pos = 0;
	p->meter_db = 0.0f;
	return p;
}

int tx_pipeline_retune(struct tx_pipeline *p, float low_hz, float high_hz, float fs_hz)
{
	// filter_tune_real(), not filter_tune(). filter_tune()'s passband is
	// one-sided (positive frequencies only), which leaves nothing for
	// TX_PIPELINE_LSB to keep - LSB put out 0W on air until this changed
	// (ARCHITECTURE.md §10 step 8; tx_pipeline_test.c Case D).
	return filter_tune_real(p->filt, low_hz / fs_hz, high_hz / fs_hz, TX_PIPELINE_KAISER_BETA);
}

// Sideband zero. Bins n <= N/2 are positive frequencies (kept for CW and
// USB); n > N/2 are negative (kept for LSB), the same convention as
// filter_tune().
static void zero_sideband(complex float *freq, int N, enum tx_pipeline_signal signal)
{
	if (signal != TX_PIPELINE_LSB) {
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

// Applies the peak limiter and writes the block's real output. See
// tx_pipeline.h, "Peak limiter (ALC)", for why this works on the complex
// signal's magnitude and why the output is delayed.
//
// 'scale' carries both the x2 for the discarded sideband and the
// per-block sign flip, so out_c is read once per sample.
static void limit_and_emit(struct tx_pipeline *p, const complex float *out_c,
                            float scale, float *out)
{
	const float ceiling = p->ceiling;
	// Crossing the whole gain range in exactly the look-ahead window is
	// what bounds the overshoot: however far the target drops, the gain
	// can get there before the sample that needs it is emitted.
	const float attack_step = 1.0f / (float)TX_ALC_LOOKAHEAD_SAMPLES;
	const float release_step = 1.0f / (TX_ALC_RELEASE_S * TX_PIPELINE_FS_HZ);
	float gain = p->gain;
	float min_gain = 1.0f;

	for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++) {
		// The envelope of the real signal below: out_c is analytic here
		// (the sideband zero left a one-sided spectrum), so its magnitude
		// is what the exciter will radiate.
		float env = 2.0f * cabsf(out_c[i]);
		float target = (env > ceiling) ? ceiling / env : 1.0f;

		if (target < gain) {
			gain -= attack_step;
			if (gain < target)
				gain = target;
		} else {
			gain += release_step;
			if (gain > target)
				gain = target;
		}
		if (gain < min_gain)
			min_gain = gain;

		// Emit the sample that entered the delay line
		// TX_ALC_LOOKAHEAD_SAMPLES ago, at the gain reached by now, then
		// put this sample in its place.
		out[i] = p->delay[p->delay_pos] * gain;
		p->delay[p->delay_pos] = scale * crealf(out_c[i]);
		p->delay_pos = (p->delay_pos + 1) % TX_ALC_LOOKAHEAD_SAMPLES;
	}
	p->gain = gain;

	// Peak-hold for the meter: jump to this block's deepest reduction,
	// otherwise fall at TX_ALC_METER_RANGE_DB per TX_ALC_METER_DECAY_S.
	float reduction_db = (min_gain < 1.0f) ? -20.0f * log10f(min_gain) : 0.0f;
	float held = p->meter_db - TX_ALC_METER_RANGE_DB * (float)TX_PIPELINE_BLOCK_LEN /
	                                (TX_ALC_METER_DECAY_S * TX_PIPELINE_FS_HZ);
	if (held < 0.0f)
		held = 0.0f;
	p->meter_db = (reduction_db > held) ? reduction_db : held;
}

void tx_pipeline_process_block(struct tx_pipeline *p, enum tx_pipeline_signal signal,
                                const float *in, float *out)
{
	struct filter *f = p->filt;
	complex float in_c[TX_PIPELINE_BLOCK_LEN];
	complex float out_c[TX_PIPELINE_BLOCK_LEN];

	// Real audio in, imaginary part 0 (as sbitx's tx_process()).
	for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++)
		in_c[i] = in[i];

	// CW anchors its tone on the dial, SSB its suppressed carrier - see
	// the IF placement comment in tx_pipeline.h.
	int shift_bins = (signal == TX_PIPELINE_CW) ? p->cw_shift_bins : p->ssb_shift_bins;

	filter_forward(f, in_c);
	zero_sideband(f->freq, f->N, signal);
	rotate_bins(f->freq, f->N, shift_bins, p->rotate_scratch);
	filter_inverse(f, out_c);

	// Phase continuity. Rotating each block's spectrum by k bins modulates
	// the block's *local* samples by e^(j*2*pi*k*m/N), but overlap-save's
	// output starts L samples later in global time every block, which adds
	// a per-block phase of e^(-j*2*pi*k*b*L/N). With L/N exactly 1/2
	// (1024/2048) that factor is e^(-j*pi*k*b): +1 unless both k and b are
	// odd, then -1. Both rotations here are odd (467, 497), so without this
	// the carrier flips sign every other block (measured: the wanted tone
	// at -97.75dB, ARCHITECTURE.md §10 step 4; CW's 467 bins are odd,
	// SSB's 482 even). If the block/impulse sizes
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
	limit_and_emit(p, out_c, flip ? -2.0f : 2.0f, out);
}

void tx_pipeline_reset(struct tx_pipeline *p)
{
	// Everything here is signal state; see tx_pipeline.h for what
	// deliberately survives.
	filter_reset(p->filt);
	p->block_count = 0;
	p->gain = 1.0f;
	memset(p->delay, 0, sizeof(p->delay));
	p->delay_pos = 0;
	p->meter_db = 0.0f;
}

int tx_pipeline_set_if_placement(struct tx_pipeline *p, int bfo_hz, int xtal_center_hz,
                                  int cw_tone_hz)
{
	// The SSB shift is the whole difference; CW's is that less the tone it
	// anchors on the dial - the caller's live pitch rather than
	// CW_PITCH_HZ, so moving the sidetone doesn't move the carrier
	// (tx_pipeline.h).
	double ssb_hz = (double)bfo_hz - (double)xtal_center_hz;
	double cw_hz = ssb_hz - (double)cw_tone_hz;
	double nyquist = TX_PIPELINE_FS_HZ / 2.0;

	if (!(cw_hz > 0.0) || ssb_hz >= nyquist)
		return -1;

	p->cw_shift_bins = (int)(cw_hz / TX_PIPELINE_BIN_HZ + 0.5);
	p->ssb_shift_bins = (int)(ssb_hz / TX_PIPELINE_BIN_HZ + 0.5);
	return 0;
}

void tx_pipeline_set_ceiling(struct tx_pipeline *p, float ceiling)
{
	if (!(ceiling > 0.0f) || ceiling > 1.0f)
		ceiling = 1.0f;
	p->ceiling = ceiling;
}

float tx_pipeline_alc_db(const struct tx_pipeline *p)
{
	return p->meter_db;
}

void tx_pipeline_free(struct tx_pipeline *p)
{
	filter_free(p->filt);
	free(p->rotate_scratch);
	free(p);
}
