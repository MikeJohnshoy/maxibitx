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

// ---------------------------------------------------------------------
// Peak limiter (ALC). Applies gain min(1, ceiling/envelope) to hold the
// transmit envelope at or below tx_pipeline_set_ceiling()'s value. It
// only ever reduces, so quiet audio transmits quietly; raising the
// average would be a speech compressor, which belongs upstream of this
// file. Envelope means 2*|out_c[i]|, the magnitude of the still-complex
// signal, not the input audio's peak. A constant-envelope signal at or
// below the ceiling - CW key-down, a steady tone - never moves the gain.
// Why each of those: docs/03_tx_processing_pipeline.md, "Setting power".
//
// The output is delayed by TX_ALC_LOOKAHEAD_SAMPLES while the gain is
// computed from the undelayed signal, and attack is rate-limited to
// cross the whole gain range in exactly that window - so the gain has
// reached its target before the peak that needs it is emitted. That
// delay is also the TX path's added latency: 2ms, which shifts the
// transmitted envelope (CW element timing included) without changing
// its shape.
#define TX_ALC_LOOKAHEAD_SAMPLES 192  // 2ms at 96kHz; also the attack time
#define TX_ALC_RELEASE_S 0.25f        // slow enough not to pump in a syllable
#define TX_ALC_METER_DECAY_S 1.0f     // peak-hold decay for the ALC readout
#define TX_ALC_METER_RANGE_DB 20.0f   // dB the meter falls in one decay time

struct tx_pipeline {
	struct filter *filt;
	complex float *rotate_scratch; // N-point scratch for the bin rotate, so
	                                // the audio path never mallocs
	long block_count; // blocks processed - drives the phase correction in
	                   // tx_pipeline_process_block()

	// Limiter state, all in the audio thread except ceiling (written by
	// whoever sets power) and meter_db (read by whoever reports ALC).
	volatile float ceiling;    // amplitude, (0, 1]; 1.0 = limiter at unity
	float gain;                 // current limiter gain, (0, 1]
	float delay[TX_ALC_LOOKAHEAD_SAMPLES]; // output samples awaiting their gain
	int delay_pos;              // next slot to use; persists across blocks,
	                             // since the block length isn't a multiple
	                             // of the look-ahead
	volatile float meter_db;   // gain reduction in dB, peak-held
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

// Sets the limiter's ceiling as an amplitude in (0, 1], where 1.0 is the
// full-scale signal the per-band calibration turns into
// full_scale_power watts. sound.c derives it from hw_settings.ini's
// full_scale_power and max_power and the operator's POWER setting; see
// hw_settings.h, "TX power ceiling". Out-of-range values clamp to 1.0.
// Safe to call from another thread between blocks.
void tx_pipeline_set_ceiling(struct tx_pipeline *p, float ceiling);

// Current gain reduction in dB (0.0 = not limiting), peak-held and
// decaying over TX_ALC_METER_DECAY_S so a meter can read it. Raw
// per-block values change far faster than anyone can follow.
float tx_pipeline_alc_db(const struct tx_pipeline *p);

void tx_pipeline_free(struct tx_pipeline *p);

#endif /* TX_PIPELINE_H */
