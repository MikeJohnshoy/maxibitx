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
// transmit was refused because the dial is outside every calibrated
// [tx_band] range (hw_settings_tx_allowed()). Doing the check here
// covers every PTT source at once - the key, rigctld's T, Kenwood TX;
// and HPSDR MOX. Refusal leaves the radio in receive.
//
// tx_on = 0 is never refused: stopping transmitting must always work.
int radio_set_tx(int tx_on);

// The dial frequency of the most recent refusal, or 0 if there hasn't
// been one since the last call. Reading it clears it.
//
// Refusals are reported this way rather than logged where they happen
// because cw_poll_key() calls radio_set_tx() from the real-time audio
// thread, which may not do I/O. maxibitx.c's idle loop drains this.
int radio_tx_refused_hz(void);

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
// USB audio gadget - transmitted and received as USB.
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
void radio_set_mode(enum radio_mode m);

// The current mode; RADIO_MODE_CW until something sets it.
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

// Parses and applies one command string from a control surface (currently
// just "freq NNN" from hpsdr_p1.c).
void remote_execute(char *command);

#endif /* RADIO_H */
