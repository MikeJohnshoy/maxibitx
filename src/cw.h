// cw.h

#ifndef CW_H
#define CW_H

// DEFAULT CW sidetone and keying pitch, Hz - what everything starts at,
// and what the bench harnesses measure against. The live pitch is
// runtime-settable (cw_set_pitch() below), so this is the starting point
// rather than the whole story.
//
// cw_get_sample() generates this one tone; sound.c uses it both as the
// local sidetone and as tx_pipeline.c's CW input, so the sidetone is
// exactly what's transmitted. rx_audio.c's CW demod and narrow filter are
// centered on the same pitch, and tx_pipeline.c's CW IF shift is derived
// from it so a key-down lands on the dial at any pitch.
#define CW_PITCH_HZ 700

// Call once at startup, after radio_hw_gpio_init() (CW_KEY must already
// be configured) and after vfo_init_phase_table().
void cw_init(void);

// Call once per audio block (~10.7ms) from the audio thread. Polls the
// key/PTT line and is the only place this module calls radio_set_tx().
// What a closure means depends on radio_get_mode(): in CW a straight key
// with semi break-in (hang timer); in USB/LSB a mic PTT switch (TX follows
// the switch, no hang). DIGITAL ignores the line - its PTT comes from
// CAT, rigctld or HPSDR.
void cw_poll_key(void);

// True while cw_poll_key() has TX asserted, in any mode. sound.c checks
// it before pulling TX audio (this tone in CW, the mic in USB/LSB); remote
// PTT paths check it so the local key wins.
int cw_tx_active(void);

// Call exactly once per audio sample while transmitting in CW - it
// owns the envelope advance. Returns the current-pitch tone times the
// attack/decay envelope, approximately [-1, 1].
double cw_get_sample(void);

// Retunes the keyed tone. Not a control in its own right: the sidetone has
// to equal the pitch the receiver is rendering, or zero-beating by ear puts
// the transmission off frequency by the difference, so this is driven by
// radio_set_cw_pitch() (radio.c) together with the RX side and the TX IF
// shift. Call that, not this.
//
// Transmitting the tone at a new frequency would move the carrier unless
// tx_pipeline.c's CW shift is re-derived to match - which is why the
// coordinating function exists, and why it refuses to act mid-transmission.
//
// Restarts the tone oscillator's phase, so calling it between elements is
// silent and calling it mid-element would click. vfo_start() quantizes to
// 96000/65536 = 1.46Hz steps, the same quantization rx_audio.c's BFO has,
// so the two stay in step at every pitch.
void cw_set_pitch(int hz);

// The pitch cw_get_sample() is currently generating, as last set - the
// nominal value, not the oscillator's quantized one.
int cw_get_pitch(void);

#endif /* CW_H */
