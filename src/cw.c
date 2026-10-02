// cw.c
//
// Keys the radio from the key jack and generates the keyed tone that feeds
// both the local sidetone and tx_pipeline.c (via sound.c). key_input.c
// delivers the jack's edges; once per block this file takes them, each at
// its own sample, and hands them to the keyer (keyer.h), which turns them
// into a key value per sample - a straight key following the contacts, or a
// bug, ultimatic or iambic keyer timing elements - one block late. In CW and
// CWR that keys the transmitter, with semi break-in; in USB/LSB the ring
// (sbitx's PTT line) is a mic PTT switch.
//
// Keying envelope: a table-driven Blackman-Harris ramp, 480 samples (5ms
// at 96kHz), from ~0 to 1.0 - similar to sBitx's own keyer (modem_cw.c).
// Read forward for attack and backward for decay. A different keying
// shape is a table swap, not a logic change.
//
// Weighting: the table is the rising half of a Blackman-Harris window, so it
// crosses 50% well past its midpoint. Read forward it reaches 50% late, read
// backward it reaches 50% early, and a mark measured between the 50% points
// comes out shorter than the key was down - by 150 samples (1.56ms) for this
// table. So every fall is held off by that many samples after key-up
// (weighting_hold below, computed from the table at cw_init()), which makes
// the transmitted mark equal the key-down time at the 50% points: 1:1
// weighting. docs/dsp_design_notes/cw_keyer_design_study.md §4 and §17.

#include "cw.h"
#include "key_input.h"
#include "keyer.h"
#include "radio.h"
#include "vfo.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#define CW_ENVELOPE_LEN 480
#define CW_SAMPLE_RATE 96000 // sound.c's rate, for block times
#define CW_MAX_EVENTS 64     // edges taken per block; any beyond wait a block
#define CW_MAX_BLOCK 4096    // sound.c's MAX_FRAMES
// How much later than one period after the last boundary estimate the next
// may be: 1 us per block, ~94 ppm - room for error in the period estimate,
// which follows the codec's clock against CLOCK_MONOTONIC.
#define CW_BOUNDARY_SLEW_NS 1000.0
// The period estimate averages the time between reads over this many blocks
// (~11 s): the reads' latencies cancel between one read and the next, so
// the average converges on the codec's true period.
#define CW_PERIOD_AVERAGE_BLOCKS 1024.0

// Semi break-in: audio blocks to hold TX after the key goes up, so the
// relay doesn't chatter between elements. 28 x ~10.7ms = ~300ms. A paddle
// keyer or text holds it at least a word space, 7 dits, which is longer
// below ~28 WPM (hang_polls()).
#define CW_HANG_POLLS 28

static const double cw_envelope[CW_ENVELOPE_LEN] = {
    0.000060, 0.000061, 0.000062, 0.000065, 0.000070, 0.000075, 0.000082, 0.000090, 0.000099,
    0.000110, 0.000121, 0.000135, 0.000149, 0.000165, 0.000182, 0.000200, 0.000220, 0.000241,
    0.000264, 0.000288, 0.000314, 0.000341, 0.000369, 0.000400, 0.000432, 0.000465, 0.000500,
    0.000537, 0.000576, 0.000617, 0.000659, 0.000704, 0.000750, 0.000798, 0.000848, 0.000901,
    0.000956, 0.001012, 0.001071, 0.001133, 0.001197, 0.001263, 0.001331, 0.001403, 0.001477,
    0.001553, 0.001633, 0.001715, 0.001800, 0.001888, 0.001979, 0.002073, 0.002171, 0.002271,
    0.002375, 0.002483, 0.002594, 0.002709, 0.002827, 0.002949, 0.003075, 0.003205, 0.003339,
    0.003477, 0.003620, 0.003766, 0.003917, 0.004073, 0.004233, 0.004398, 0.004568, 0.004743,
    0.004923, 0.005108, 0.005298, 0.005494, 0.005695, 0.005901, 0.006114, 0.006332, 0.006556,
    0.006786, 0.007023, 0.007265, 0.007515, 0.007770, 0.008032, 0.008302, 0.008578, 0.008861,
    0.009151, 0.009448, 0.009753, 0.010066, 0.010386, 0.010714, 0.011050, 0.011394, 0.011747,
    0.012108, 0.012477, 0.012855, 0.013242, 0.013637, 0.014042, 0.014456, 0.014880, 0.015313,
    0.015756, 0.016208, 0.016671, 0.017144, 0.017627, 0.018120, 0.018624, 0.019139, 0.019665,
    0.020201, 0.020749, 0.021309, 0.021880, 0.022462, 0.023057, 0.023663, 0.024282, 0.024913,
    0.025556, 0.026212, 0.026881, 0.027563, 0.028258, 0.028967, 0.029689, 0.030424, 0.031173,
    0.031937, 0.032714, 0.033506, 0.034312, 0.035133, 0.035968, 0.036819, 0.037684, 0.038565,
    0.039461, 0.040373, 0.041301, 0.042244, 0.043204, 0.044180, 0.045173, 0.046182, 0.047207,
    0.048250, 0.049310, 0.050387, 0.051481, 0.052593, 0.053723, 0.054870, 0.056036, 0.057219,
    0.058421, 0.059642, 0.060881, 0.062139, 0.063416, 0.064712, 0.066027, 0.067362, 0.068717,
    0.070091, 0.071484, 0.072898, 0.074332, 0.075787, 0.077261, 0.078756, 0.080272, 0.081809,
    0.083367, 0.084946, 0.086546, 0.088167, 0.089810, 0.091475, 0.093161, 0.094869, 0.096599,
    0.098352, 0.100126, 0.101923, 0.103742, 0.105584, 0.107448, 0.109335, 0.111245, 0.113178,
    0.115134, 0.117113, 0.119115, 0.121141, 0.123190, 0.125263, 0.127359, 0.129479, 0.131622,
    0.133790, 0.135981, 0.138196, 0.140435, 0.142699, 0.144986, 0.147298, 0.149633, 0.151994,
    0.154378, 0.156787, 0.159220, 0.161678, 0.164160, 0.166666, 0.169198, 0.171753, 0.174334,
    0.176938, 0.179568, 0.182222, 0.184901, 0.187604, 0.190332, 0.193084, 0.195861, 0.198663,
    0.201489, 0.204340, 0.207215, 0.210114, 0.213038, 0.215987, 0.218959, 0.221956, 0.224978,
    0.228023, 0.231093, 0.234186, 0.237304, 0.240445, 0.243610, 0.246799, 0.250012, 0.253248,
    0.256508, 0.259791, 0.263097, 0.266427, 0.269779, 0.273154, 0.276553, 0.279973, 0.283417,
    0.286883, 0.290371, 0.293881, 0.297413, 0.300967, 0.304542, 0.308139, 0.311758, 0.315397,
    0.319057, 0.322739, 0.326440, 0.330162, 0.333905, 0.337667, 0.341449, 0.345251, 0.349072,
    0.352912, 0.356771, 0.360649, 0.364545, 0.368459, 0.372392, 0.376342, 0.380309, 0.384294,
    0.388296, 0.392314, 0.396349, 0.400400, 0.404466, 0.408549, 0.412646, 0.416759, 0.420886,
    0.425027, 0.429183, 0.433353, 0.437535, 0.441731, 0.445940, 0.450161, 0.454395, 0.458640,
    0.462897, 0.467165, 0.471443, 0.475732, 0.480031, 0.484340, 0.488658, 0.492986, 0.497321,
    0.501665, 0.506017, 0.510376, 0.514743, 0.519116, 0.523495, 0.527881, 0.532271, 0.536667,
    0.541068, 0.545473, 0.549882, 0.554294, 0.558709, 0.563127, 0.567547, 0.571969, 0.576393,
    0.580817, 0.585241, 0.589666, 0.594090, 0.598514, 0.602936, 0.607356, 0.611775, 0.616190,
    0.620603, 0.625012, 0.629417, 0.633817, 0.638213, 0.642603, 0.646987, 0.651365, 0.655736,
    0.660100, 0.664456, 0.668804, 0.673144, 0.677474, 0.681795, 0.686105, 0.690405, 0.694694,
    0.698971, 0.703236, 0.707489, 0.711729, 0.715955, 0.720168, 0.724366, 0.728549, 0.732716,
    0.736868, 0.741004, 0.745123, 0.749224, 0.753308, 0.757373, 0.761420, 0.765447, 0.769455,
    0.773442, 0.777409, 0.781355, 0.785279, 0.789181, 0.793060, 0.796917, 0.800750, 0.804559,
    0.808344, 0.812103, 0.815838, 0.819546, 0.823229, 0.826885, 0.830513, 0.834114, 0.837687,
    0.841231, 0.844746, 0.848232, 0.851688, 0.855114, 0.858509, 0.861873, 0.865206, 0.868506,
    0.871774, 0.875010, 0.878212, 0.881380, 0.884514, 0.887614, 0.890679, 0.893709, 0.896703,
    0.899661, 0.902582, 0.905467, 0.908315, 0.911125, 0.913897, 0.916631, 0.919326, 0.921982,
    0.924599, 0.927177, 0.929714, 0.932211, 0.934667, 0.937082, 0.939456, 0.941788, 0.944078,
    0.946326, 0.948531, 0.950694, 0.952813, 0.954889, 0.956921, 0.958909, 0.960853, 0.962752,
    0.964607, 0.966417, 0.968181, 0.969900, 0.971573, 0.973200, 0.974781, 0.976316, 0.977803,
    0.979245, 0.980639, 0.981986, 0.983285, 0.984537, 0.985742, 0.986898, 0.988006, 0.989067,
    0.990078, 0.991042, 0.991957, 0.992823, 0.993640, 0.994408, 0.995127, 0.995797, 0.996418,
    0.996989, 0.997511, 0.997984, 0.998406, 0.998780, 0.999103, 0.999377, 0.999601, 0.999776,
    0.999900, 0.999975, 1.000000,
};
static struct vfo cw_tone;     // keyed-tone oscillator, running at cw_pitch_hz
static int cw_pitch_hz = CW_PITCH_HZ; // live pitch - see cw_set_pitch()
static int envelope_pos = 0;   // 0 = silent, CW_ENVELOPE_LEN-1 = full output
static int key_down = 0;       // keyer output at the sample being generated
static _Atomic int tx_active = 0; // PTT/relay asserted by this file
static int hang_counter = 0;   // polls remaining before TX releases
static int text_in_burst = 0;  // the keyer sent text during this transmission
static pthread_mutex_t tx_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int keying_enabled = 1; // cleared by cw_audio_stopped()
static _Atomic int tx_hold = 0;        // cw_hold_tx()

// The block being generated: keyer.c's key value for each sample, which
// cw_get_sample() follows.
static uint8_t key_block[CW_MAX_BLOCK];
static int key_block_len = 0;
static int sample_index = 0;   // cw_get_sample() calls since cw_poll_key()

static unsigned contacts_closed = 0; // bit per enum key_contact, at the end of the last block
static double boundary_ns = 0; // estimated time the last captured block ended
static double period_ns = 0;   // estimated block length, CLOCK_MONOTONIC ns
static int64_t last_capture_ns = 0;

// Samples each fall is held off after key-up, for 1:1 weighting (above).
static int weighting_hold = 0;
static int hold_remaining = 0; // samples of the current hold still to run

// Where the table crosses 0.5, as a fractional index.
static double envelope_half_index(void) {
    for (int i = 0; i < CW_ENVELOPE_LEN - 1; i++)
        if (cw_envelope[i] < 0.5 && cw_envelope[i + 1] >= 0.5)
            return i + (0.5 - cw_envelope[i]) / (cw_envelope[i + 1] - cw_envelope[i]);
    return (CW_ENVELOPE_LEN - 1) / 2.0; // unreachable for a 0-to-1 table
}

void cw_init(void) {
    cw_pitch_hz = CW_PITCH_HZ;
    vfo_start(&cw_tone, cw_pitch_hz, 0);
    envelope_pos = 0;
    key_down = 0;
    atomic_store(&tx_active, 0);
    atomic_store(&keying_enabled, 1);
    atomic_store(&tx_hold, 0);
    hang_counter = 0;
    key_block_len = sample_index = 0;
    contacts_closed = 0;
    keyer_reset();
    boundary_ns = 0;
    period_ns = 0;
    last_capture_ns = 0;

    // The rise reaches 50% at half_index samples after key-down and the fall,
    // reading the same table backwards, at (N - 1 - half_index) after it
    // starts. Delaying the fall by the difference makes the two equal.
    double half = envelope_half_index();
    weighting_hold = (int)lround(2.0 * half - (CW_ENVELOPE_LEN - 1));
    if (weighting_hold < 0)
        weighting_hold = 0; // a table that falls early would need a shorter mark instead
    hold_remaining = 0;
    printf("cw: weighting correction %d samples (%.2f ms) - each fall starts that long "
           "after key-up, for 1:1 at the envelope's 50%% points\n",
           weighting_hold, weighting_hold * 1000.0 / 96000.0);
}

int cw_weighting_hold_samples(void) {
    return weighting_hold;
}

void cw_set_pitch(int hz) {
    cw_pitch_hz = hz;
    vfo_start(&cw_tone, cw_pitch_hz, 0);
}

int cw_get_pitch(void) {
    return cw_pitch_hz;
}

// Requests TX if this file hasn't. Two threads start transmissions: the
// input thread on a closure (cw_key_closed()), up to a block before the
// audio thread sees the edge, and the audio thread if it gets there first.
// tx_lock orders both against the audio thread's release. The input thread
// waits for it; the audio thread only tries it, since the only other holder
// is the input thread making this same request.
static void tx_start(int from_audio_thread) {
    if (from_audio_thread ? pthread_mutex_trylock(&tx_lock) != 0
                          : pthread_mutex_lock(&tx_lock) != 0)
        return;
    // With the audio thread gone nothing would ever release it.
    if (!atomic_load(&keying_enabled)) {
        pthread_mutex_unlock(&tx_lock);
        return;
    }
    // A refused TX (dial outside the calibrated bands) leaves tx_active
    // clear, so the audio thread retries on its next poll. maxibitx.c's idle
    // loop does the reporting - nothing on the audio thread may do I/O.
    if (!atomic_load(&tx_active) && radio_set_tx(1) == 0)
        atomic_store(&tx_active, 1);
    pthread_mutex_unlock(&tx_lock);
}

// Audio thread: releases TX unless an edge is still on its way. Under
// tx_lock, so a closure queued after the check finds TX released and
// requests it again, rather than being released by a check that missed it.
static void tx_stop_if_idle(void) {
    if (pthread_mutex_trylock(&tx_lock) != 0)
        return; // the input thread is starting TX - not idle
    if (atomic_load(&tx_active) && !key_input_pending()) {
        radio_set_tx(0);
        atomic_store(&tx_active, 0);
    }
    pthread_mutex_unlock(&tx_lock);
}

void cw_audio_stopped(void) {
    pthread_mutex_lock(&tx_lock);
    atomic_store(&keying_enabled, 0);
    if (atomic_load(&tx_active)) {
        radio_set_tx(0);
        atomic_store(&tx_active, 0);
    }
    pthread_mutex_unlock(&tx_lock);
}

void cw_key_closed(void) {
    enum radio_mode mode = radio_get_mode();
    if (mode == RADIO_MODE_CW || mode == RADIO_MODE_CWR)
        tx_start(0);
}

void cw_text_queued(void) {
    cw_key_closed();
}

void cw_hold_tx(int hold) {
    atomic_store(&tx_hold, hold != 0);
    if (hold)
        cw_key_closed();
}

// The hang time in polls of n samples: CW_HANG_POLLS, or a word space at
// the keyer's speed if that is longer and the keyer is timing the gaps - a
// paddle mode, or text - so TX doesn't drop between characters or words.
static int hang_polls(int n) {
    if (keyer_get_mode() == KEYER_STRAIGHT && !text_in_burst)
        return CW_HANG_POLLS;
    int word = 7 * ((115200 + keyer_get_wpm() / 2) / keyer_get_wpm());
    int polls = (word + n - 1) / n;
    return polls > CW_HANG_POLLS ? polls : CW_HANG_POLLS;
}

void cw_poll_key(int64_t capture_ns, int n) {
    // This block replays the interval between the last block boundary and
    // this one. A capture read returns when its period has ended or later -
    // up to a period later when the audio thread is catching up - never
    // sooner, so the earliest reads mark the boundaries best: each boundary
    // is estimated as one period after the last, or this read's time if
    // that is earlier. With no usable last one - the first block, or after a
    // capture overrun - this read's time is taken as the boundary.
    double nominal_ns = n * 1e9 / CW_SAMPLE_RATE;
    if (period_ns == 0 || n != (int)lround(period_ns * CW_SAMPLE_RATE / 1e9))
        period_ns = nominal_ns;
    double start_ns = boundary_ns;
    double end_ns = start_ns + period_ns + CW_BOUNDARY_SLEW_NS;
    int64_t read_gap_ns = capture_ns - last_capture_ns;
    last_capture_ns = capture_ns;
    if (start_ns == 0 || capture_ns < start_ns || capture_ns - start_ns > 3 * period_ns) {
        end_ns = capture_ns;
        start_ns = end_ns - period_ns;
    } else {
        if (capture_ns < end_ns)
            end_ns = capture_ns;
        // Follow the codec's clock, within 1000 ppm of nominal.
        period_ns += (read_gap_ns - period_ns) / CW_PERIOD_AVERAGE_BLOCKS;
        if (period_ns > nominal_ns * 1.001)
            period_ns = nominal_ns * 1.001;
        if (period_ns < nominal_ns * 0.999)
            period_ns = nominal_ns * 0.999;
    }
    boundary_ns = end_ns;

    struct key_event ev[CW_MAX_EVENTS];
    int n_ev = key_input_take((int64_t)start_ns, (int64_t)end_ns, n, ev, CW_MAX_EVENTS);

    // The keyer runs in every mode, so its paddle state stays true to the
    // jack; only CW and CWR use what it keys.
    for (int i = 0; i < n_ev; i++) {
        unsigned bit = 1u << ev[i].contact;
        contacts_closed = ev[i].closed ? (contacts_closed | bit) : (contacts_closed & ~bit);
    }
    if (n > CW_MAX_BLOCK)
        n = CW_MAX_BLOCK;
    int any_down = keyer_run_block(ev, n_ev, key_block, n);
    key_block_len = n;
    sample_index = 0;

    enum radio_mode mode = radio_get_mode();
    if (mode == RADIO_MODE_CW || mode == RADIO_MODE_CWR) {
        // Semi break-in: the hang time (hang_polls()) holds TX through the
        // gaps the keyer leaves - between a straight key's elements, or once
        // a paddle keyer's element and its space are done. Queued text keeps
        // TX wanted even before the keyer starts it. CWR keys identically; it
        // differs only in which side of the BFO rx_audio.c demodulates.
        int text = keyer_text_busy();
        if (text)
            text_in_burst = 1;
        if (any_down || text || atomic_load(&tx_hold)) {
            tx_start(1);
            hang_counter = hang_polls(n);
        } else if (atomic_load(&tx_active)) {
            if (hang_counter > 0)
                hang_counter--;
            else
                tx_stop_if_idle();
        }
        if (!atomic_load(&tx_active))
            text_in_burst = 0;
        return;
    }

    // Outside CW and CWR text has nowhere to go, and nothing holds TX.
    if (keyer_text_busy())
        keyer_stop_text();
    atomic_store(&tx_hold, 0);
    if (mode == RADIO_MODE_USB || mode == RADIO_MODE_LSB) {
        // Mic PTT on the ring: TX follows the switch, no hang timer (as
        // sbitx does on the same GPIO).
        if (contacts_closed & (1u << KEY_RING))
            tx_start(1);
        else if (atomic_load(&tx_active))
            tx_stop_if_idle();
        hang_counter = 0; // no semi break-in outside CW mode
    } else if (atomic_load(&tx_active)) {
        // DIGITAL: the key is ignored; PTT comes from CAT, rigctld or
        // HPSDR. A transmission the key started before the mode changed -
        // within CW's hang time, or a closure the input thread saw just as
        // it changed - is released here rather than left keyed.
        tx_stop_if_idle();
    }
}

int cw_tx_active(void) {
    return atomic_load(&tx_active);
}

double cw_get_sample(void) {
    // The envelope follows the key, except that after key-up it keeps rising
    // or holding for weighting_hold samples before it starts to fall.
    if (sample_index < key_block_len)
        key_down = key_block[sample_index];
    sample_index++;

    int keyed;
    if (key_down) {
        keyed = 1;
        hold_remaining = weighting_hold;
    } else if (hold_remaining > 0) {
        keyed = 1;
        hold_remaining--;
    } else {
        keyed = 0;
    }

    if (keyed) {
        if (envelope_pos < CW_ENVELOPE_LEN - 1) envelope_pos++;
    } else {
        if (envelope_pos > 0) envelope_pos--;
    }

    int tone = vfo_read(&cw_tone);           // Q30 fixed-point sine (vfo.c)
    double tone_f = (double)tone / 1073741824.0;
    return tone_f * cw_envelope[envelope_pos];
}

double cw_envelope_level(void) {
    return cw_envelope[envelope_pos];
}
