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
void radio_set_tx(int tx_on);

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
enum radio_mode {
  RADIO_MODE_CW,
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

// Parses and applies one command string from a control surface (currently
// just "freq NNN" from hpsdr_p1.c).
void remote_execute(char *command);

#endif /* RADIO_H */
