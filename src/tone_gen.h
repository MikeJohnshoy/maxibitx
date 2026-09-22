// tone_gen.h
//
// TX test-tone generator: a transmit audio source for checking carrier
// placement, power and linearity. While on, sound.c transmits these tones
// instead of the mode's normal source (key tone, mic or USB audio); any
// PTT source keys it. Design and measurement procedure:
// docs/dsp_design_notes/tx_test_tones_and_alc.md.
//
// No hardware or radio dependencies, so the bench tests can link it.

#ifndef TONE_GEN_H
#define TONE_GEN_H

enum tone_gen_mode {
    TONE_GEN_OFF = 0,
    TONE_GEN_SINGLE = 1, // TONE_GEN_SINGLE_HZ at full scale
    TONE_GEN_TWO = 2,    // TONE_GEN_TWO_LOW_HZ + TONE_GEN_TWO_HIGH_HZ, 0.5 each
};

#define TONE_GEN_SINGLE_HZ   1000.0
#define TONE_GEN_TWO_LOW_HZ   700.0
#define TONE_GEN_TWO_HIGH_HZ 1900.0

// Seconds of continuous transmit with the generator on before
// maxibitx.c's idle loop turns it off and drops PTT.
#define TONE_GEN_TIMEOUT_S 30

// Safe from any thread; takes effect at the next sample. Out-of-range
// values are ignored.
void tone_gen_set_mode(enum tone_gen_mode m);
enum tone_gen_mode tone_gen_get_mode(void);

// Next sample at 96kHz, peak 1.0 in both tone modes (0 when off). Call
// only from the audio thread, once per sample. Phases run continuously
// across calls and mode changes.
double tone_gen_sample(void);

#endif /* TONE_GEN_H */
