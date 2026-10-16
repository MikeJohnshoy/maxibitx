// cw_decoder.h
//
// Decodes received CW into text, for hand-sent code at about 5 to 35 WPM
// (Farnsworth spacing included) at good signal-to-noise ratios. It learns
// the sender's timing from the signal itself: no WPM setting is used. See
// cw_decoder.c for how, and docs/dsp_design_notes/rx_cw_decoder_study.md
// for the measurements behind it.
//
// rx_audio.c feeds it the demodulated audio in CW and CWR; any number of
// readers (rigctld's \get_cw_text, the console) take the text.

#ifndef CW_DECODER_H
#define CW_DECODER_H

#include <stdint.h>

// Call once at startup, before the audio thread starts.
void cw_decoder_init(void);

// Audio thread: n samples of the demodulated audio at 96 kHz (any level),
// with the CW tone at pitch_hz. Does nothing while the decoder is off.
void cw_decoder_process(const float *audio, int n, int pitch_hz);

// On (1, the default) or off (0). Turning it off stops decoding at the next
// block; turning it on starts afresh.
void cw_decoder_set_enabled(int on);
int cw_decoder_get_enabled(void);

// Forget the sender's timing and start learning again, at the next block.
// For a retune or a mode change: the next signal may be another station.
// Any thread.
void cw_decoder_reset(void);

// The decoded text. Characters are numbered from 0 as they are decoded;
// copies those from number `since` on into buf (NUL-terminated, at most
// len - 1 of them) and returns the number of the next character to come.
// Pass 0 the first time, then what it returned. If `since` is older than
// the decoder still holds (it keeps the last CW_DECODER_TEXT_KEEP), the
// copy starts at the oldest it has. Text is upper case, a space between
// words, prosigns as <AR>, <SK>, ..., '*' for a character it couldn't
// read, and '\n' when a transmission ends (a long pause) or on a reset.
// Any thread.
#define CW_DECODER_TEXT_KEEP 1024
uint32_t cw_decoder_read(uint32_t since, char *buf, int len);

// The sender's speed as the decoder has learned it, in WPM (from the dot
// length), or 0 before it has learned one.
int cw_decoder_get_wpm(void);

#endif /* CW_DECODER_H */
