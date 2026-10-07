// knobs.h
//
// What the front-panel knobs do, on a board that has them (encoder.c reads
// them; radio_hw_knob() says where they are).
//
// Tuning knob: each click moves the dial by the tuning step, which rigctld
// reads and sets with n and N (tools/rigctl_panel.py sets it to the digit
// tapped). Below 1 kHz, a fast turn multiplies the step by 2, 5 or 10.
// A push moves the step on through 10 Hz, 100 Hz, 1 kHz and 10 kHz, and
// back to 10 Hz. The knob does nothing to the dial while transmitting.
//
// Volume knob: each click is KNOBS_VOLUME_PER_CLICK percent (1 dB). A push
// mutes or unmutes the speaker; turning it unmutes.

#ifndef KNOBS_H
#define KNOBS_H

#include "radio_hw.h" // enum radio_knob
#include <stdint.h>

#define KNOBS_STEP_DEFAULT_HZ 10
#define KNOBS_STEP_MAX_HZ 10000000 // the 10 MHz digit
#define KNOBS_VOLUME_PER_CLICK 2
// The dial's range for the tuning knob, as tools/rigctl_panel.py tunes.
#define KNOBS_FREQ_MIN_HZ 100000
#define KNOBS_FREQ_MAX_HZ 30000000

// Starts encoder.c on the board's knobs with the handlers below. Returns
// the number of knobs, 0 if the board has none the Pi reads, or -1 if
// they can't be read (already logged).
int knobs_start(void);

// The tuning step, Hz: 1 to KNOBS_STEP_MAX_HZ. set clamps and returns what
// it set. Safe from any thread.
int knobs_set_step(int hz);
int knobs_get_step(void);

// The handlers encoder.c calls, from its thread.
void knobs_turn(enum radio_knob k, int detents, int64_t ts_ns);
void knobs_push(enum radio_knob k);

#endif /* KNOBS_H */
