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
// Keying envelope: DL1YCF's ramp (piHPSDR's transmitter.c), 9 ms at every
// speed, from exactly 0 to 1.0, computed into a table at cw_init() and read
// forward for attack and backward for decay. Its five sine terms are
// optimised so a string of dots at 40 WPM meets ARRL's Clean Signal
// Initiative mask; test-cw-shape checks it through tx_pipeline.c, and
// docs/dsp_design_notes/tx_cw_keying_spectrum.md has the numbers.
//
// Weighting: the ramp crosses 50% at its midpoint whichever way it is read,
// so a mark between the 50% points is as long as the key was down - if it
// is at least CW_RAMP_SAMPLES; a shorter one never reaches full output and
// comes out shorter. weighting_hold holds each fall off by however late a
// table crosses 50%, 0 for this one (cw_keyer_design_study.md §4, §17).

#include "cw.h"
#include "key_input.h"
#include "keyer.h"
#include "radio.h"
#include "vfo.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#define CW_RAMP_SAMPLES 864 // 9 ms at 96 kHz
#define CW_ENVELOPE_LEN (CW_RAMP_SAMPLES + 1)
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

// DL1YCF's coefficients, for sin(2 pi k y), k = 1 to 5, y running 0 to 1
// along the ramp.
static const double cw_ramp_coeff[] = {
    -0.12182865361171612, -0.018557469249199286, -0.0009378783245428506,
    0.0008567571519403228, 0.00018706912431472442,
};

static double cw_envelope[CW_ENVELOPE_LEN];
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

static void make_envelope(void) {
    for (int i = 0; i < CW_ENVELOPE_LEN; i++) {
        double y = (double)i / CW_RAMP_SAMPLES;
        double v = y;
        for (int k = 0; k < (int)(sizeof cw_ramp_coeff / sizeof cw_ramp_coeff[0]); k++)
            v += cw_ramp_coeff[k] * sin(2.0 * M_PI * (k + 1) * y);
        cw_envelope[i] = v;
    }
    cw_envelope[0] = 0.0;
    cw_envelope[CW_RAMP_SAMPLES] = 1.0;
}

// Where the table crosses 0.5, as a fractional index.
static double envelope_half_index(void) {
    for (int i = 0; i < CW_ENVELOPE_LEN - 1; i++)
        if (cw_envelope[i] < 0.5 && cw_envelope[i + 1] >= 0.5)
            return i + (0.5 - cw_envelope[i]) / (cw_envelope[i + 1] - cw_envelope[i]);
    return (CW_ENVELOPE_LEN - 1) / 2.0; // unreachable for a 0-to-1 table
}

void cw_init(void) {
    make_envelope();
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
    if (weighting_hold)
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
