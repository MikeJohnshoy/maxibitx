// radio.c

#include "radio.h"
#include "radio_hw.h"
#include "si5351.h"
#include "sound.h"
#include "rx_audio.h" // rx_audio_set_demod() - radio_set_mode() below
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

int freq_hdr = 7030000;
int in_tx = 0;

// xtal_filter_center: the crystal filter's real, measured passband
// center (~40.0124 MHz on this board) - a physical property of the
// hardware, not a clock setting. RX aims its mixing product AT this
// value; bfo_freq (below) deliberately does not. See
// docs/dsp_design_notes/antialias_filter_design.md §3 for why the two
// used to be coupled (and aren't any more). Board-specific; override in
// hw_settings.ini if a different board's filter measures differently.
int xtal_filter_center = 40012400;

// bfo_freq: the real si5351 clk1 frequency used only while transmitting
// (radio_tx_apply(), below). Deliberately NOT xtal_filter_center - the
// TX-modulating waveform (tx_pipeline.c, via sound.c) is placed by its
// own shared IF bin-rotate (docs/ARCHITECTURE.md §10 step 4's
// derivation) at bfo_freq - xtal_filter_center - CW_PITCH_HZ above 0Hz,
// specifically so bfo_freq's single real mixer produces a difference
// product landing almost exactly on xtal_filter_center while the sum
// product lands safely out in the crystal filter's stopband - the same
// "BFO at the filter's edge" placement real sbitx's own design article
// describes. bfo_freq == xtal_filter_center + ~22.6kHz by calibration,
// not enforced in code - changing bfo_freq without re-deriving
// tx_pipeline.c's TX_IF_SHIFT_HZ to match throws away that image
// suppression. RX is unaffected either way (adjust xtal_filter_center
// for that).
int bfo_freq = 40035000;
struct vfo lo;

// rit_offset/rit_enabled: the receive-only tuning offset described in
// radio.h's radio_set_rit()/radio_set_rit_enabled() - the dialed-in
// value and whether it's currently being applied are deliberately
// separate (see radio.h), matching a real rig's RIT knob vs its RIT
// ON/OFF button. Both start at 0/disabled, and both go back to 0/
// disabled on every radio_tune_to() call. Deliberately NOT involved in
// clk1 (bfo_freq) at all: clk1's two jobs (RX centering vs TX
// edge-placement for image rejection, see tx_pipeline.h's
// TX_IF_SHIFT_HZ comment) are both fixed offsets from the crystal
// filter's measured center, unrelated to the tuned dial frequency -
// RIT only ever adjusts the dial-frequency term that clk2 carries, and
// only for RX.
static int rit_offset = 0;
static int rit_enabled = 0;

// What RIT is actually contributing to RX's clk2 right now - 0 whenever
// it's disabled, regardless of what value is still remembered.
static int rit_applied_hz(void) {
  return rit_enabled ? rit_offset : 0;
}

// "Master"'s RIGHT channel (sound_set_tx_drive(), sound.c) is what
// actually feeds the exciter/PA during TX - not a volume knob in the
// usual sense, even though it's set via the same volume-percent
// mechanism (see docs/03_tx_processing_pipeline.md's TX_MASTER_VOL
// bullet for the sbitx-matching derivation of 95). LEFT is a completely
// separate channel (the local speaker/headphone output) and is
// unaffected by this.
#define TX_MASTER_VOL 95

void radio_tune_to(uint32_t f) {
  freq_hdr = f;
  // A RIT offset dialed in against the OLD frequency has no defined
  // meaning on a new one (different band, different QSO) - clear it
  // rather than silently carrying it forward. See radio_set_rit()'s
  // comment (radio.h) - this is a deliberate simplicity choice, not an
  // oversight; a rig that instead preserves RIT across a retune is an
  // equally valid design, just not this one.
  rit_offset = 0;
  rit_enabled = 0;
  // clk2 places f at the crystal filter's real center; clk1 is left
  // untouched here (it's set at startup and only ever retuned by
  // radio_tx_apply() below) - this call is also used for plain RX
  // retuning with no TX transition involved, so it must not carry
  // either TX-only clock value.
  si5351bx_setfreq(2, f + xtal_filter_center);
  vfo_start(&lo, RX_IF_FREQ_HZ, lo.phase);
  set_lpf_40mhz(f); // enable the correct LPF for this band
}

void radio_set_rit(int hz) {
  rit_offset = hz;
  // Implicitly enables/disables to match - see this function's comment
  // in radio.h for why rigctld's j/J (the only caller that doesn't also
  // have radio_set_rit_enabled() available) needs this: it has no
  // separate on/off concept, so "the value" and "on" are the same thing
  // there. A caller that DOES want to change the value without touching
  // enabled state has no way to ask for that through this function - use
  // radio_set_rit_enabled() separately for that (Kenwood CAT's RU/RD do
  // both together deliberately, see usb_gadget.c).
  rit_enabled = (hz != 0);
  if (!in_tx) {
    si5351bx_setfreq(2, freq_hdr + rit_applied_hz() + xtal_filter_center);
  }
  // else: stored only for now - radio_tx_apply()'s tx_on==0 branch below
  // applies it the moment RX resumes. TX's own clk2 line (also below)
  // never adds it in the first place, so there's nothing to undo there.
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

// current_mode: the real, single-owner mode state described in
// radio.h's enum radio_mode comment. Defaults to CW - the one mode
// minibitx can actually transmit today - rather than either control
// surface's old, independently-chosen cosmetic default (hamlib.c's was
// "USB", usb_gadget.c's Kenwood CAT was CW already; this makes both
// agree on the one that was actually honest).
static enum radio_mode current_mode = RADIO_MODE_CW;

void radio_set_mode(enum radio_mode m) {
  current_mode = m;
  // Keep rx_audio.c's demodulator in step - every mode change (rigctld M,
  // Kenwood MD, the control panel) already funnels through here. DIGITAL
  // demodulates as USB, the universal FT8/digital-mode convention (same
  // choice sound.c's TX branch makes with TX_PIPELINE_KEEP_UPPER).
  switch (m) {
  case RADIO_MODE_USB:
  case RADIO_MODE_DIGITAL: rx_audio_set_demod(RX_DEMOD_USB); break;
  case RADIO_MODE_LSB:     rx_audio_set_demod(RX_DEMOD_LSB); break;
  case RADIO_MODE_CW:
  default:                 rx_audio_set_demod(RX_DEMOD_CW);  break;
  }
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
    // clk1 -> bfo_freq, clk2 -> freq_hdr + xtal_filter_center - the same
    // plain formula radio_tune_to() uses for RX, no correction term.
    //
    // Before docs/ARCHITECTURE.md build order step 5, cw.c generated its
    // TX-modulating tone at a fixed, bench-derived IF offset
    // (TX_IF_OFFSET_HZ) chosen to land the wanted mixing product close
    // to, but CW_PITCH_HZ short of, xtal_filter_center - a small,
    // deliberate residual baked into that one fixed NCO frequency (see
    // cw.c's git history, or docs/03_tx_processing_pipeline.md's "Known
    // limitations" for the old derivation), which this function used to
    // cancel by subtracting that same CW_PITCH_HZ from clk2 instead.
    // Step 5 replaced cw.c's IF-shifted NCO with tx_pipeline.c's shared
    // FFT bin-rotate (sound.c), which is aimed directly at
    // xtal_filter_center in the first place (docs/ARCHITECTURE.md §10
    // step 4's derivation: shift_hz = bfo_freq - xtal_filter_center -
    // CW_PITCH_HZ, landing the product on-center to within a
    // bin-quantization residual of ~9Hz, not ~700Hz) - so this function
    // no longer needs a compensating correction of its own. This is the
    // one place all TX (straight key via cw.c, remote MOX via
    // hpsdr_p1.c) funnels through, rather than radio_tune_to() itself,
    // which is also used for plain RX retuning. Capture is muted first -
    // before PTT/the relay/either clock, i.e. before any TX RF exists at
    // all - see sound_set_rx_capture()'s comment (sound.c) for why, and
    // the tx_off branch below for the matching restore.
    sound_set_rx_capture(0);
    si5351bx_setfreq(1, bfo_freq);
    si5351bx_setfreq(2, freq_hdr + xtal_filter_center);
    radio_hw_set_ptt(1);
    usleep(20000); // let PTT assert before keying the relay
    radio_hw_set_tx_relay(1);
    // Only the RIGHT channel of "Master" feeds the exciter - the LEFT
    // channel (local speaker/headphone) is independent and is never
    // touched here, see sound_set_tx_drive()'s comment (sound.c).
    sound_set_tx_drive(TX_MASTER_VOL);
  } else {
    sound_set_tx_drive(0); // mute the exciter feed before dropping the relay
    radio_hw_set_ptt(0);
    usleep(5000); // let the relay settle before dropping PTT
    radio_hw_set_tx_relay(0);
    // Restore clk1/clk2 to their RX values - matters most for the
    // straight-key path, which (unlike hpsdr_p1.c's MOX path) has no
    // separate radio_tune_to() call of its own to undo this. clk2 adds
    // back rit_applied_hz() here (0 if RIT was never set, cleared by a
    // retune since, or currently disabled via radio_set_rit_enabled()) -
    // RIT persists across your own TX bursts, only radio_tune_to() ever
    // resets it, so a burst sent while RIT was dialed in and enabled
    // must not come back to RX having silently lost it.
    si5351bx_setfreq(1, xtal_filter_center + RX_IF_FREQ_HZ);
    si5351bx_setfreq(2, freq_hdr + rit_applied_hz() + xtal_filter_center);
    // Restore Capture only now that the relay has actually settled - any
    // earlier would feed the DSP chain raw relay-transient noise. There
    // is no equivalent local-monitor restore needed here: unlike the
    // old shared-"Master" design, the local speaker/headphone (LEFT
    // channel) was never touched by this function in the first place -
    // see sound_set_local_monitor()'s comment (sound.c).
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

// switch between RX and TX
void radio_set_tx(int tx_on) {
  pthread_once(&tx_worker_once, radio_tx_worker_start);

  in_tx = tx_on ? 1 : 0; // updated synchronously so other threads'
                         // guards (cw_tx_active(), network MOX logic)
                         // see it right away - see
                         // docs/05_process_and_threading_model.md

  pthread_mutex_lock(&tx_mutex);
  tx_pending_on = tx_on;
  tx_pending = 1;
  pthread_cond_signal(&tx_cond);
  pthread_mutex_unlock(&tx_mutex);
}
