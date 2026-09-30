// keyer.h
//
// The CW keyer: the key jack's edges in (key_input.c, already placed at
// sample offsets in the block), a key-down/up value per sample out, which
// cw.c's envelope follows. Straight key, bug, ultimatic, iambic A and B,
// 1-60 WPM. Two implementations, one chosen by the Makefile: keyer.c
// (every mode) and keyer_straight.c (the straight key alone), so the keyer
// can be removed or replaced without touching anything else.
// docs/dsp_design_notes/cw_keyer_design_study.md §10 and §15.

#ifndef KEYER_H
#define KEYER_H

#include "key_input.h"
#include <stdint.h>

enum keyer_mode {
  KEYER_STRAIGHT,
  KEYER_BUG,
  KEYER_ULTIMATIC,
  KEYER_IAMBIC_A,
  KEYER_IAMBIC_B,
  KEYER_MODES,
};

#define KEYER_WPM_MIN 1
#define KEYER_WPM_MAX 60
#define KEYER_WPM_DEFAULT 20

// Audio thread, once per block: applies ev[] (offsets non-decreasing, as
// key_input_take() returns them) and writes key[0..n-1], 1 = key down.
// Returns nonzero while TX is wanted: the key was down, or an element was in
// progress (its trailing space included), at some point in this block.
int keyer_run_block(const struct key_event *ev, int n_ev, uint8_t *key, int n);

// Returns to idle with both paddles up, keeping the mode and speed. cw_init()
// calls it; so do the bench harnesses between cases.
void keyer_reset(void);

// Control side, any thread. A mode change takes effect once the keyer is
// idle; a speed change at the next element start - never mid-element.
// keyer_set_mode() returns 0, or -1 if this keyer doesn't offer that mode.
// keyer_set_wpm() clamps to KEYER_WPM_MIN..KEYER_WPM_MAX and returns the
// speed in force. keyer_get_mode() returns the mode last set, which may not
// have taken effect yet.
int keyer_set_mode(enum keyer_mode m);
enum keyer_mode keyer_get_mode(void);
int keyer_set_wpm(int wpm);
int keyer_get_wpm(void);

// "straight", "bug", "ultimatic", "iambic A", "iambic B"; "?" otherwise.
const char *keyer_mode_name(enum keyer_mode m);

#endif /* KEYER_H */
