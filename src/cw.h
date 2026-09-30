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

#include <stdint.h>

// Call once at startup, after vfo_init_phase_table() and before
// key_input_start(), which is given cw_key_closed() below.
void cw_init(void);

// Call once per audio block (~10.7ms) from the audio thread, with the
// CLOCK_MONOTONIC time the block's capture read returned and its length in
// samples. Estimates where the block really ended from those times (cw.c),
// takes the key's edges from key_input.c for the interval since the last
// boundary and places each at its sample in the block about to be
// generated, then requests or releases TX. What a closure means depends on
// radio_get_mode(): in CW and CWR either contact is a straight key with semi
// break-in (hang timer); in USB/LSB the ring is a mic PTT switch (TX follows
// the switch, no hang). DIGITAL ignores the key - its PTT comes from CAT,
// rigctld or HPSDR - and releases a TX the key started before the change.
void cw_poll_key(int64_t capture_ns, int n);

// key_input.c's on_closed hook, run on its input thread when a contact
// closes: requests TX at once in CW and CWR, rather than a block later
// when cw_poll_key() sees the edge.
void cw_key_closed(void);

// Called by the audio thread as it exits, for any reason: releases a TX
// this file asserted and refuses any further request, since from then on
// nothing would release one.
void cw_audio_stopped(void);

// Control surfaces call this after queueing text with keyer_send_text():
// requests TX at once in CW and CWR, as a key closure does, so the T/R
// sequence starts before the first element does.
void cw_text_queued(void);

// True while cw.c has TX asserted, in any mode. sound.c checks
// it before pulling TX audio (this tone in CW, the mic in USB/LSB); remote
// PTT paths check it so the local key wins.
int cw_tx_active(void);

// Call exactly once per audio sample while transmitting in CW - it
// owns the envelope advance, and counts samples to apply each key edge at
// its offset in the block. Returns the current-pitch tone times the
// attack/decay envelope, approximately [-1, 1]. Each fall starts
// cw_weighting_hold_samples() after key-up, which makes the transmitted mark
// equal the key-down time at the envelope's 50% points (cw.c).
double cw_get_sample(void);

// The keying envelope's value, 0 to 1, as cw_get_sample() last applied it -
// for measuring the envelope directly (test-cw) rather than recovering it
// from the tone.
double cw_envelope_level(void);

// Samples each fall is held off after key-up for 1:1 weighting, derived from
// the envelope table at cw_init(): 150 (1.56 ms) for the current table.
int cw_weighting_hold_samples(void);

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
