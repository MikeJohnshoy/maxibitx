// tx_pipeline.h
//
// The shared TX pipeline: one FFT overlap-save chain for every mode (CW,
// USB, LSB, DIGITAL). sound.c feeds it one block of real baseband audio
// at a time and sends its output - a real IF near 22.6kHz - to the
// exciter. Per block: bandpass (300-3000Hz), zero one sideband, rotate
// bins (the IF shift), inverse FFT, real part. How that IF becomes RF:
// docs/03_tx_processing_pipeline.md. Derivations and bench results:
// docs/ARCHITECTURE.md §10 steps 4, 5 and 8.
//
// Built on fft_filter.h's per-instance struct filter. Deliberately free
// of radio.h/radio_hw.h/sound.h, so tx_pipeline_test.c links without any
// hardware code.

#ifndef TX_PIPELINE_H
#define TX_PIPELINE_H

#include <complex.h>
#include <stdlib.h>
#include "fft_filter.h"
#include "cw.h" // CW_PITCH_HZ only

// Block size and impulse length - sbitx's own values
// (filter_new(1024, 1025), Kaiser beta 5). sound.c's PERIOD_FRAMES must
// equal TX_PIPELINE_BLOCK_LEN.
#define TX_PIPELINE_BLOCK_LEN 1024
#define TX_PIPELINE_IMPULSE_LEN 1025
#define TX_PIPELINE_N (TX_PIPELINE_BLOCK_LEN + TX_PIPELINE_IMPULSE_LEN - 1)
#define TX_PIPELINE_KAISER_BETA 5.0f
#define TX_PIPELINE_FS_HZ 96000.0f
#define TX_PIPELINE_BIN_HZ (TX_PIPELINE_FS_HZ / TX_PIPELINE_N) // Fs/N, 46.875Hz here

// ---------------------------------------------------------------------
// IF placement. After the bin rotate, the analog chain takes the
// difference product against bfo_freq (clk1 during TX), which must land
// on xtal_filter_center. What should land there differs by signal:
//
//   CW  - the CW_PITCH_HZ tone itself, so a key-down carrier is on the
//         dial:   shift = bfo_freq - xtal_filter_center - CW_PITCH_HZ
//   SSB - the suppressed carrier (audio 0 Hz), so audio at a Hz goes out
//         at dial+a (USB) or dial-a (LSB):
//                 shift = bfo_freq - xtal_filter_center
//
// Both sidebands use the same SSB shift: USB rotates its kept positive
// half up onto the carrier point, LSB its kept negative half, and each
// band then extends from that same point in its own direction.
//
// A rotate moves whole bins, so each is rounded to the nearest one: 467
// bins (21,890.625 Hz) for CW and 482 (22,593.750 Hz) for SSB, leaving
// the carrier 9.4 Hz and 6.3 Hz low respectively.
//
// Caveat: bfo_freq and xtal_filter_center below are compiled-in copies
// of radio.c's defaults, not the values hw_settings.ini loads at
// startup. They match this board's ini today; on a board with different
// values the TX frequency moves by the difference.
#define TX_PIPELINE_BENCH_BFO_FREQ_HZ 40035000          // radio.c's bfo_freq default
#define TX_PIPELINE_BENCH_XTAL_CENTER_HZ 40012400       // radio.c's xtal_filter_center default

#define TX_IF_SHIFT_CW_HZ ((float)(TX_PIPELINE_BENCH_BFO_FREQ_HZ - TX_PIPELINE_BENCH_XTAL_CENTER_HZ - CW_PITCH_HZ))
#define TX_IF_SHIFT_CW_BINS ((int)(TX_IF_SHIFT_CW_HZ / TX_PIPELINE_BIN_HZ + 0.5f))

#define TX_IF_SHIFT_SSB_HZ ((float)(TX_PIPELINE_BENCH_BFO_FREQ_HZ - TX_PIPELINE_BENCH_XTAL_CENTER_HZ))
#define TX_IF_SHIFT_SSB_BINS ((int)(TX_IF_SHIFT_SSB_HZ / TX_PIPELINE_BIN_HZ + 0.5f))

// What the pipeline is building: which half of the spectrum the
// sideband-zero step keeps, and which IF shift places it (above). CW and
// USB both keep the upper half but anchor different things on the dial.
enum tx_pipeline_signal {
	TX_PIPELINE_CW,  // upper half, CW_PITCH_HZ tone on the dial
	TX_PIPELINE_USB, // upper half, carrier on the dial - also DIGITAL
	TX_PIPELINE_LSB, // lower half, carrier on the dial
};

struct tx_pipeline {
	struct filter *filt;
	complex float *rotate_scratch; // N-point scratch for the bin rotate, so
	                                // the audio path never mallocs
	long block_count; // blocks processed - drives the phase correction in
	                   // tx_pipeline_process_block()
};

// Allocates the pipeline and tunes its filter to 300-3000Hz, keeping both
// the positive and negative images (filter_tune_real()) so either
// sideband is available to the sideband-zero step.
struct tx_pipeline *tx_pipeline_new(void);

// Re-tunes the passband. Safe at any time - it only replaces the filter
// coefficients, not the overlap-save state. Currently unused.
int tx_pipeline_retune(struct tx_pipeline *p, float low_hz, float high_hz, float fs_hz);

// Processes one TX_PIPELINE_BLOCK_LEN block of real baseband audio
// (roughly [-1, 1]; the CW tone, the mic, WSJT-X audio or tone_gen.c's
// test tones) into
// TX_PIPELINE_BLOCK_LEN samples of real IF. Unity gain for a steady tone
// in the passband. Must be called with exactly one block per call and
// every block in sequence - the overlap-save history and the phase
// correction both depend on it.
void tx_pipeline_process_block(struct tx_pipeline *p, enum tx_pipeline_signal signal,
                                const float *in, float *out);

void tx_pipeline_free(struct tx_pipeline *p);

#endif /* TX_PIPELINE_H */
