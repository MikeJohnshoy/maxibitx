// radio.h

#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>
#include "vfo.h"

extern int freq_hdr;    // current frequency, Hz
extern int in_tx;       // 0 = RX, 1 = TX
extern int xtal_filter_center; // measured true center of the crystal
                                // filter, Hz - see radio.c
extern int bfo_freq;    // real clk1/BFO frequency used only while
                         // transmitting - deliberately NOT
                         // xtal_filter_center; see radio.c
extern struct vfo lo;   // software LO for RX quadrature mixing

#define RX_IF_FREQ_HZ 24000

// Conventional +/-9.999kHz RIT range (matches common Icom/Kenwood-style
// rigs) - a UI/protocol bound only, not a hardware limit. The caller
// (currently just hamlib.c's J command) is responsible for range-
// checking a requested offset against this before calling
// radio_set_rit() - that function does not reject out-of-range values
// itself, same convention as radio_tune_to() trusting its own caller.
#define RIT_MAX_HZ 9999

// Tunes to f (Hz): sets the si5351 oscillator, restarts the software RX
// VFO, and selects the matching LPF. Also clears any RIT offset back to
// 0 - see radio_set_rit() below for why an offset dialed in against the
// old frequency shouldn't silently carry forward onto a new one.
void radio_tune_to(uint32_t f);

// Switches between receive (tx_on = 0) and transmit (tx_on = 1): drives
// EXT_PTT and TX_LINE in the correct order with a relay-settling delay
// between them, and updates in_tx.
void radio_set_tx(int tx_on);

// Sets the receive-only tuning offset, hz, added to freq_hdr when
// computing RX's clk2 - TX's own clk2 (radio_tx_apply(), radio.c) never
// sees it, so dialing in RIT never moves your transmit frequency, only
// where you're listening. Also implicitly enables RIT (see
// radio_set_rit_enabled() below) unless hz is 0, in which case it's
// implicitly disabled too - this matches rigctld's j/J, which has no
// separate on/off concept: 0 Hz IS off. Takes effect immediately if
// currently receiving; if mid-transmission, the stored value is applied
// the moment TX drops back to RX. Cleared to 0 (and disabled) by every
// radio_tune_to() call - a RIT offset has no defined meaning once
// you've retuned to a different frequency or band, so a fresh VFO
// frequency always starts RIT-free rather than silently inheriting
// whatever was dialed in before. See docs/04_remote_control_and_iq_output.md
// for the rigctld j/J commands that reach this, and radio.c's
// radio_set_rit() for why this is RX-only rather than something clk1
// (the physical BFO) needs to know about at all.
void radio_set_rit(int hz);

// Returns the current RIT offset, hz (0 if none has ever been set) -
// the raw dialed-in value, unaffected by radio_rit_enabled()/
// radio_set_rit_enabled() below. This is what rigctld's j reports:
// real Hamlib clients have no "toggle it off but remember the value"
// concept, so they only ever need the number itself.
int radio_get_rit(void);

// Independently enables/disables applying the stored RIT offset to the
// hardware, WITHOUT changing the stored value - the "RIT ON/OFF" button
// found on real rigs (and Kenwood CAT's RT0;/RT1;, usb_gadget.c), as
// opposed to radio_set_rit() above, which is the "spin the RIT knob to
// a value" action and always changes what's stored. rigctld's j/J never
// calls this - it has no reason to, since for that protocol the value
// and the on/off state are the same thing (see radio_set_rit()'s
// comment). Takes effect immediately if currently receiving, same as
// radio_set_rit(). Cleared to disabled by every radio_tune_to() call,
// same as the offset itself.
void radio_set_rit_enabled(int on);

// Returns whether RIT is currently being applied to the hardware (see
// radio_set_rit_enabled() above) - independent of what radio_get_rit()
// reports, which stays the same whether enabled or not.
int radio_rit_enabled(void);

// The real, single-owner mode state - the same "one owner in radio.c,
// every control surface calls into it instead of keeping its own copy"
// pattern as RIT above, applied to mode instead of a tuning offset.
// Before this existed, hamlib.c and usb_gadget.c each kept their own
// independently-cosmetic "current mode" - nothing enforced they agreed
// (hamlib.c defaulted to "USB", usb_gadget.c's Kenwood CAT surface
// defaulted to CW), and neither one meant anything since nothing read
// it. Now there's one real value both surfaces translate to/from their
// own protocol's representation (see hamlib.c's mode_to_name()/
// name_to_mode(), usb_gadget.c's mode_to_kenwood_digit()/
// kenwood_digit_to_mode()).
//
// Still doesn't drive anything downstream as of this enum's
// introduction - see docs/ARCHITECTURE.md's build order: this is step
// 3 ("the state is finally real, and every control surface agrees on
// it"), not steps 4/5 ("the state actually selects a TX i_sample
// source and a sideband-zero branch"). RADIO_MODE_CW/_USB/_LSB are
// minibitx's real future v1 modes; RADIO_MODE_DIGITAL is a placeholder
// for externally-generated digital-mode audio (WSJT-X on a host PC)
// riding the same path SSB will, per ARCHITECTURE.md §5's TX pipeline
// section - not a mode anything can select meaningfully yet either,
// just a real, storable value instead of an absent one.
enum radio_mode {
  RADIO_MODE_CW,
  RADIO_MODE_USB,
  RADIO_MODE_LSB,
  RADIO_MODE_DIGITAL,
};

// Sets the current mode. No validity check here - same convention as
// radio_tune_to()/radio_set_rit(): trusts its caller. That caller
// (hamlib.c's M, usb_gadget.c's MD) is responsible for translating its
// own protocol's mode representation into one of the four values above
// and handling anything that doesn't map to one of them per its own
// protocol's convention (hamlib.c's rigctld surface replies RPRT -1,
// same as an out-of-range RIT value; usb_gadget.c's Kenwood CAT surface
// silently ignores it, same as any other command it doesn't recognize
// - see each file's own comment). Takes effect immediately - there is
// nothing downstream to update yet (see this enum's comment above).
void radio_set_mode(enum radio_mode m);

// Returns the current mode (RADIO_MODE_CW, matching the one mode
// minibitx can actually transmit today, until something calls
// radio_set_mode()).
enum radio_mode radio_get_mode(void);

// Parses one command string from a control surface (currently just
// * "freq NNN" from hpsdr_p1.c) and applies it.
void remote_execute(char *command);

#endif /* RADIO_H */
