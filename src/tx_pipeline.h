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
// IF shift. After the bin rotate, the analog chain takes the difference
// product against bfo_freq (clk1 during TX), which must land on
// xtal_filter_center. For the kept upper half, a tone at CW_PITCH_HZ
// lands there when
//   shift = bfo_freq - xtal_filter_center - CW_PITCH_HZ = 22600 - 700 = 21900 Hz
// and for the kept lower half (LSB, content at -CW_PITCH_HZ) when
//   shift = bfo_freq - xtal_filter_center + CW_PITCH_HZ = 22600 + 700 = 23300 Hz
// A rotate moves whole bins, so each is rounded to the nearest bin
// (467 and 497); the carrier ends up 9.4Hz (upper) and 3.1Hz (lower) off.
//
// Caveats, both open:
//  - bfo_freq and xtal_filter_center are compiled-in copies of radio.c's
//    defaults, not the values hw_settings.ini loads at startup. They
//    match this board's ini today; on a board with different values the
//    TX frequency moves by the difference.
//  - Anchoring a CW_PITCH_HZ tone on the dial is right for CW, but for
//    USB/LSB/DIGITAL it puts the suppressed carrier ~700Hz off the dial
//    (docs/03_tx_processing_pipeline.md, "Known limitations").
#define TX_PIPELINE_BENCH_BFO_FREQ_HZ 40035000          // radio.c's bfo_freq default
#define TX_PIPELINE_BENCH_XTAL_CENTER_HZ 40012400       // radio.c's xtal_filter_center default

#define TX_IF_SHIFT_HZ ((float)(TX_PIPELINE_BENCH_BFO_FREQ_HZ - TX_PIPELINE_BENCH_XTAL_CENTER_HZ - CW_PITCH_HZ))
#define TX_IF_SHIFT_BINS ((int)(TX_IF_SHIFT_HZ / TX_PIPELINE_BIN_HZ + 0.5f))

#define TX_IF_SHIFT_HZ_LSB ((float)(TX_PIPELINE_BENCH_BFO_FREQ_HZ - TX_PIPELINE_BENCH_XTAL_CENTER_HZ + CW_PITCH_HZ))
#define TX_IF_SHIFT_BINS_LSB ((int)(TX_IF_SHIFT_HZ_LSB / TX_PIPELINE_BIN_HZ + 0.5f))

// Which half of the spectrum the sideband-zero step keeps. CW groups with
// USB, as in sbitx (which keeps the lower half only for LSB and CWR).
enum tx_pipeline_sideband {
	TX_PIPELINE_KEEP_UPPER, // zero negative-frequency bins - CW, USB, DIGITAL
	TX_PIPELINE_KEEP_LOWER, // zero positive-frequency bins (and DC) - LSB
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
// (roughly [-1, 1]; the CW tone, the mic or WSJT-X audio) into
// TX_PIPELINE_BLOCK_LEN samples of real IF. Unity gain for a steady tone
// in the passband. Must be called with exactly one block per call and
// every block in sequence - the overlap-save history and the phase
// correction both depend on it.
void tx_pipeline_process_block(struct tx_pipeline *p, enum tx_pipeline_sideband sideband,
                                const float *in, float *out);

void tx_pipeline_free(struct tx_pipeline *p);

#endif /* TX_PIPELINE_H */
