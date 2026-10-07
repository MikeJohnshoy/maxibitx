// encoder.h
//
// The front-panel knobs: each a rotary encoder with a push switch, on the
// pins radio_hw_knob() gives for the board. All their lines are one GPIO
// request with edge events, read by a thread of its own that sleeps until
// a knob moves. Turns are decoded from the encoder's two contacts and
// reported in whole clicks (detents); presses are debounced and reported
// as they happen. What a turn or a press does is knobs.c's: this file
// knows nothing of frequency or volume.

#ifndef ENCODER_H
#define ENCODER_H

#include "radio_hw.h" // enum radio_knob, struct radio_hw_knob
#include <stdint.h>

// One click of knob k: detents is +1 (A changes before B) or -1, at the
// kernel's timestamp of the edge that completed it.
typedef void (*encoder_turn_fn)(enum radio_knob k, int detents, int64_t ts_ns);
// Knob k's switch has been pressed (not called on release).
typedef void (*encoder_push_fn)(enum radio_knob k);

// Claims every knob the board has, reads where each rests, and starts the
// thread. A board with no knobs on the Pi's GPIO (radio_hw_knob()'s a_pin
// -1 for all) claims nothing and starts nothing. Returns the number of
// knobs being read, or -1 if the lines can't be claimed or the thread
// can't start (already logged).
int encoder_start(encoder_turn_fn on_turn, encoder_push_fn on_push);

// ---- The steps the thread runs, for encoder_test.c ----------------------
//
// Levels are the lines' own: 1 = high = open (pulled up), 0 = closed to
// ground.

enum encoder_line { ENCODER_A = 0, ENCODER_B = 1, ENCODER_SW = 2 };

// Sets where to report to. encoder_start() calls it.
void encoder_set_hooks(encoder_turn_fn on_turn, encoder_push_fn on_push);

// Starts reading knob k from its lines' current levels: no turn is
// counted from wherever it rests, and a switch already closed isn't a
// press until it opens and closes again.
void encoder_arm(enum radio_knob k, int edges_per_detent, int a, int b, int sw);

// One edge: knob k's line has gone to level at ts_ns.
void encoder_edge(enum radio_knob k, enum encoder_line line, int level, int64_t ts_ns);

// Ends any switch debounce lockout that has run out by now_ns.
void encoder_tick(int64_t now_ns);

// The earliest time encoder_tick() has something to do, or 0 for none.
int64_t encoder_next_due(void);

#endif /* ENCODER_H */
