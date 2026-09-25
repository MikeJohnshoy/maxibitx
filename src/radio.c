// radio.c
// The single owner of radio state (radio.h), and the TX/RX transition
// sequence, run on its own worker thread.

#include "radio.h"
#include "radio_hw.h"
#include "si5351.h"
#include "sound.h"
#include "cw.h"       // cw_set_pitch() - radio_set_cw_pitch() below
#include "rx_audio.h" // rx_audio_set_demod() - radio_set_mode() below
#include "hw_settings.h" // hw_settings_tx_allowed() - radio_set_tx() below
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

int freq_hdr = 7030000;
int in_tx = 0;

// Measured center of this board's crystal filter (~40.0124MHz) - a
// property of the hardware, not a clock setting. RX places its mixing
// product here. Why it's separate from bfo_freq:
// antialias_filter_design.md §3. Overridable in hw_settings.ini.
int xtal_filter_center = 40012400;

// clk1 while transmitting - deliberately not xtal_filter_center.
// tx_pipeline.c places a CW waveform at
// bfo_freq - xtal_filter_center - (the keyed tone), so bfo_freq's mixer
// lands the wanted difference product on xtal_filter_center and the sum
// product in the crystal filter's stopband ("BFO at the filter's edge", as
// in sbitx). ~xtal_filter_center + 22.6kHz by calibration, not enforced in
// code: changing it means re-deriving tx_pipeline.h's IF shifts, which
// sound_update_cw_if_placement() does from these two values and the live
// pitch. RX doesn't use it. Overridable in hw_settings.ini.
int bfo_freq = 40035000;
struct vfo lo;

// RIT: the dialed value and whether it's applied are separate, like a
// rig's RIT knob and ON/OFF button (radio.h). Only ever affects RX's clk2;
// clk1's jobs are fixed offsets from the crystal filter center. Both reset
// by radio_tune_to().
static int rit_offset = 0;
static int rit_enabled = 0;

// What RIT is actually contributing to RX's clk2 right now - 0 whenever
// it's disabled, regardless of what value is still remembered.
static int rit_applied_hz(void) {
  return rit_enabled ? rit_offset : 0;
}

// 'Master' RIGHT channel level during TX - the exciter feed
// (sound_set_tx_drive()); LEFT is the local speaker and is never touched
// here. Why 95: docs/03_tx_processing_pipeline.md, TX_MASTER_VOL.
#define TX_MASTER_VOL 95

void radio_tune_to(uint32_t f) {
  freq_hdr = f;
  // Clear RIT: an offset from the old frequency means nothing here.
  // (Keeping it across a retune would be equally valid; this rig doesn't.)
  rit_offset = 0;
  rit_enabled = 0;
  // clk2 puts f at the crystal filter center. clk1 isn't touched: it's set
  // at startup and changed only by radio_tx_apply().
  si5351bx_setfreq(2, f + xtal_filter_center);
  vfo_start(&lo, RX_IF_FREQ_HZ, lo.phase);
  set_lpf_40mhz(f); // enable the correct LPF for this band
}

void radio_set_rit(int hz) {
  rit_offset = hz;
  // Setting a value implies on (0 = off) - see radio.h. Use
  // radio_set_rit_enabled() to toggle without changing the value.
  rit_enabled = (hz != 0);
  if (!in_tx) {
    si5351bx_setfreq(2, freq_hdr + rit_applied_hz() + xtal_filter_center);
  }
  // In TX: stored only; radio_tx_apply() applies it when RX resumes. TX's
  // own clk2 never includes it.
}

int radio_get_rit(void) {
  return rit_offset;
}

void radio_set_rit_enabled(int on) {
  rit_enabled = on ? 1 : 0;
  if (!in_tx) {
    si5351bx_setfreq(2, freq_hdr + rit_applied_hz() + xtal_filter_center);
  }
}

int radio_rit_enabled(void) {
  return rit_enabled;
}

// Starts in CW (see radio.h).
static enum radio_mode current_mode = RADIO_MODE_CW;

void radio_set_mode(enum radio_mode m) {
  current_mode = m;
  // Keep rx_audio.c's demodulator in step. DIGITAL receives as USB, the
  // FT8/digital convention (sound.c transmits it as USB too). CWR is the
  // only mode whose demodulator differs from its transmit behavior - it
  // transmits exactly as CW (radio.h).
  switch (m) {
  case RADIO_MODE_USB:
  case RADIO_MODE_DIGITAL: rx_audio_set_demod(RX_DEMOD_USB); break;
  case RADIO_MODE_LSB:     rx_audio_set_demod(RX_DEMOD_LSB); break;
  case RADIO_MODE_CWR:     rx_audio_set_demod(RX_DEMOD_CWR); break;
  case RADIO_MODE_CW:
  default:                 rx_audio_set_demod(RX_DEMOD_CW);  break;
  }
}

int radio_set_cw_pitch(int hz) {
  // Refused mid-transmission. The updates below aren't atomic against each
  // other, and a key-down straddling them would transmit the old tone with
  // the new shift (or the reverse) - off frequency by the pitch change for
  // as long as that lasts. Waiting for key-up costs nothing; nobody adjusts
  // pitch while sending.
  if (in_tx) {
    printf("radio: CW pitch change to %d Hz refused while transmitting - "
           "still %d Hz\n", hz, radio_get_cw_pitch());
    return radio_get_cw_pitch();
  }

  // RX first, because it owns which pitches exist: it snaps the request to
  // its filter bank and reports what it actually selected, and that value -
  // not the caller's request - is what the TX side has to match. Handing
  // cw.c the unsnapped request is exactly how the sidetone and the received
  // tone would silently disagree again.
  int pitch = rx_audio_set_narrow_pitch(hz);

  // Then the sidetone, then the shift that cancels it. Order matters:
  // sound_update_cw_if_placement() reads cw_get_pitch(), so the tone has to
  // be set first or the shift gets derived from the old one.
  cw_set_pitch(pitch);
  if (sound_update_cw_if_placement() < 0) {
    // The pitch still applies - this only means the board's bfo_freq and
    // xtal_filter_center give no usable IF, a calibration problem that
    // predates the pitch change.
    fprintf(stderr, "radio: CW pitch is %d Hz but the TX IF placement was "
                    "refused - check bfo_freq/xtal_filter_center\n", pitch);
  }
  printf("radio: CW pitch %d Hz - sidetone, RX BFO, narrow filter and TX IF "
         "shift all moved together\n", pitch);
  return pitch;
}

int radio_get_cw_pitch(void) {
  // rx_audio.c's bank selection is the single source of truth, since cw.c's
  // tone is set from it; radio_set_cw_pitch() is what keeps them equal.
  return rx_audio_get_narrow_pitch();
}

enum radio_mode radio_get_mode(void) {
  return current_mode;
}

// TX transitions run on this dedicated worker thread rather than
// whatever thread calls radio_set_tx() - see
// docs/05_process_and_threading_model.md for why (short version: cw.c
// calls radio_set_tx() from the real-time audio thread, which must
// never block on the ~20ms+ PTT/relay/ALSA sequence below).
static pthread_mutex_t tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tx_cond = PTHREAD_COND_INITIALIZER;
static int tx_pending = 0;    // 1 = worker has a state change to apply
static int tx_pending_on = 0; // the state to apply (1 = TX, 0 = RX)
static pthread_once_t tx_worker_once = PTHREAD_ONCE_INIT;

static void radio_tx_apply(int tx_on) {
  if (tx_on) {
    // Mute RX capture first - before PTT, the relay or the clocks, i.e.
    // before any TX RF exists (see sound_set_rx_capture()). Then clk1 ->
    // bfo_freq and clk2 -> freq_hdr + xtal_filter_center, with no correction
    // term: tx_pipeline.c already aims the TX waveform at the filter center
    // (ARCHITECTURE.md §10 step 4). Every TX path - straight key via cw.c,
    // CAT/network MOX - comes through here.
    sound_set_rx_capture(0);
    si5351bx_setfreq(1, bfo_freq);
    si5351bx_setfreq(2, freq_hdr + xtal_filter_center);
    radio_hw_set_ptt(1);
    usleep(20000); // let PTT assert before keying the relay
    radio_hw_set_tx_relay(1);
    // Only 'Master' RIGHT feeds the exciter; LEFT (the local speaker) is
    // never touched here.
    sound_set_tx_drive(TX_MASTER_VOL);
  } else {
    sound_set_tx_drive(0); // mute the exciter feed before dropping the relay
    radio_hw_set_ptt(0);
    usleep(5000); // let the relay settle before dropping PTT
    radio_hw_set_tx_relay(0);
    // Restore the RX clocks - needed for the straight-key path, which has
    // no radio_tune_to() of its own afterwards. RIT survives TX bursts (only
    // radio_tune_to() clears it), so clk2 re-adds it.
    si5351bx_setfreq(1, xtal_filter_center + RX_IF_FREQ_HZ);
    si5351bx_setfreq(2, freq_hdr + rit_applied_hz() + xtal_filter_center);
    // Unmute capture only now that the relay has settled, so relay
    // transients never reach the DSP chain. The local speaker needs no
    // restore - TX never touched it.
    sound_set_rx_capture(1);
  }
}

static void *radio_tx_worker(void *arg) {
  (void)arg;

  for (;;) {
    pthread_mutex_lock(&tx_mutex);
    while (!tx_pending)
      pthread_cond_wait(&tx_cond, &tx_mutex);
    int on = tx_pending_on;
    tx_pending = 0;
    pthread_mutex_unlock(&tx_mutex);

    radio_tx_apply(on);
  }

  return NULL;
}

static void radio_tx_worker_start(void) {
  pthread_t worker;
  pthread_create(&worker, NULL, radio_tx_worker, NULL);
}

// Set by radio_set_tx() when it refuses, drained by
// radio_tx_refused_hz() - see radio.h for why it isn't logged in place.
static volatile int tx_refused_hz = 0;

int radio_tx_refused_hz(void) {
  int hz = tx_refused_hz;
  tx_refused_hz = 0;
  return hz;
}

// switch between RX and TX
int radio_set_tx(int tx_on) {
  // Only transmitting is gated; returning to receive always proceeds.
  if (tx_on && !hw_settings_tx_allowed(freq_hdr)) {
    tx_refused_hz = freq_hdr;
    return -1;
  }

  pthread_once(&tx_worker_once, radio_tx_worker_start);

  in_tx = tx_on ? 1 : 0; // set synchronously so other threads' guards
                         // (cw_tx_active(), network MOX) see it at once -
                         // docs/05_process_and_threading_model.md

  pthread_mutex_lock(&tx_mutex);
  tx_pending_on = tx_on;
  tx_pending = 1;
  pthread_cond_signal(&tx_cond);
  pthread_mutex_unlock(&tx_mutex);
  return 0;
}
