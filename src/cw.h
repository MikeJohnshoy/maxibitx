// cw.h

#ifndef CW_H
#define CW_H

// Sidetone/keying pitch - what the operator actually hears on the local
// monitor (see cw_get_sample() / SIDETONE_SCALE in sound.c). This is what
// a real CW pitch control would adjust. It is NOT the frequency written
// to the DAC's TX-feeding (right) channel - see TX_IF_OFFSET_HZ in cw.c
// and cw_get_tx_sample().
//
// Shared with radio.c: cw.c's TX carrier sits at CW_PITCH_HZ +
// TX_IF_OFFSET_HZ rather than at RX_IF_FREQ_HZ (radio.h), which - left
// uncorrected - makes the transmitted RF frequency land CW_PITCH_HZ
// below the tuned dial frequency (bench-confirmed; see
// docs/03_tx_processing_pipeline.md's "Known limitations" for the
// derivation). radio.c's radio_tx_apply() applies a +CW_PITCH_HZ
// correction to clk2 for the duration of TX to cancel this out - the
// mirror of real sbitx's own rx_pitch TX correction, applied here at the
// one place all TX (straight key via cw.c, and remote MOX via
// hpsdr_p1.c) funnels through.
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
// envelope advance - call this before cw_get_tx_sample() for the same
// sample, not after.
double cw_get_sample(void);

// Call once per audio sample, immediately after cw_get_sample(), while
// cw_tx_active() is true. Returns the actual TX-modulating waveform -
// same envelope position cw_get_sample() just advanced to, but at the
// IF-shifted carrier (CW_PITCH_HZ + TX_IF_OFFSET_HZ) that lands inside
// the crystal filter's passband instead of producing two RF tones. See
// cw.c's TX_IF_OFFSET_HZ comment. Range is approximately [-1, 1].
double cw_get_tx_sample(void);

#endif /* CW_H */
