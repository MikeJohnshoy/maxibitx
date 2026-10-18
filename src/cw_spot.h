// cw_spot.h
//
// Finds the strongest CW tone in a stretch of audio, for SPOT (auto
// zero-beat): radio_spot() (radio.h) moves the dial so that the tone lands
// on the CW pitch. rx_audio.c keeps the audio it searches.

#ifndef CW_SPOT_H
#define CW_SPOT_H

// The sample rate cw_spot_find() expects: the demodulated audio at 96 kHz,
// summed 16 samples at a time.
#define CW_SPOT_RATE 6000

// How much audio a search uses, in samples at CW_SPOT_RATE: 1.5 s.
#define CW_SPOT_SAMPLES 9000

// Searches n samples of audio at CW_SPOT_RATE for the strongest tone
// between lo_hz and hi_hz. Returns 1 and its frequency in *freq_hz, or 0
// when there is no clear one: nothing at least 10 dB above the median, or
// a stronger tone within 150 Hz outside the band (cw_spot.c says why).
// Any thread; see cw_spot_test for what it costs.
int cw_spot_find(const float *x, int n, double lo_hz, double hi_hz, double *freq_hz);

#endif /* CW_SPOT_H */
