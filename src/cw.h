// cw.h

#ifndef CW_H
#define CW_H

// Sidetone/keying pitch - what the operator actually hears on the local
// monitor (see cw_get_sample()). This is what a real CW pitch control
// would adjust. As of docs/ARCHITECTURE.md build order step 5, this is
// also the ONLY frequency cw.c ever generates - the same real-valued,
// envelope-shaped tone at CW_PITCH_HZ feeds both the local sidetone
// monitor AND (as the shared TX pipeline's `i_sample`, sound.c) the
// actual TX-modulating waveform, matching real sbitx's own
// `output_speaker[j] = i_sample * sidetone` pattern (one signal, two
// uses) instead of a second, IF-shifted NCO of its own.
//
// Before step 5, cw.c generated a SECOND tone here, at
// CW_PITCH_HZ + TX_IF_OFFSET_HZ (a now-removed constant - see this
// file's git history / docs/ARCHITECTURE.md §5 for the derivation),
// specifically to land the actual TX product inside the crystal
// filter's passband without a phasing/Hilbert stage. That whole
// IF-shifted-NCO/residual-correction scheme (and the matching
// `- CW_PITCH_HZ` term `radio_tx_apply()`, radio.c, used to apply to
// clk2 for the same reason) is now handled instead by tx_pipeline.c's
// shared IF bin-rotate, bench-derived and verified in
// docs/ARCHITECTURE.md §10 step 4 - see radio.c's radio_tx_apply() for
// the current clk2 formula and derivation.
#define CW_PITCH_HZ 700

// Call once at startup, after radio_hw_gpio_init() (CW_KEY must already
// be configured) and after vfo_init_phase_table().
void cw_init(void);

// Call once per audio block (~10.7ms - sound.c's PERIOD_FRAMES at
// 96kHz) from the audio thread. Polls the key, manages the keying-burst
// hang timer, and is the only place this module calls radio_set_tx().
void cw_poll_key(void);

// True while a keying burst has TX asserted (radio_set_tx(1) called and
// not yet released). sound.c checks this before pulling samples from
// cw_get_sample().
int cw_tx_active(void);

// Call once per audio sample while cw_tx_active() is true. Returns the
// next output sample: the sidetone, scaled by the attack/decay envelope
// as the key goes down/up. Range is approximately [-1, 1]. Owns the
// envelope advance.
//
// This is now the ONLY sample cw.c produces (see CW_PITCH_HZ's comment
// above) - sound.c uses it two ways every TX sample: directly, for the
// local sidetone monitor (unchanged), and as tx_pipeline.c's `i_sample`
// input, one full TX_PIPELINE_BLOCK_LEN-sized block at a time, to
// produce the actual TX-modulating waveform. There is no longer a
// second, IF-shifted function to call afterward.
double cw_get_sample(void);

#endif /* CW_H */
