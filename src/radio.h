// radio.h
//
// Radio state with one owner: frequency, TX/RX, RIT and mode. Every
// control surface (hamlib.c, usb_gadget.c, hpsdr_p1.c, cw.c) calls in
// here instead of keeping its own copy.

#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>
#include "vfo.h"

extern int freq_hdr;    // current frequency, Hz
extern int in_tx;       // 0 = RX, 1 = TX
extern int xtal_filter_center; // measured center of the crystal
                                // filter, Hz - see radio.c
extern int bfo_freq;    // clk1 frequency while transmitting - not
                         // xtal_filter_center; see radio.c
extern struct vfo lo;   // software LO for RX quadrature mixing

#define RX_IF_FREQ_HZ 24000

// RIT range, +/-9.999kHz as on common Icom/Kenwood rigs - a protocol
// bound, not a hardware limit. Callers range-check against it;
// radio_set_rit() doesn't.
#define RIT_MAX_HZ 9999

// Tunes to f (Hz): sets the si5351, restarts the software RX LO, selects
// the band's LPF, and clears RIT (an offset dialed in on the old
// frequency means nothing on the new one).
void radio_tune_to(uint32_t f);

// Switches RX (tx_on = 0) / TX (1). Updates in_tx at once; a worker thread
// (radio.c) then sequences the capture mute, clocks, PTT and relay with
// their settling delays. Never blocks, so it's safe to call from the
// real-time audio thread.
//
// Returns 0 when the state was accepted and -1 when a request to
// transmit was refused: this board may not transmit
// (radio_hw_tx_permitted()), bfo_freq and xtal_filter_center give no
// usable TX IF (sound_tx_if_placed()), or the dial is outside every
// calibrated [tx_band] range (hw_settings_tx_allowed()). Doing the check
// here covers every PTT source at once - the key, rigctld's T, Kenwood TX,
// HPSDR MOX and TCI. Refusal leaves the radio in receive.
//
// tx_on = 0 is never refused: stopping transmitting must always work.
int radio_set_tx(int tx_on);

// 1 if radio_set_tx(1) would be accepted with the dial at freq_hz: the
// same three checks, for an interface that reports whether TX is possible.
int radio_tx_allowed(int freq_hz);

enum radio_tx_refusal {
  RADIO_TX_NOT_REFUSED = 0,
  RADIO_TX_REFUSED_BOARD, // this board may not transmit
  RADIO_TX_REFUSED_IF,    // bfo_freq and xtal_filter_center give no
                          // usable TX IF (sound_tx_if_placed())
  RADIO_TX_REFUSED_BAND,  // outside every calibrated [tx_band]
};

// Why the most recent transmit request was refused, with the dial
// frequency then in *freq_hz, or RADIO_TX_NOT_REFUSED if none has been
// since the last call. Reading it clears it.
//
// Refusals are reported this way rather than logged where they happen
// because cw_poll_key() calls radio_set_tx() from the real-time audio
// thread, which may not do I/O. maxibitx.c's idle loop drains this.
enum radio_tx_refusal radio_tx_refused(int *freq_hz);

// Sets the RX-only tuning offset (Hz). It's added to RX's clk2 only, so it
// never moves the transmit frequency. Also sets the enabled state: on for
// a nonzero value, off for 0 (rigctld's j/J has no separate on/off - 0 Hz
// is off). Applied immediately in RX; if called during TX, applied when RX
// resumes. Reset to 0/off by radio_tune_to(). Reached via rigctld j/J
// (docs/04_remote_control_and_iq_output.md).
void radio_set_rit(int hz);

// The stored RIT offset (Hz), whether or not it's currently enabled - what
// rigctld's j reports.
int radio_get_rit(void);

// RIT ON/OFF without changing the stored offset - a rig's RIT button, as
// opposed to radio_set_rit()'s knob. Used by Kenwood RT0;/RT1;
// (usb_gadget.c). Applied immediately in RX. Reset to off by
// radio_tune_to().
void radio_set_rit_enabled(int on);

// Whether RIT is currently applied (see radio_set_rit_enabled()).
int radio_rit_enabled(void);

// Operating mode. hamlib.c and usb_gadget.c translate to and from their
// own protocol's names/digits (mode_to_name()/name_to_mode(),
// mode_to_kenwood_digit()/kenwood_digit_to_mode()). The mode selects TX's
// audio source and sideband (sound.c) and RX's demodulator (rx_audio.c,
// via radio_set_mode()). DIGITAL is external audio - WSJT-X etc. over the
// USB audio gadget - transmitted and received as USB. It is also the one
// mode that holds rx_audio.c's narrow CW filter out of circuit, because the
// audio it exists to serve is taken downstream of that filter - see
// radio_set_mode() below.
//
// CWR is CW-reverse: the same key, hang timer and transmitted carrier as
// CW, receiving the other side of the BFO. Transmit is identical because
// a key-down carrier lands on the dial whichever side is kept; the mode
// exists to move away from an interfering signal on the side CW hears.
enum radio_mode {
  RADIO_MODE_CW,
  RADIO_MODE_CWR,
  RADIO_MODE_USB,
  RADIO_MODE_LSB,
  RADIO_MODE_DIGITAL,
};

// Sets the mode and updates rx_audio.c's demodulator to match. No validity
// check: callers translate their protocol's value and handle unknown ones
// themselves (hamlib.c replies RPRT -1; usb_gadget.c ignores it).
//
// Also the one place that decides whether stage 3, the narrow CW filter, is
// allowed in circuit: DIGITAL holds it out and every other mode releases it
// (rx_audio_inhibit_narrow_filter()). The operator's own on/off setting is
// untouched by this and comes back when the mode does. Every mode change -
// rigctld M, Kenwood MD, the control panel - already funnels through here,
// which is what makes one call enough.
void radio_set_mode(enum radio_mode m);

// The mode maxibitx starts in, applied by maxibitx.c with radio_set_mode()
// once rx_audio.c is up. DIGITAL, since most clients are remote SDR and
// digital-mode programs: USB demodulation, the narrow CW filter held out,
// and PTT from the computer only (the key jack is ignored until a client
// selects CW).
#define RADIO_STARTUP_MODE RADIO_MODE_DIGITAL

// The current mode.
enum radio_mode radio_get_mode(void);

// Moves the CW pitch, everywhere it has to move, and returns the pitch
// actually selected - the request snapped to the pitches rx_audio.c's
// filter bank carries, so a caller asking for 725 gets 700 back and should
// display that.
//
// Four things have to agree and this is the only function that makes them:
// rx_audio.c's BFO (the tone you hear), its narrow filter (centered on that
// tone), cw.c's keyed tone (the sidetone), and tx_pipeline.c's CW IF shift
// (derived from the tone so a key-down still lands on the dial). Setting a
// subset is what produces the failure this exists to prevent: with the
// sidetone at one pitch and the receiver at another, zero-beating by ear
// puts the transmission off frequency by the difference - the operator
// tunes for the tone they hear in the sidetone, which is no longer the tone
// that means "on my dial".
//
// The on-air frequency does not change: the tone and the shift cancel, so
// the carrier stays on the dial at every pitch (tx_pipeline_test.c Case H).
// Only the pitch you hear moves.
//
// Refused while transmitting, returning the current pitch unchanged - the
// updates aren't atomic and a key-down straddling them would be briefly off
// frequency. Not for the audio thread: it re-tunes the FFT filter, which
// allocates.
int radio_set_cw_pitch(int hz);

// The current CW pitch in Hz - one value for the sidetone, the RX BFO and
// the narrow filter, since radio_set_cw_pitch() keeps them equal.
int radio_get_cw_pitch(void);

// Bounds radio_set_xtal_filter_center() accepts: a guard against a typo
// (a dropped digit would retune both clocks megahertz away), not the
// filter's limits. Both boards' 40MHz filters sit well inside.
#define RADIO_XTAL_CENTER_MIN_HZ 39900000
#define RADIO_XTAL_CENTER_MAX_HZ 40100000

// Moves xtal_filter_center at run time, for measuring the crystal filter
// (tools/xtal_sweep.py) - the ini's value is what a restart uses. Reached
// via rigctld's L XTALCENTER (docs/06_api.md).
//
// In receive, retunes both clocks that depend on it: clk1 to the new
// center + RX_IF_FREQ_HZ and clk2 to the dial (+ RIT) + the new center, so
// the dial stays at IF RX_IF_FREQ_HZ. Then re-derives the TX IF placement
// (sound_update_cw_if_placement()), because the TX shifts are
// bfo_freq - xtal_filter_center. A center that leaves no usable TX IF is
// still accepted - a sweep has to cross such values - and transmit is
// refused until a usable one is set again (RADIO_TX_REFUSED_IF).
//
// Returns 0, or -1 with nothing changed while transmitting (the TX clocks
// and shift would disagree for the rest of the transmission) or outside
// RADIO_XTAL_CENTER_MIN_HZ..MAX_HZ.
int radio_set_xtal_filter_center(int hz);

// The xtal_filter_center in force, Hz - the ini's value until
// radio_set_xtal_filter_center() moves it. What rigctld's l XTALCENTER
// reports.
int radio_get_xtal_filter_center(void);

// Parses and applies one command string from a control surface (currently
// just "freq NNN" from hpsdr_p1.c).
void remote_execute(char *command);

#endif /* RADIO_H */
