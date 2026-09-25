// fft_filter.c
//
// See fft_filter.h for the design rationale (ported design math, but a
// self-contained per-instance convolution engine rather than sbitx's
// shared-globals one) and docs/ARCHITECTURE.md §4/§5 for why this
// exists at all.

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fft_filter.h"

// How far below the passband peak filter_min_phase() clamps the
// magnitude before taking its logarithm. Deep enough to be inaudible
// under any stopband this project designs (the narrow CW filter reaches
// about -80dB), shallow enough to keep the cepstrum short: the floor
// sets how fast the folded cepstrum decays, and a cepstrum that outruns
// N wraps around and corrupts the response it was supposed to preserve.
// -100dB was bench-chosen against both ends of that trade - see
// docs/dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md.
#define MIN_PHASE_FLOOR_DB (-100.0f)

// What fraction of the impulse filter_min_phase() fades out before
// truncating it to M taps - the last third here. See the truncation
// comment inside that function for what the fade buys and what the bench
// sweep measured for other lengths.
#define MIN_PHASE_FADE_DIVISOR 3

struct filter *filter_new_ex(int block_len, int impulse_len, unsigned fftw_flags)
{
	struct filter *f = malloc(sizeof(struct filter));
	f->L = block_len;
	f->M = impulse_len;
	f->N = f->L + f->M - 1;

	f->fir_coeff = fftwf_alloc_complex(f->N);
	f->history   = fftwf_alloc_complex(f->M - 1);
	f->time      = fftwf_alloc_complex(f->N);
	f->freq      = fftwf_alloc_complex(f->N);

	memset(f->fir_coeff, 0, f->N * sizeof(complex float));
	memset(f->history, 0, (f->M - 1) * sizeof(complex float));
	memset(f->time, 0, f->N * sizeof(complex float));
	memset(f->freq, 0, f->N * sizeof(complex float));

	// fftw_flags is normally FFTW_MEASURE (via filter_new() below):
	// worth the one-time startup cost (benchmarks a few candidate
	// algorithms against this actual machine) because this plan is
	// reused every filter_forward()/filter_inverse() call - once per
	// audio block (~10.7ms at minibitx's 96kHz/PERIOD_FRAMES=1024 - see
	// sound.c - which is also exactly this L, not a coincidence: it's
	// the block size this filter was designed to match). No wisdom
	// file yet (unlike sbitx's WISDOM_MODE, which caches a MEASURE
	// search's result across process restarts) - see
	// docs/ARCHITECTURE.md's open questions; on the Pi this means every
	// process start pays the MEASURE search once per filter_new_ex()
	// call, not on every block. rx_filter.c instead passes
	// FFTW_ESTIMATE here - see filter_new_ex()'s own header comment.
	f->plan_fwd = fftwf_plan_dft_1d(f->N, f->time, f->freq, FFTW_FORWARD, fftw_flags);
	f->plan_rev = fftwf_plan_dft_1d(f->N, f->freq, f->time, FFTW_BACKWARD, fftw_flags);

	return f;
}

struct filter *filter_new(int block_len, int impulse_len)
{
	return filter_new_ex(block_len, impulse_len, FFTW_MEASURE);
}

// Modified Bessel function of the 0th kind - the Kaiser window's
// defining series. Ported verbatim from mj_zbitx/src/fft_filter.c.
static float i0(float z)
{
	float const t = (z * z) / 4;
	float sum = 1 + t;
	float term = t;
	for (int k = 2; k < 40; k++) {
		term *= t / (k * k);
		sum += term;
		if (term < 1e-12 * sum)
			break;
	}
	return sum;
}

// Computes an entire Kaiser window of length M into `window`. Ported
// verbatim from mj_zbitx/src/fft_filter.c's make_kaiser().
static void make_kaiser(float *window, unsigned int M, float beta)
{
	float const numc = M_PI * beta;
	float const inv_denom = 1. / i0(numc);
	float const pc = 2.0 / (M - 1);

	// Symmetric - compute half, mirror the rest.
	for (unsigned int n = 0; n < M / 2; n++) {
		float const p = pc * n - 1;
		window[M - 1 - n] = window[n] = i0(numc * sqrtf(1 - p * p)) * inv_denom;
	}
	if (M & 1)
		window[(M - 1) / 2] = 1;
}

// Applies a Kaiser-windowed impulse-response limit to a frequency
// response: round-trips `response` (N-point, frequency domain) through
// an inverse FFT, truncates/windows the time-domain impulse to M taps,
// zero-pads back out to N, and FFTs forward again - in place. Ported
// from mj_zbitx/src/fft_filter.c's window_filter(), with one deliberate
// change: FFTW_ESTIMATE instead of WISDOM_MODE/FFTW_MEASURE for this
// function's own throwaway plans. This runs at filter_tune() time, not
// per audio block - i.e. at human-interaction speed (a pitch/width
// control changing), where FFTW_MEASURE's benchmarking search would
// make every retune visibly stall without a wisdom-file cache to
// amortize it. ESTIMATE picks a good-enough (not necessarily fastest)
// algorithm immediately; for an N this small (2048 points), the
// resulting transform still runs in well under a millisecond, so
// "good enough" costs nothing measurable here - unlike filter_new()'s
// persistent per-block plans above, where MEASURE's payoff (a faster
// per-block transform, paid 96000/1024 ≈ 94 times a second) is worth
// its one-time search cost.
static void window_filter(int L, int M, complex float *response, float beta)
{
	int const N = L + M - 1;
	complex float *buffer = fftwf_alloc_complex(N);

	fftwf_plan fwd = fftwf_plan_dft_1d(N, buffer, buffer, FFTW_FORWARD, FFTW_ESTIMATE);
	fftwf_plan rev = fftwf_plan_dft_1d(N, buffer, buffer, FFTW_BACKWARD, FFTW_ESTIMATE);

	// Frequency response -> time-domain impulse response.
	memcpy(buffer, response, N * sizeof(*buffer));
	fftwf_execute(rev);

	float *kaiser_window = malloc(M * sizeof(float));
	make_kaiser(kaiser_window, M, beta);

	// Shift to make the impulse causal (time zero at M/2), then window
	// and zero-pad the rest - same order as the original.
	for (int n = M - 1; n >= 0; n--)
		buffer[n] = buffer[(n - M / 2 + N) % N];
	for (int n = M - 1; n >= 0; n--)
		buffer[n] = buffer[n] * kaiser_window[n];
	memset(buffer + M, 0, (N - M) * sizeof(*buffer));

	// Back to frequency domain - this is the actual filter response
	// filter_forward() will multiply live blocks by.
	fftwf_execute(fwd);
	memcpy(response, buffer, N * sizeof(*response));

	free(kaiser_window);
	fftwf_free(buffer);
	fftwf_destroy_plan(fwd);
	fftwf_destroy_plan(rev);
}

// Shared by filter_tune()/filter_tune_real() below - builds the
// brick-wall passband and rounds its edges via window_filter(); `mirror`
// is the only difference between the two public entry points (see their
// header comments for why that one bit matters).
static int filter_tune_ex(struct filter *f, float low, float high, float kaiser_beta, int mirror)
{
	if (isnan(low) || isnan(high) || isnan(kaiser_beta))
		return -1;

	// Start from a brick-wall passband in the frequency domain: gain
	// inside [low, high), zero outside - same normalized-frequency
	// convention as real sbitx's filter_tune() (s in [-0.5, 0.5) per
	// bin, folded so bins n > N/2 represent negative frequencies).
	// `mirror` additionally passes [-high, -low] - see
	// filter_tune_real()'s header comment for why a real-signal filter
	// needs that second interval too.
	//
	// gain = 1/N^2, not real sbitx's 1/N: window_filter() below is a
	// backward-FFT/window/forward-FFT round trip, and FFTW normalizes
	// neither direction, so each of those two transforms multiplies
	// magnitude by an extra, uncompensated N. 1/N here cancels only the
	// first (backward) one - real sbitx's own filter_tune() does exactly
	// that, leaving every filter's passband gain at a real, measured
	// +20*log10(N) (+66dB at this N=2048) that its rx_linear()/
	// tx_process() apparently never corrects for as such - it's
	// presumably just folded, unremarked, into whatever their other
	// empirically-bench-tuned gain constants (ssb_val, tx_amp, volume,
	// TX_GAIN_CORRECTION-alikes) happen to land on. Verified here by
	// running this bench harness with plain 1/N first: case A's
	// passband measured +66.23dB, not ~0dB, exactly 20*log10(2048).
	// Squaring it - 1/N^2 - cancels both transforms, so this port's
	// filter is unity-gain by construction: whatever gain constants get
	// added later (build order step 7's power/ALC calibration) are
	// calibrating real analog/mixer gain, not partly undoing an
	// unlabeled FFT-normalization artifact mixed in with it.
	float gain = 1. / ((float)f->N * (float)f->N);
	for (int n = 0; n < f->N; n++) {
		float s = (n <= f->N / 2) ? (float)n / f->N : (float)(n - f->N) / f->N;
		int pass = (s >= low && s <= high);
		if (mirror && !pass)
			pass = (s >= -high && s <= -low);
		f->fir_coeff[n] = pass ? gain : 0;
	}

	// Then round-trip it through window_filter() to replace that brick
	// wall's infinite-sharp (and therefore ringing) edges with the
	// Kaiser-windowed FIR's actual, finite transition - see
	// docs/ARCHITECTURE.md §4's derivation of what beta actually buys.
	window_filter(f->L, f->M, f->fir_coeff, kaiser_beta);
	return 0;
}

// Replaces the tuned response with a minimum-phase one of the SAME
// magnitude: identical passband and skirts, but with the group delay
// collapsed and the impulse decaying from time zero instead of being
// symmetric about its middle.
//
// Why it matters: filter_tune_ex() above centres a symmetric impulse at
// M/2, which is linear phase - constant group delay of (M-1)/2 samples.
// At the narrow-CW filter's M=3073 that is 16ms at 96kHz, and the
// impulse spreads symmetrically around it, so a keyed CW element needs
// ~20ms to reach within 3dB of its settled level. See
// dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md for the
// measurements and what it does to the sound.
//
// How: cepstral factorization. Every zero of the response outside the
// unit circle is reflected inside it, which is the one rearrangement
// that changes phase while leaving |H| untouched. Working in logs makes
// that a fold rather than a root-finding problem:
//
//   1. take log|H| (floored, since log 0 isn't a number),
//   2. inverse-transform it to the real cepstrum,
//   3. zero the anticausal half and double the causal half,
//   4. transform back and exponentiate.
//
// The magnitude survives because step 3 only touches the odd (phase)
// part of the log-spectrum, never the even (magnitude) part.
int filter_min_phase(struct filter *f)
{
	int const N = f->N;
	complex float *buf = fftwf_alloc_complex(N);
	if (!buf)
		return -1;

	fftwf_plan fwd = fftwf_plan_dft_1d(N, buf, buf, FFTW_FORWARD, FFTW_ESTIMATE);
	fftwf_plan rev = fftwf_plan_dft_1d(N, buf, buf, FFTW_BACKWARD, FFTW_ESTIMATE);

	// Floor the magnitude before taking its log. The stopband reaches
	// -80dB and true zeros do occur; MIN_PHASE_FLOOR_DB is far enough
	// below anything audible that clamping there can't change the
	// response, and it keeps log() finite.
	float peak = 0;
	for (int n = 0; n < N; n++) {
		float m = cabsf(f->fir_coeff[n]);
		if (m > peak)
			peak = m;
	}
	if (!(peak > 0)) {
		fftwf_free(buf);
		fftwf_destroy_plan(fwd);
		fftwf_destroy_plan(rev);
		return -1; // nothing tuned yet
	}
	float floor_mag = peak * powf(10.0f, MIN_PHASE_FLOOR_DB / 20.0f);

	for (int n = 0; n < N; n++) {
		float m = cabsf(f->fir_coeff[n]);
		buf[n] = logf(m > floor_mag ? m : floor_mag);
	}

	// Real cepstrum. FFTW normalizes neither direction, so the 1/N that
	// makes this a true inverse transform is applied by hand.
	fftwf_execute(rev);
	for (int n = 0; n < N; n++)
		buf[n] /= (float)N;

	// The fold: keep c[0] and the Nyquist term, double the causal half,
	// discard the anticausal half. This is what turns a zero-phase
	// log-spectrum into a minimum-phase one.
	for (int n = 1; n < N / 2; n++)
		buf[n] *= 2.0f;
	for (int n = N / 2 + 1; n < N; n++)
		buf[n] = 0;

	fftwf_execute(fwd);
	for (int n = 0; n < N; n++)
		buf[n] = cexpf(buf[n]);

	// Fit the impulse into M taps, the same budget filter_tune_ex()'s own
	// impulse respects. A minimum-phase impulse decays rather than
	// ending, so left alone its tail runs past tap M-1 - and
	// overlap-save's discard region is exactly M-1 samples long, so a
	// tail beyond that wraps into the block instead of being thrown away.
	// Measured before this step, that tail held -58dB of the impulse's
	// energy: inaudible in level, but it lands as an artifact repeating
	// at the block rate rather than as a smooth error in the response.
	//
	// The last MIN_PHASE_FADE_DIVISOR-th of the taps is faded out with a
	// raised cosine before the cut. A hard cut alone costs up to 5dB of
	// stopband depth (spectral leakage from the discontinuity); fading
	// first brings the response back to within 0.5dB of the linear-phase
	// filter it is supposed to match everywhere, which is what makes
	// "same magnitude" an honest claim. Both numbers are from the bench
	// sweep in docs/dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md.
	fftwf_execute(rev);
	for (int n = 0; n < N; n++)
		buf[n] /= (float)N;

	int fade = f->M / MIN_PHASE_FADE_DIVISOR;
	for (int n = 0; n < fade; n++) {
		float w = 0.5f + 0.5f * cosf((float)M_PI * (float)n / (float)fade);
		buf[f->M - fade + n] *= w;
	}
	memset(buf + f->M, 0, (size_t)(N - f->M) * sizeof(complex float));

	fftwf_execute(fwd);
	memcpy(f->fir_coeff, buf, (size_t)N * sizeof(complex float));

	fftwf_free(buf);
	fftwf_destroy_plan(fwd);
	fftwf_destroy_plan(rev);
	return 0;
}

int filter_tune(struct filter *f, float low, float high, float kaiser_beta)
{
	return filter_tune_ex(f, low, high, kaiser_beta, 0);
}

int filter_tune_real(struct filter *f, float low, float high, float kaiser_beta)
{
	return filter_tune_ex(f, low, high, kaiser_beta, 1);
}

void filter_forward(struct filter *f, const complex float *in)
{
	// [saved history | new block] - classic overlap-save framing.
	memcpy(f->time, f->history, (f->M - 1) * sizeof(complex float));
	memcpy(f->time + (f->M - 1), in, f->L * sizeof(complex float));

	// Save this block's own tail as next call's history, before the
	// forward FFT/plan_fwd overwrites f->time's role as "the next
	// buffer FFTW reads" - fftwf_execute() re-reads/rewrites the exact
	// buffers plan_fwd was created against, so f->time is safe to reuse
	// for the inverse FFT's output later (filter_inverse()) once this
	// copy is done.
	memcpy(f->history, f->time + f->N - (f->M - 1), (f->M - 1) * sizeof(complex float));

	fftwf_execute(f->plan_fwd);

	// Unconditional passband multiply - every mode gets this; anything
	// mode-specific (sideband zeroing, an IF bin-rotate) is the caller's
	// job on f->freq before filter_inverse() runs.
	for (int i = 0; i < f->N; i++)
		f->freq[i] *= f->fir_coeff[i];
}

void filter_inverse(struct filter *f, complex float *out)
{
	fftwf_execute(f->plan_rev);

	// Discard the first M-1 samples (circular-convolution wrap-around,
	// contaminated by whatever was in the *other* end of this block's
	// input) - keep the last L, which overlap-save guarantees are a
	// correct linear convolution.
	for (int i = 0; i < f->L; i++)
		out[i] = f->time[f->N - f->L + i];
}

// See fft_filter.h. history is the only carried state, so clearing it is
// the whole job.
void filter_reset(struct filter *f)
{
	memset(f->history, 0, (f->M - 1) * sizeof(complex float));
}

void filter_free(struct filter *f)
{
	fftwf_destroy_plan(f->plan_fwd);
	fftwf_destroy_plan(f->plan_rev);
	fftwf_free(f->fir_coeff);
	fftwf_free(f->history);
	fftwf_free(f->time);
	fftwf_free(f->freq);
	free(f);
}
