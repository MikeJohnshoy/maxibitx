// tx_pipeline.h
//
// The shared FFT overlap-save TX pipeline docs/ARCHITECTURE.md §5
// describes: one filter, one explicit sideband-zero step, one shared IF
// bin-rotate, meant to eventually replace cw.c's own dedicated
// IF-shifted NCO (cw_tx_carrier/TX_IF_OFFSET_HZ) and the
// CW_PITCH_HZ residual correction radio_tx_apply() (radio.c) applies to
// clk2 because of it. Built on fft_filter.h's self-contained struct
// filter (see its own header for why that's a per-instance engine, not
// sbitx's shared-globals one).
//
// docs/ARCHITECTURE.md build order step 4 status: this module exists
// and is bench-verified (tx_pipeline_test.c) against a synthetic stand-in
// for cw.c's real sidetone - it is NOT wired into sound.c or cw.c yet.
// cw.c's existing cw_tx_carrier/TX_IF_OFFSET_HZ path is completely
// untouched and is still what the real, running binary transmits with;
// this is a parallel, proven-on-the-bench-only replacement, not yet a
// live one. See ARCHITECTURE.md §10 step 4 for what's proven vs. still
// open, and steps 5/6 for what's still ahead before any live cutover.

#ifndef TX_PIPELINE_H
#define TX_PIPELINE_H

#include <complex.h>
#include <stdlib.h>
#include "fft_filter.h"
#include "cw.h" // CW_PITCH_HZ only - see TX_IF_SHIFT_HZ below. Deliberately
                // NOT radio.h/cw.c/radio_hw.h - see tx_pipeline.c's
                // TX_IF_SHIFT_HZ comment for why this module stays free
                // of anything that would pull in GPIO/I2C/ALSA link
                // requirements a bench build has no business needing.

// TX block size/impulse length: identical to real sbitx's own choice and
// step 2's already-bench-verified fft_filter_test.c parameters
// (filter_new(1024, 1025), Kaiser beta=5) - see docs/ARCHITECTURE.md §9's
// "port, don't redesign" note. Exposed here so a caller (this module's
// own test harness today; sound.c, eventually) can size its own buffers
// to match without a second copy of these numbers.
#define TX_PIPELINE_BLOCK_LEN 1024
#define TX_PIPELINE_IMPULSE_LEN 1025
#define TX_PIPELINE_N (TX_PIPELINE_BLOCK_LEN + TX_PIPELINE_IMPULSE_LEN - 1)
#define TX_PIPELINE_KAISER_BETA 5.0f
#define TX_PIPELINE_FS_HZ 96000.0f
#define TX_PIPELINE_BIN_HZ (TX_PIPELINE_FS_HZ / TX_PIPELINE_N) // Fs/N, 46.875Hz here

// ---------------------------------------------------------------------
// TX_IF_SHIFT_HZ / TX_IF_SHIFT_BINS - derivation
//
// docs/ARCHITECTURE.md §5 is explicit that one shared bin-rotate should
// replace BOTH cw.c's TX_IF_OFFSET_HZ (its dedicated IF-shifted NCO) AND
// radio_tx_apply()'s "- CW_PITCH_HZ" residual correction on clk2
// (radio.c) - not just the first of those. That residual exists today
// only because TX_IF_OFFSET_HZ (cw.c) was bench-derived to land the
// wanted mixing product CW_PITCH_HZ *short of* xtal_filter_center, not
// exactly on it (see cw.c's own TX_IF_OFFSET_HZ comment) - a small,
// deliberate imprecision baked into one fixed NCO frequency, corrected
// for after the fact by retuning a completely different clock (clk2). A
// bin-rotate has no reason to inherit that imprecision: aim it to land
// the product exactly on xtal_filter_center in the first place and the
// clk2 correction becomes unnecessary.
//
// The board constants below mirror radio.c's bfo_freq/xtal_filter_center
// *compiled defaults* as of this writing. This module deliberately does
// NOT #include "radio.h"/link radio.c to read the real externs, to keep
// this bench harness free of radio_hw.c/si5351.c/sound.c's hardware
// (GPIO/I2C/ALSA) link requirements - the same "no hardware deps in a
// bench test" precedent fft_filter_test.c already set. Whichever future
// step actually wires this into sound.c/radio.c must either recompute
// this at runtime from the real bfo_freq/xtal_filter_center externs
// (both overridable per-board via data/hw_settings.ini) or re-derive/
// re-verify this constant against whatever board it's built for - it is
// NOT automatically kept in sync with those two variables today.
#define TX_PIPELINE_BENCH_BFO_FREQ_HZ 40035000          // radio.c's bfo_freq default
#define TX_PIPELINE_BENCH_XTAL_CENTER_HZ 40012400       // radio.c's xtal_filter_center default

// Target: (CW_PITCH_HZ + shift_hz), mixed against
// TX_PIPELINE_BENCH_BFO_FREQ_HZ, should produce a difference product
// landing exactly on TX_PIPELINE_BENCH_XTAL_CENTER_HZ:
//   shift_hz = bfo_freq - xtal_filter_center - CW_PITCH_HZ
//            = 22600 - 700 = 21900 Hz
// (cw.c's own TX_IF_OFFSET_HZ, 22600, answers a related but different
// question - "what NCO frequency, added directly, lands close to but
// 700Hz short of center" - these two numbers are not meant to match.)
#define TX_IF_SHIFT_HZ ((float)(TX_PIPELINE_BENCH_BFO_FREQ_HZ - TX_PIPELINE_BENCH_XTAL_CENTER_HZ - CW_PITCH_HZ))

// A bin-rotate can only shift by a whole number of bins (TX_PIPELINE_BIN_HZ
// per step), unlike the old scheme's continuous-frequency NCO.
// TX_IF_SHIFT_BINS is TX_IF_SHIFT_HZ rounded to the nearest bin - the
// resulting rounding residual (a few Hz - see tx_pipeline_test.c's Case A
// for the measured number) is the one new, honestly-quantified cost of
// this migration, and it is checked against the *old* scheme's much
// larger fixed 700Hz residual (CW_PITCH_HZ itself) there too.
#define TX_IF_SHIFT_BINS ((int)(TX_IF_SHIFT_HZ / TX_PIPELINE_BIN_HZ + 0.5f))

// TX_IF_SHIFT_HZ/_BINS above only works for TX_PIPELINE_KEEP_UPPER
// (CW/USB/DIGITAL): it was derived to take the KEPT, POSITIVE-frequency
// half's content (a real tone's spectrum always has energy at both +f
// and -f - zero_sideband() keeps one, discards the other) and rotate it
// up onto TX_PIPELINE_BENCH_XTAL_CENTER_HZ. TX_PIPELINE_KEEP_LOWER
// (LSB) keeps the OTHER half instead - NEGATIVE-frequency content - so
// applying the SAME rotation to it lands the result at a completely
// different, uncentered frequency (docs/ARCHITECTURE.md build order
// step 8's on-air follow-up: LSB measured a hard 0W while USB, the only
// thing sideband-different, put out full rated power - this constant
// being reused unmirrored for both is exactly why: LSB's product was
// landing far enough off TX_PIPELINE_BENCH_XTAL_CENTER_HZ to fall on
// the crystal filter's stopband skirt instead of its passband).
//
// The fix is the mirror-image derivation: for a real tone at
// -CW_PITCH_HZ (the kept half when the tone itself is +CW_PITCH_HZ) to
// land back on that same TX_PIPELINE_BENCH_XTAL_CENTER_HZ target,
//   shift_hz_lsb = bfo_freq - xtal_filter_center + CW_PITCH_HZ
//                = 22600 + 700 = 23300 Hz
// (the sign on CW_PITCH_HZ flips relative to TX_IF_SHIFT_HZ above -
// everything else is identical). This also has the property that
// matters for a real (not single-tone) audio passband: USB's whole
// 300-3000Hz band and LSB's mirrored -300..-3000Hz band both end up
// straddling the same ~22600Hz IF anchor point, extending in opposite
// directions from it - USB upward, LSB downward - which is exactly
// what real USB vs. LSB are supposed to do relative to a shared carrier
// placement, not two unrelated numbers.
#define TX_IF_SHIFT_HZ_LSB ((float)(TX_PIPELINE_BENCH_BFO_FREQ_HZ - TX_PIPELINE_BENCH_XTAL_CENTER_HZ + CW_PITCH_HZ))
#define TX_IF_SHIFT_BINS_LSB ((int)(TX_IF_SHIFT_HZ_LSB / TX_PIPELINE_BIN_HZ + 0.5f))

// Which half of the spectrum survives the explicit sideband-zero step -
// see fft_filter.h's filter_forward() comment on why this is the
// caller's job, not the filter's. Matches real sbitx's MODE_LSB/MODE_CWR
// vs. everything-else split (tx_process(), mj_zbitx/src/sbitx.c) - CW
// (plain, not CWR - maxibitx's enum radio_mode has no CWR yet) groups
// with USB's "keep the upper half" treatment, same as
// docs/ARCHITECTURE.md §5's pipeline diagram describes.
enum tx_pipeline_sideband {
	TX_PIPELINE_KEEP_UPPER, // zero negative-frequency bins - CW, USB, DIGITAL
	TX_PIPELINE_KEEP_LOWER, // zero positive-frequency bins (and DC) - LSB
};

struct tx_pipeline {
	struct filter *filt;
	complex float *rotate_scratch; // N-point scratch for the bin-rotate -
	                                // owned here so a block-rate call
	                                // never mallocs (see tx_pipeline.c)
	long block_count; // running block index - see
	                   // tx_pipeline_process_block()'s phase-continuity
	                   // comment (tx_pipeline.c) for why a per-block
	                   // bin-rotate needs this at all
};

// Allocates and tunes the shared filter for the CW/USB passband
// (300-3000Hz, beta 5 - see docs/ARCHITECTURE.md §4/§9's "port, don't
// redesign" derivation, and real sbitx's own
// filter_tune(tx_filter, 300.0/96000, 3000.0/96000, 5) for USB/CW).
struct tx_pipeline *tx_pipeline_new(void);

// Re-tunes the shared filter's passband - e.g. for LSB's mirrored range,
// once a future step actually needs it. Safe to call at any time, same
// guarantee filter_tune() itself gives (only touches fir_coeff, not the
// running overlap-save state in history/time/freq).
int tx_pipeline_retune(struct tx_pipeline *p, float low_hz, float high_hz, float fs_hz);

// Processes one TX_PIPELINE_BLOCK_LEN-sample block of real baseband
// audio (cw.c's cw_get_sample() sidetone today; SSB/DIGITAL's mic audio
// later, unwritten) through the shared pipeline: passband filter, the
// explicit sideband-zero `sideband` selects, the shared IF bin-rotate
// (TX_IF_SHIFT_BINS above), inverse FFT, and real-part extraction - the
// same "take the real part of a frequency-shifted analytic signal"
// construction that makes single-sideband come out of an FFT bin-zero
// step in the first place (see docs/ARCHITECTURE.md §4). `in`/`out` are
// both TX_PIPELINE_BLOCK_LEN real samples, roughly [-1, 1] in, unity
// passband gain out (same convention fft_filter.h's filter_forward()/
// filter_inverse() already give - see fft_filter.c's gain-fix comment).
void tx_pipeline_process_block(struct tx_pipeline *p, enum tx_pipeline_sideband sideband,
                                const float *in, float *out);

void tx_pipeline_free(struct tx_pipeline *p);

#endif /* TX_PIPELINE_H */
