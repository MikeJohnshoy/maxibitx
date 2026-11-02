// codec_check.h
//
// The start-up check for the codec's sampling state. Now and then the
// WM8731 starts with its sample timing modulated in a pattern that repeats
// every 128 samples (it makes 96 kHz from a 12 MHz clock, 125 clocks a
// sample, in its USB mode). Every received signal then gets copies at
// multiples of 750 Hz, filling the spectrum with ghosts. The pattern has
// more than one shape: one is strongest at its 5th harmonic (copies
// strongest 3750 Hz away), another at its 1st, with every signal also
// imaged about the 24 kHz IF. Its registers are the same as in a good
// start; restarting its digital interface (ACTIVE off and on) clears it.
//
// The pattern shows plainly on the right channel (the mic line, quiet at
// start-up): the power of its sample-to-sample differences, folded over a
// 128-sample cycle, has a strong harmonic. codec_check measures the
// strongest of them for a quarter of a second in receive, and if it's
// there restarts the codec's interface and measures again.
// docs/08_troubleshooting_and_bringup.md, "A spectrum full of ghosts".

#ifndef CODEC_CHECK_H
#define CODEC_CHECK_H

#include <stdint.h>

#define CODEC_CHECK_PERIOD 128                    // samples in the pattern
#define CODEC_CHECK_HARMONICS 62 // the 1st to the 62nd are scored
#define CODEC_CHECK_FRAMES (CODEC_CHECK_PERIOD * 188) // ~0.25 s at 96 kHz
#define CODEC_CHECK_BAD 0.3   // score: 0.5 to 0.9 in the bad states, under 0.1 in a good one
#define CODEC_CHECK_TRIES 3   // interface restarts before giving up
#ifndef CODEC_CHECK_SETTLE_MS
#define CODEC_CHECK_SETTLE_MS 1000 // after the audio starts, before measuring
#endif

// The score of one measurement: power[] is the summed squared difference
// of successive samples at each phase of the cycle. The strongest of that
// pattern's harmonics, 1 to CODEC_CHECK_HARMONICS, over its mean, and in
// *harmonic (if not NULL) which one; 0 when the power doesn't depend on
// phase. The 63rd and 64th are left out: a good codec shows about 0.1 at
// the 63rd. Ordinary priority: about 16,000 sines and cosines.
double codec_check_score(const double power[CODEC_CHECK_PERIOD], int *harmonic);

// Starts the check on its own thread. restart: restarts the codec's
// interface, 0 or -1 on failure; NULL measures and reports only. Returns
// 0, or -1 if the thread can't start.
int codec_check_start(int (*restart)(void));

// The audio thread, after every capture read: interleaved frames of
// `channels` int32 channels, the right channel (1) measured. usable: 0
// while transmitting (the line carries the mic then), which starts the
// measurement over. Never blocks.
void codec_check_block(const int32_t *interleaved, int frames, int channels, int usable);

// The audio thread, when frames were lost (an xrun): the 128-sample
// cycle's phase is lost too, so the measurement starts over.
void codec_check_gap(void);

// What the check came to.
enum {
  CODEC_CHECK_RUNNING,    // not yet decided
  CODEC_CHECK_CLEAN,      // clean, at once or after restarts
  CODEC_CHECK_BAD_STATE,  // still patterned, or couldn't restart it
  CODEC_CHECK_UNMEASURED, // no receive audio to measure, or the check couldn't start
};
int codec_check_outcome(void);

// Restarts the WM8731's digital interface - register 9 (ACTIVE) to 0 and
// back to 1 - over the I2C bus the kernel driver found it on. Returns 0,
// or -1 if there's no WM8731 or the write fails.
int codec_check_wm8731_restart(void);

#endif /* CODEC_CHECK_H */
