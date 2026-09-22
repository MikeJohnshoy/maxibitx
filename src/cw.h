// cw.h

#ifndef CW_H
#define CW_H

// CW sidetone and keying pitch, Hz. cw_get_sample() generates this one
// tone; sound.c uses it both as the local sidetone and as tx_pipeline.c's
// CW input, so the sidetone is exactly what's transmitted. rx_audio.c's
// CW demod and narrow filter are centered on it too, and tx_pipeline.h's
// IF shift is derived from it - changing it moves all of those.
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
// owns the envelope advance. Returns the CW_PITCH_HZ tone times the
// attack/decay envelope, approximately [-1, 1].
double cw_get_sample(void);

#endif /* CW_H */
