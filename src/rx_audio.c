// rx_audio.c
//
// Demodulates the receiver's baseband I/Q into audio for two consumers:
// the local speaker/headphones (out[], via the WM8731 codec cw.c's
// sidetone also uses) and the USB audio gadget (uac_out, see rx_audio.h).
//
// Signal chain, per sample except where noted:
//
//   1. Wide complex bandpass (ssb_filter_apply): keeps 0..~3000Hz of
//      positive baseband, rejects negative. Sideband selection
//      happens by conjugating the input first (or not) - see
//      rx_audio_process().
//   2. Demod: CW and CWR mix up to the selected CW pitch; USB/LSB take
//      the real part directly, so audio Hz == |RF - dial|.
//   3. Optional narrow selectivity, centered on that same pitch: an
//      elliptic IIR from a pre-designed bank (default) or rx_filter.c's
//      FFT filter. Both always run, so switching or un-bypassing never
//      clicks.
//
// Pitch and width are both selectable at runtime - 500-1000Hz in 100Hz
// steps, by 150/300/450/600Hz - see rx_audio_set_narrow_pitch()/_width() in
// rx_audio.h, and radio_set_cw_pitch() for why the pitch is normally moved
// from radio.c rather than here.
//   4. AGC, then rx_volume for out[] only.
//
// The AGC measures the RAW input magnitude, not any stage's output: its
// envelope has to be frequency-independent, or the gain undoes the very
// selectivity stages 1 and 3 provide. The consequence, by design: a
// strong signal anywhere in the captured band lowers the gain for
// everything ("AGC desense"), as with a real front-end AGC.
//
// Design rationale, measurements and the history of how each stage got
// here: docs/dsp_design_notes/rx_audio_demod_design.md (stages 1-4, AGC)
// and rx_uac_out_digital_mode_bandwidth.md §10 (sideband selection, I/Q
// inversion).

#include "rx_audio.h"
#include "cw.h"
#include "narrow_filter_bank.h"
#include "rx_filter.h"
#include "vfo.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>   // abs(), for the bank's nearest-value snap

#define SAMPLE_RATE_HZ 96000

#define SSB_FIR_TAPS 327

// Stage 1: wide image-reject complex bandpass. A real lowpass prototype,
//   scipy.signal.remez(SSB_FIR_TAPS, [0, 1500, 1900, 48000], [1, 0],
//                      weight=[1, 10], fs=96000)
// (~1.7dB ripple, -40dB stopband), modulated by exp(j*2*pi*1500*m/Fs), m
// counted from the center tap, so it passes 0..+3000Hz of baseband and
// rejects negative frequencies. Rejection is only a few dB right at 0Hz
// (nothing can separate +0 from -0), ~-10dB at -200Hz, and -40..-75dB
// from about -400Hz outward. Stored pre-reversed (ssb_hr[i] ==
// hr[N-1-i], same for ssb_hi) so ssb_filter_apply()'s loop computes
// y[n] = sum h[k]*x[n-k]. Derivation and verification:
// rx_audio_demod_design.md §7.
static const double ssb_hr[SSB_FIR_TAPS] = {
     0.00472215, -0.00002114, -0.00003969, -0.00006961, -0.00011114,
    -0.00016150, -0.00021969, -0.00028154, -0.00034457, -0.00040409,
    -0.00045675, -0.00049749, -0.00052287, -0.00052815, -0.00051054,
    -0.00046645, -0.00039427, -0.00029231, -0.00016067,  0.00000000,
     0.00018761,  0.00039892,  0.00062998,  0.00087507,  0.00112885,
     0.00138360,  0.00163348,  0.00186954,  0.00208671,  0.00227510,
     0.00243290,  0.00254717,  0.00263303,  0.00266151,  0.00264065,
     0.00257886,  0.00246894,  0.00231978,  0.00213206,  0.00191535,
     0.00167489,  0.00142112,  0.00116156,  0.00090716,  0.00066616,
     0.00044873,  0.00026227,  0.00011472,  0.00001135, -0.00004331,
    -0.00004725,  0.00000000,  0.00009665,  0.00023889,  0.00042126,
     0.00063610,  0.00087500,  0.00112753,  0.00138360,  0.00163154,
     0.00186146,  0.00206177,  0.00222514,  0.00234085,  0.00240789,
     0.00241989,  0.00237175,  0.00226963,  0.00211372,  0.00191145,
     0.00166944,  0.00139798,  0.00110810,  0.00081252,  0.00052378,
     0.00025532,  0.00001930, -0.00017243, -0.00031014, -0.00038575,
    -0.00039387, -0.00033165, -0.00019931,  0.00000000,  0.00026013,
     0.00057211,  0.00092454,  0.00130381,  0.00169486,  0.00208146,
     0.00244720,  0.00277575,  0.00305229,  0.00326292,  0.00339712,
     0.00344584,  0.00340487,  0.00327470,  0.00305457,  0.00275329,
     0.00238031,  0.00194957,  0.00147779,  0.00098353,  0.00048771,
     0.00001204, -0.00042216, -0.00079408, -0.00108504, -0.00127877,
    -0.00136258, -0.00132784, -0.00117059, -0.00089187, -0.00049789,
     0.00000000,  0.00058554,  0.00123806,  0.00193307,  0.00264318,
     0.00333898,  0.00399025,  0.00456706,  0.00504100,  0.00538654,
     0.00558190,  0.00561042,  0.00546133,  0.00512984,  0.00462087,
     0.00394329,  0.00311532,  0.00216248,  0.00111596,  0.00001411,
    -0.00110139, -0.00218483, -0.00318792, -0.00406261, -0.00476159,
    -0.00524066, -0.00545995, -0.00538558, -0.00499112, -0.00425865,
    -0.00317975, -0.00175614,  0.00000000,  0.00206603,  0.00440922,
     0.00698740,  0.00975003,  0.01263942,  0.01559237,  0.01854190,
     0.02141906,  0.02415516,  0.02668356,  0.02894165,  0.03087317,
     0.03242858,  0.03356919,  0.03426578,  0.03449970,  0.03426578,
     0.03356919,  0.03242858,  0.03087317,  0.02894165,  0.02668356,
     0.02415516,  0.02141906,  0.01854190,  0.01559237,  0.01263942,
     0.00975003,  0.00698740,  0.00440922,  0.00206603,  0.00000000,
    -0.00175614, -0.00317975, -0.00425865, -0.00499112, -0.00538558,
    -0.00545995, -0.00524066, -0.00476159, -0.00406261, -0.00318792,
    -0.00218483, -0.00110139,  0.00001411,  0.00111596,  0.00216248,
     0.00311532,  0.00394329,  0.00462087,  0.00512984,  0.00546133,
     0.00561042,  0.00558190,  0.00538654,  0.00504100,  0.00456706,
     0.00399025,  0.00333898,  0.00264318,  0.00193307,  0.00123806,
     0.00058554,  0.00000000, -0.00049789, -0.00089187, -0.00117059,
    -0.00132784, -0.00136258, -0.00127877, -0.00108504, -0.00079408,
    -0.00042216,  0.00001204,  0.00048771,  0.00098353,  0.00147779,
     0.00194957,  0.00238031,  0.00275329,  0.00305457,  0.00327470,
     0.00340487,  0.00344584,  0.00339712,  0.00326292,  0.00305229,
     0.00277575,  0.00244720,  0.00208146,  0.00169486,  0.00130381,
     0.00092454,  0.00057211,  0.00026013,  0.00000000, -0.00019931,
    -0.00033165, -0.00039387, -0.00038575, -0.00031014, -0.00017243,
     0.00001930,  0.00025532,  0.00052378,  0.00081252,  0.00110810,
     0.00139798,  0.00166944,  0.00191145,  0.00211372,  0.00226963,
     0.00237175,  0.00241989,  0.00240789,  0.00234085,  0.00222514,
     0.00206177,  0.00186146,  0.00163154,  0.00138360,  0.00112753,
     0.00087500,  0.00063610,  0.00042126,  0.00023889,  0.00009665,
     0.00000000, -0.00004725, -0.00004331,  0.00001135,  0.00011472,
     0.00026227,  0.00044873,  0.00066616,  0.00090716,  0.00116156,
     0.00142112,  0.00167489,  0.00191535,  0.00213206,  0.00231978,
     0.00246894,  0.00257886,  0.00264065,  0.00266151,  0.00263303,
     0.00254717,  0.00243290,  0.00227510,  0.00208671,  0.00186954,
     0.00163348,  0.00138360,  0.00112885,  0.00087507,  0.00062998,
     0.00039892,  0.00018761,  0.00000000, -0.00016067, -0.00029231,
    -0.00039427, -0.00046645, -0.00051054, -0.00052815, -0.00052287,
    -0.00049749, -0.00045675, -0.00040409, -0.00034457, -0.00028154,
    -0.00021969, -0.00016150, -0.00011114, -0.00006961, -0.00003969,
    -0.00002114,  0.00472215,
};

static const double ssb_hi[SSB_FIR_TAPS] = {
     0.00143245, -0.00000420, -0.00000391,  0.00000000,  0.00001095,
     0.00003213,  0.00006664,  0.00011662,  0.00018418,  0.00027000,
     0.00037485,  0.00049749,  0.00063712,  0.00079043,  0.00095516,
     0.00112611,  0.00129974,  0.00146956,  0.00163127,  0.00177775,
     0.00190482,  0.00200551,  0.00207676,  0.00211260,  0.00211193,
     0.00207071,  0.00199041,  0.00186954,  0.00171252,  0.00152017,
     0.00130041,  0.00105507,  0.00079872,  0.00052941,  0.00026008,
    -0.00000000, -0.00024317, -0.00046143, -0.00064675, -0.00079336,
    -0.00089525, -0.00094956, -0.00095327, -0.00090716, -0.00081172,
    -0.00067157, -0.00049067, -0.00027697, -0.00003741,  0.00021776,
     0.00047975,  0.00073727,  0.00098129,  0.00120098,  0.00138870,
     0.00153568,  0.00163702,  0.00168747,  0.00168592,  0.00163154,
     0.00152766,  0.00137763,  0.00118936,  0.00096961,  0.00073043,
     0.00048135,  0.00023360, -0.00000000, -0.00020818, -0.00038021,
    -0.00050642, -0.00057906, -0.00059229, -0.00054291, -0.00042986,
    -0.00025532, -0.00002352,  0.00025806,  0.00058023,  0.00093129,
     0.00129843,  0.00166733,  0.00202367,  0.00235273,  0.00264113,
     0.00287619,  0.00304780,  0.00314768,  0.00317087,  0.00311513,
     0.00298192,  0.00277575,  0.00250495,  0.00218021,  0.00181580,
     0.00142731,  0.00103286,  0.00065138,  0.00030085, -0.00000000,
    -0.00023444, -0.00038779, -0.00044828, -0.00040739, -0.00026069,
    -0.00000805,  0.00034646,  0.00079408,  0.00132212,  0.00191382,
     0.00254920,  0.00320569,  0.00385893,  0.00448372,  0.00505515,
     0.00554939,  0.00594513,  0.00622413,  0.00637248,  0.00638119,
     0.00624679,  0.00597184,  0.00556498,  0.00504100,  0.00442062,
     0.00372971,  0.00299883,  0.00226216,  0.00155612,  0.00091915,
     0.00038838, -0.00000000, -0.00021299, -0.00022198, -0.00000428,
     0.00045621,  0.00116782,  0.00213010,  0.00333410,  0.00476159,
     0.00638576,  0.00817139,  0.01007572,  0.01204963,  0.01403890,
     0.01598567,  0.01783041,  0.01951336,  0.02097680,  0.02216667,
     0.02303438,  0.02353865,  0.02364668,  0.02333563,  0.02259337,
     0.02141906,  0.01982362,  0.01782939,  0.01546963,  0.01278809,
     0.00983710,  0.00667733,  0.00337488,  0.00000000, -0.00337488,
    -0.00667733, -0.00983710, -0.01278809, -0.01546963, -0.01782939,
    -0.01982362, -0.02141906, -0.02259337, -0.02333563, -0.02364668,
    -0.02353865, -0.02303438, -0.02216667, -0.02097680, -0.01951336,
    -0.01783041, -0.01598567, -0.01403890, -0.01204963, -0.01007572,
    -0.00817139, -0.00638576, -0.00476159, -0.00333410, -0.00213010,
    -0.00116782, -0.00045621,  0.00000428,  0.00022198,  0.00021299,
     0.00000000, -0.00038838, -0.00091915, -0.00155612, -0.00226216,
    -0.00299883, -0.00372971, -0.00442062, -0.00504100, -0.00556498,
    -0.00597184, -0.00624679, -0.00638119, -0.00637248, -0.00622413,
    -0.00594513, -0.00554939, -0.00505515, -0.00448372, -0.00385893,
    -0.00320569, -0.00254920, -0.00191382, -0.00132212, -0.00079408,
    -0.00034646,  0.00000805,  0.00026069,  0.00040739,  0.00044828,
     0.00038779,  0.00023444,  0.00000000, -0.00030085, -0.00065138,
    -0.00103286, -0.00142731, -0.00181580, -0.00218021, -0.00250495,
    -0.00277575, -0.00298192, -0.00311513, -0.00317087, -0.00314768,
    -0.00304780, -0.00287619, -0.00264113, -0.00235273, -0.00202367,
    -0.00166733, -0.00129843, -0.00093129, -0.00058023, -0.00025806,
     0.00002352,  0.00025532,  0.00042986,  0.00054291,  0.00059229,
     0.00057906,  0.00050642,  0.00038021,  0.00020818,  0.00000000,
    -0.00023360, -0.00048135, -0.00073043, -0.00096961, -0.00118936,
    -0.00137763, -0.00152766, -0.00163154, -0.00168592, -0.00168747,
    -0.00163702, -0.00153568, -0.00138870, -0.00120098, -0.00098129,
    -0.00073727, -0.00047975, -0.00021776,  0.00003741,  0.00027697,
     0.00049067,  0.00067157,  0.00081172,  0.00090716,  0.00095327,
     0.00094956,  0.00089525,  0.00079336,  0.00064675,  0.00046143,
     0.00024317,  0.00000000, -0.00026008, -0.00052941, -0.00079872,
    -0.00105507, -0.00130041, -0.00152017, -0.00171252, -0.00186954,
    -0.00199041, -0.00207071, -0.00211193, -0.00211260, -0.00207676,
    -0.00200551, -0.00190482, -0.00177775, -0.00163127, -0.00146956,
    -0.00129974, -0.00112611, -0.00095516, -0.00079043, -0.00063712,
    -0.00049749, -0.00037485, -0.00027000, -0.00018418, -0.00011662,
    -0.00006664, -0.00003213, -0.00001095, -0.00000000,  0.00000391,
     0.00000420, -0.00143245,
};

struct ssb_filter_state {
    double hist_i[2 * SSB_FIR_TAPS];
    double hist_q[2 * SSB_FIR_TAPS];
    int pos;   // write cursor, always in [0, SSB_FIR_TAPS)
};

static struct ssb_filter_state ssb_state;

// Complex convolution of the (I + jQ) history with (ssb_hr + j*ssb_hi):
//   i_out = sum(hr*hist_i) - sum(hi*hist_q)
//   q_out = sum(hr*hist_q) + sum(hi*hist_i)
// Each sample is written twice (pos and pos+TAPS) so a TAPS-long read
// never wraps and the loop stays branch-free for -O3 autovectorization,
// same trick as antialias.c. Deliberately not folded to exploit hr/hi's
// symmetry: mirrored indexing vectorizes worse, and ~125M MAC/s is no
// load for a Pi 4.
static void ssb_filter_apply(struct ssb_filter_state *f, double i_in, double q_in,
                              double *i_out, double *q_out) {
    f->hist_i[f->pos] = i_in;
    f->hist_i[f->pos + SSB_FIR_TAPS] = i_in;
    f->hist_q[f->pos] = q_in;
    f->hist_q[f->pos + SSB_FIR_TAPS] = q_in;

    int base = f->pos + 1;
    double acc_re = 0.0, acc_im = 0.0;
    for (int i = 0; i < SSB_FIR_TAPS; i++) {
        double hi_val = f->hist_i[base + i];
        double hq_val = f->hist_q[base + i];
        acc_re += ssb_hr[i] * hi_val - ssb_hi[i] * hq_val;
        acc_im += ssb_hr[i] * hq_val + ssb_hi[i] * hi_val;
    }

    f->pos++;
    if (f->pos == SSB_FIR_TAPS) f->pos = 0;
    *i_out = acc_re;
    *q_out = acc_im;
}

// Stage 3: narrow real bandpass - an 8-pole elliptic (Cauer) IIR, as 4
// direct-form biquad sections, ~1.9:1 shape factor (CW crystal-filter
// territory) and a few ms of group delay at center. Elliptic coefficients
// need offline design, so rather than one fixed filter there is a bank of
// 24, pre-designed at six pitches and four widths and selected at
// runtime - narrow_filter_bank.h, generated and verified by
// tools/gen_narrow_filters.py. The 700Hz/300Hz entry is bit-for-bit the
// single filter this stage used to carry, so the default is unchanged.
//
// Why a bank of fixed filters rather than the FFT filter's continuous
// pitch/width: the FFT path is the tunable one and it is still here
// (rx_filter.c), but a linear-phase FIR narrow enough for CW costs 16ms of
// group delay, and even its minimum-phase realization settles a keyed
// element in 8.8ms against this stage's 5.2ms at the same width. Switching
// between pre-designed elliptic sets keeps the attack and buys quantized
// pitch/width instead of continuous. See
// docs/dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md §12.
//
// Why elliptic rather than a resonator cascade: rx_audio_demod_design.md §8.
struct biquad_state {
    double b0, b1, b2, a1, a2;   // coefficients
    double x1, x2, y1, y2;       // history
};

#define NARROW_FILTER_SECTIONS NARROW_BANK_SECTIONS

struct narrow_filter_state {
    struct biquad_state stage[NARROW_FILTER_SECTIONS];
};

static struct narrow_filter_state narrow_filter;

// Which bank entry is loaded. Indices, not frequencies, since the bank's
// own tables are what define the selectable values; the public setters
// snap a requested Hz to the nearest one and report back what they chose.
static int narrow_pitch_idx = 1;   // 700 Hz, checked against the bank in rx_audio_init()
static int narrow_width_idx = 1;   // 300 Hz

// Index of the bank value nearest `hz`. Ties go to the lower entry, which
// only matters for a request exactly between two rungs.
static int nearest_index(const int *table, int n, int hz) {
    int best = 0;
    for (int i = 1; i < n; i++)
        if (abs(hz - table[i]) < abs(hz - table[best]))
            best = i;
    return best;
}

// Copies a bank entry's coefficients into the running filter, leaving
// x1/x2/y1/y2 alone.
//
// Keeping the history across a coefficient change is deliberate. The
// alternative - zeroing it - measures no better: swapping under a steady
// tone overshoots by at most 2.0dB either way and settles within 14ms, and
// on the widening direction keeping the history actually settles faster
// (measured, see the design note's §12). Zeroing would also throw away a
// filter's worth of signal every time the operator touched the control.
//
// Not atomic against the audio thread: it can be mid-block in
// narrow_filter_apply() while these 20 doubles are rewritten, so one block
// may be computed with a half-updated set. That is the same non-atomic
// retune rx_filter_retune() has always documented, and at 2dB of
// transient it is an audible edge when the operator turns a knob, not a
// defect that appears on its own.
static void narrow_filter_load(int pitch_idx, int width_idx) {
    for (int i = 0; i < NARROW_FILTER_SECTIONS; i++) {
        const double *c = narrow_bank_coeffs[pitch_idx][width_idx][i];
        narrow_filter.stage[i].b0 = c[0];
        narrow_filter.stage[i].b1 = c[1];
        narrow_filter.stage[i].b2 = c[2];
        narrow_filter.stage[i].a1 = c[3];
        narrow_filter.stage[i].a2 = c[4];
    }
}

static double biquad_apply(struct biquad_state *f, double x) {
    double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
             - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

static double narrow_filter_apply(struct narrow_filter_state *f, double x) {
    double y = x;
    for (int i = 0; i < NARROW_FILTER_SECTIONS; i++)
        y = biquad_apply(&f->stage[i], y);
    return y;
}

// Stage 4: AGC. Band levels swing over orders of magnitude, so no fixed
// gain works (rx_audio_demod_design.md §5).
//
// Level the AGC rides the envelope toward - well below the +-2e9 output
// clamp, so peaks above the smoothed envelope still fit. usb_gadget.c's
// UAC_RX_AUDIO_SCALE is defined relative to this; change them together.
#define AGC_TARGET_AMPLITUDE 500000000.0

// Fast attack so a strong signal keying up doesn't clip; slow release so
// gain rides the band level rather than pumping between CW elements.
#define AGC_ATTACK_MS    5.0
#define AGC_RELEASE_MS 300.0

// Caps the gain on a near-silent input, so a quiet band doesn't turn into
// full-scale hiss.
#define AGC_MAX_GAIN 8.0e11

// Mixing oscillator for CW/CWR, running at the selected pitch - moved by
// narrow_select() below, not fixed at CW_PITCH_HZ.
static struct vfo bfo;
// Local speaker volume, 0-100%, log (audio) taper: 1..100% maps evenly
// onto -RX_VOLUME_RANGE_DB..0dB relative to RX_VOLUME_MAX, so every 1% is
// 0.5dB; 0% is a true mute. RX_VOLUME_MAX is the loudest the local
// speaker needs on this hardware. The default, 67%, is 0.03 linear -
// comfortable copy. Affects out[] only; uac_out is tapped before
// rx_volume.
#define RX_VOLUME_MAX 0.20
#define RX_VOLUME_RANGE_DB 50.0
#define RX_VOLUME_DEFAULT_PERCENT 67

static double volume_percent_to_gain(int percent) {
    if (percent <= 0)
        return 0.0;
    double db = ((double)percent - 100.0) / 100.0 * RX_VOLUME_RANGE_DB;
    return RX_VOLUME_MAX * pow(10.0, db / 20.0);
}

// Stored as the caller's integer percent so get_volume() reads back
// exactly what was set. rx_volume is derived from it, including in
// rx_audio_init(), since pow() can't appear in a static initializer.
static int rx_volume_percent = RX_VOLUME_DEFAULT_PERCENT;
static double rx_volume = 0.03;

// 1 = stage 3 applied (default), 0 = bypassed. See rx_audio_set_narrow_filter().
static int narrow_filter_enabled = 1;

// Which stage 3 implementation runs when enabled. The FFT filter is the
// default: it tunes pitch and width continuously rather than to the bank's
// rungs, and its minimum-phase realization brought a keyed element's
// settling time to 8.8ms against the elliptic's 5.2ms - close enough that
// continuous tuning wins. The elliptic bank stays selectable and is NOT
// deprecated: it is four biquads per sample against a 4096-point transform
// every block, which matters on a Pi Zero 2W, and it is what the
// odd-block-size path in rx_audio_process() falls back to. See
// docs/dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md.
static enum rx_narrow_filter_impl narrow_filter_impl = RX_NARROW_FILTER_FFT;

// The FFT implementation always runs minimum phase now; linear phase has no
// operator-facing control. See rx_audio_set_narrow_filter_min_phase() in
// rx_audio.h for why the setter still exists.
static int narrow_filter_min_phase = 1;

// maxibitx's raw baseband I/Q is spectrally inverted: a station +d Hz
// above dial arrives at baseband -d. (sound.c mixes the ~24kHz IF by
// e^{+j*2pi*RX_IF_FREQ_HZ*t}, mapping IF f to 24000-f, and the analog
// chain puts the IF at 24000+d.) Sideband selection compensates when
// deciding whether to conjugate the input - see rx_audio_process(). Deliberately not corrected in sound.c:
// that would flip every other I/Q consumer too, and hpsdr_p1.c (e.g.
// SparkSDR) works as-is. Checked on air: in USB, a +100Hz dial step
// moves signals left in WSJT-X. Set to 0 for a board whose analog chain
// isn't inverted. Derivation: rx_uac_out_digital_mode_bandwidth.md §10.
#define RX_IQ_SPECTRUM_INVERTED 1

// Current demodulator, set via rx_audio_set_demod() from radio_set_mode().
// CW matches radio.c's startup mode.
static enum rx_demod demod = RX_DEMOD_CW;

// Stage 3's FFT implementation (rx_filter.c). One persistent instance,
// since its FFTW plans are expensive to build. Never freed: this module
// has no shutdown path, and the OS reclaims it at exit.
static struct rx_filter *rx_fft_filter;

// AGC envelope: smoothed magnitude of the RAW input I/Q (see file
// header). gain = AGC_TARGET_AMPLITUDE / agc_env.
static double agc_env = 0.0;
static double agc_attack_alpha, agc_release_alpha;

// S-meter envelope: smoothed |stage 3 output| (or its bypass), i.e. what
// actually reaches the speaker. Read-only - it never feeds the gain.
// See rx_audio_get_strength_db().
static double meter_env = 0.0;

// Time-constant (not cutoff-frequency) one-pole coefficient, for the
// AGC's attack/release smoothing - alpha such that a step input reaches
// ~63% of the way there after time_ms.
static double onepole_alpha_from_ms(double time_ms) {
    double dt = 1.0 / (double)SAMPLE_RATE_HZ;
    double tau = time_ms / 1000.0;
    return 1.0 - exp(-dt / tau);
}

void rx_audio_init(void) {
    rx_volume = volume_percent_to_gain(rx_volume_percent);

    // Start on the bank entry nearest CW_PITCH_HZ, so the compile-time
    // pitch stays the thing that defines the default even if the bank's
    // rungs are ever regenerated at other frequencies.
    narrow_pitch_idx = nearest_index(narrow_bank_pitch_hz, NARROW_BANK_PITCHES, CW_PITCH_HZ);
    narrow_width_idx = nearest_index(narrow_bank_width_hz, NARROW_BANK_WIDTHS,
                                     (int)RX_FILTER_DEFAULT_WIDTH_HZ);

    // CW BFO at the selected pitch: a station at dial center is heard at
    // that pitch, and one d Hz above dial at pitch + d (CW keeps the upper
    // side, same as USB - see rx_audio_process()). This is the software
    // audio-frequency oscillator stage 2 mixes with, NOT hw_settings.ini's
    // bfo_freq (the ~22.6kHz hardware crystal-filter BFO tx_pipeline.c
    // derives its IF placement from) - moving this one does not touch
    // where the radio transmits.
    vfo_start(&bfo, narrow_bank_pitch_hz[narrow_pitch_idx], 0);

    for (int i = 0; i < 2 * SSB_FIR_TAPS; i++) {
        ssb_state.hist_i[i] = 0.0;
        ssb_state.hist_q[i] = 0.0;
    }
    ssb_state.pos = 0;

    narrow_filter_load(narrow_pitch_idx, narrow_width_idx);
    for (int i = 0; i < NARROW_FILTER_SECTIONS; i++) {
        narrow_filter.stage[i].x1 = narrow_filter.stage[i].x2 = 0.0;
        narrow_filter.stage[i].y1 = narrow_filter.stage[i].y2 = 0.0;
    }

    agc_attack_alpha  = onepole_alpha_from_ms(AGC_ATTACK_MS);
    agc_release_alpha = onepole_alpha_from_ms(AGC_RELEASE_MS);
    agc_env = 0.0;
    meter_env = 0.0;

    // Same pitch and width as the elliptic bank entry above, so switching
    // implementations compares two filters aimed at the same passband
    // rather than two different passbands.
    rx_fft_filter = rx_filter_new((float)narrow_bank_pitch_hz[narrow_pitch_idx],
                                  (float)narrow_bank_width_hz[narrow_width_idx]);
    rx_filter_set_min_phase(rx_fft_filter, narrow_filter_min_phase);
}

void rx_audio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    rx_volume_percent = percent;
    rx_volume = volume_percent_to_gain(percent);
}

int rx_audio_get_volume(void) {
    return rx_volume_percent;
}

void rx_audio_set_narrow_filter(int enable) {
    narrow_filter_enabled = (enable != 0);
}

int rx_audio_get_narrow_filter(void) {
    return narrow_filter_enabled;
}

void rx_audio_set_narrow_filter_impl(int use_fft) {
    narrow_filter_impl = use_fft ? RX_NARROW_FILTER_FFT : RX_NARROW_FILTER_ELLIPTIC;
}

int rx_audio_get_narrow_filter_impl(void) {
    return narrow_filter_impl == RX_NARROW_FILTER_FFT;
}

void rx_audio_set_narrow_filter_min_phase(int min_phase) {
    narrow_filter_min_phase = (min_phase != 0);
    // Re-designs fir_coeff. The audio thread may be mid-block in
    // filter_forward()/filter_inverse() while this runs, so one block can
    // see a partly-rewritten response - the same non-atomic retune
    // rx_filter_retune() has always documented, and the reason this is a
    // control-thread call: an audible one-block edge when the operator
    // flips a switch, not a glitch that happens on its own.
    rx_filter_set_min_phase(rx_fft_filter, narrow_filter_min_phase);
}

int rx_audio_get_narrow_filter_min_phase(void) {
    return narrow_filter_min_phase;
}

// Shared by the two setters below: load the selected bank entry, move the
// BFO to the selected pitch, and point the FFT implementation at the same
// passband.
//
// The BFO move is what makes pitch selection mean anything. Without it the
// demodulated tone stays where it was and a new pitch only detunes the
// filter off it - at the 150Hz width that is 38dB of attenuation on the
// operator's own signal, while at 600Hz it does nothing observable. Moving
// both together is the whole control.
//
// vfo_start() quantizes to a 65536-entry phase table, so 96000/65536 =
// 1.46Hz steps: the bank's rungs land at 599.5/698.7/799.8Hz rather than
// exactly. Against a 150Hz-wide filter centered on the nominal value that
// is a fraction of a percent of the passband, and it is the same
// quantization the BFO has always had at CW_PITCH_HZ.
static void narrow_select(int pitch_idx, int width_idx) {
    int pitch_hz = narrow_bank_pitch_hz[pitch_idx];
    int width_hz = narrow_bank_width_hz[width_idx];

    narrow_pitch_idx = pitch_idx;
    narrow_width_idx = width_idx;
    narrow_filter_load(pitch_idx, width_idx);
    vfo_start(&bfo, pitch_hz, 0);
    rx_filter_retune(rx_fft_filter, (float)pitch_hz, (float)width_hz);
}

int rx_audio_set_narrow_pitch(int hz) {
    narrow_select(nearest_index(narrow_bank_pitch_hz, NARROW_BANK_PITCHES, hz),
                  narrow_width_idx);
    return narrow_bank_pitch_hz[narrow_pitch_idx];
}

int rx_audio_get_narrow_pitch(void) {
    return narrow_bank_pitch_hz[narrow_pitch_idx];
}

int rx_audio_set_narrow_width(int hz) {
    narrow_select(narrow_pitch_idx,
                  nearest_index(narrow_bank_width_hz, NARROW_BANK_WIDTHS, hz));
    return narrow_bank_width_hz[narrow_width_idx];
}

int rx_audio_get_narrow_width(void) {
    return narrow_bank_width_hz[narrow_width_idx];
}

int rx_audio_narrow_pitch_count(void) {
    return NARROW_BANK_PITCHES;
}

int rx_audio_narrow_width_count(void) {
    return NARROW_BANK_WIDTHS;
}

int rx_audio_narrow_pitch_at(int index) {
    if (index < 0 || index >= NARROW_BANK_PITCHES)
        return -1;
    return narrow_bank_pitch_hz[index];
}

int rx_audio_narrow_width_at(int index) {
    if (index < 0 || index >= NARROW_BANK_WIDTHS)
        return -1;
    return narrow_bank_width_hz[index];
}

void rx_audio_set_demod(enum rx_demod d) {
    // No filter reset: a mode change leaves at most one stage-1 length
    // (~3.4ms) of the previous sideband in flight.
    demod = d;
}

enum rx_demod rx_audio_get_demod(void) {
    return demod;
}

double rx_audio_debug_agc_envelope(void) {
    return agc_env;
}

double rx_audio_debug_meter_envelope(void) {
    return meter_env;
}

// --- Signal strength (rigctld "l STRENGTH") ---
//
// Reads meter_env, not agc_env: agc_env is wideband by design (see the
// file header), so it would read the same for a signal at dial center or
// 10kHz away. meter_env tracks what's actually audible.
//
// Units are dBFS, relative only. RX_STRENGTH_S9_DBFS is a placeholder,
// not a calibration: calibrating needs a known signal (-73dBm = S9)
// injected at the antenna port. See rx_gain_and_level_calibration.md §2
// and §9.
#define RX_STRENGTH_S9_DBFS -40.0

// Reported range, dB relative to S9: S0 (-54) up to S9+60. Clamps the
// report only, not the envelope or the AGC.
#define RX_STRENGTH_MIN_DB (-54)
#define RX_STRENGTH_MAX_DB   60

int rx_audio_get_strength_db(void) {
    // Guard against log10(0) the same way the AGC's own gain math guards
    // agc_env's denominator, just up here instead of at every call site.
    double dbfs = 20.0 * log10(meter_env > 1e-12 ? meter_env : 1e-12);
    double rel = dbfs - RX_STRENGTH_S9_DBFS;
    if (rel < RX_STRENGTH_MIN_DB) rel = RX_STRENGTH_MIN_DB;
    if (rel > RX_STRENGTH_MAX_DB) rel = RX_STRENGTH_MAX_DB;
    return (int)(rel >= 0.0 ? rel + 0.5 : rel - 0.5);  // round half away from zero
}

// Largest block rx_audio_process() handles - matches sound.c's MAX_FRAMES.
#define RX_AUDIO_MAX_BLOCK 4096

void rx_audio_process(const double *i_samples, const double *q_samples,
                       int n, int32_t *out, double *uac_out) {
    // Two passes. Stages 1-2 and the elliptic filter run per sample into
    // these buffers; the block-based FFT filter then runs once over
    // audio_buf; then stage 3 selection and stage 4 run per sample.
    static float audio_buf[RX_AUDIO_MAX_BLOCK];
    static float elliptic_buf[RX_AUDIO_MAX_BLOCK];
    static float fft_buf[RX_AUDIO_MAX_BLOCK];
    int have_fft = 0;

    if (n > RX_AUDIO_MAX_BLOCK)
        n = RX_AUDIO_MAX_BLOCK;

    // Sideband selection. Stage 1 keeps positive baseband, so choosing a
    // sideband means choosing whether to conjugate the input first. With the
    // I/Q inverted, the upper side (above dial) sits at negative baseband and
    // needs it; the lower side doesn't. CW and USB keep the upper side, LSB
    // and CWR the lower - CWR is CW's BFO on LSB's side (rx_audio.h).
    int upper = (demod != RX_DEMOD_LSB && demod != RX_DEMOD_CWR);
    int conjugate = upper ? RX_IQ_SPECTRUM_INVERTED : !RX_IQ_SPECTRUM_INVERTED;

    for (int k = 0; k < n; k++) {
        // Stage 1: image-reject bandpass.
        double fi, fq;
        double q_in = conjugate ? -q_samples[k] : q_samples[k];
        ssb_filter_apply(&ssb_state, i_samples[k], q_in, &fi, &fq);

        // Stage 2: demod. CW and CWR mix up to the selected pitch:
        //   Re[(fi + j*fq)(cos + j*sin)] = fi*cos - fq*sin
        // USB/LSB take the real part directly, so audio Hz == |RF - dial|,
        // which is what WSJT-X assumes. The BFO advances every sample
        // regardless, keeping its phase continuous across mode changes.
        int bfo_cos, bfo_sin;
        vfo_read_iq(&bfo, &bfo_cos, &bfo_sin);
        double audio;
        if (demod == RX_DEMOD_CW || demod == RX_DEMOD_CWR) {
            double c = (double)bfo_cos / 1073741824.0;
            double s = (double)bfo_sin / 1073741824.0;
            audio = fi * c - fq * s;
        } else {
            audio = fi;
        }
        audio_buf[k] = (float)audio;

        // Stage 3, elliptic - always run, so its state is warm if selected.
        elliptic_buf[k] = (float)narrow_filter_apply(&narrow_filter, audio);
    }

    // Stage 3, FFT - block-based, and also always run. rx_filter needs
    // exactly RX_FILTER_BLOCK_LEN samples (sound.c's PERIOD_FRAMES); any
    // other size would misalign its overlap-save history, so that block is
    // skipped and the elliptic output used instead.
    if (n == RX_FILTER_BLOCK_LEN) {
        rx_filter_process_block(rx_fft_filter, audio_buf, fft_buf);
        have_fft = 1;
    } else if (narrow_filter_impl == RX_NARROW_FILTER_FFT) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                    "rx_audio: block size %d != %d (rx_filter's fixed "
                    "block length) - falling back to the elliptic filter "
                    "for this block only (further occurrences not "
                    "logged)\n",
                    n, RX_FILTER_BLOCK_LEN);
            warned = 1;
        }
    }

    for (int k = 0; k < n; k++) {
        double audio = audio_buf[k];
        double narrowed;
        if (!narrow_filter_enabled)
            narrowed = audio;
        else if (narrow_filter_impl == RX_NARROW_FILTER_FFT && have_fft)
            narrowed = fft_buf[k];
        else
            narrowed = elliptic_buf[k];

        // Stage 4: AGC on the raw input magnitude (see file header) - fast
        // attack, slow release - applied to stage 3's output.
        double mag = sqrt(i_samples[k] * i_samples[k] + q_samples[k] * q_samples[k]);
        double alpha = (mag > agc_env) ? agc_attack_alpha : agc_release_alpha;
        agc_env += alpha * (mag - agc_env);

        double gain = AGC_TARGET_AMPLITUDE / (agc_env > 1e-9 ? agc_env : 1e-9);
        if (gain > AGC_MAX_GAIN) gain = AGC_MAX_GAIN;

        // S-meter envelope - see meter_env.
        double meter_mag = fabs(narrowed);
        double meter_alpha = (meter_mag > meter_env) ? agc_attack_alpha : agc_release_alpha;
        meter_env += meter_alpha * (meter_mag - meter_env);

        // Post-AGC, pre-volume - see rx_audio.h.
        if (uac_out)
            uac_out[k] = narrowed * gain;

        double sample = narrowed * gain * rx_volume;
        if (sample >  2000000000.0) sample =  2000000000.0;
        if (sample < -2000000000.0) sample = -2000000000.0;
        out[k] = (int32_t)sample;
    }
}
