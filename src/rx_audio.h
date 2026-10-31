// rx_audio.h
//
// The receiver's audio: demodulates the I/Q sound.c mixes down (CW, CWR,
// USB, LSB) through the wide image-reject filter, the narrow CW filter and
// the AGC, to the local speaker and, before the volume, to WSJT-X and TCI.
// Also the S-meter and SPOT's receive half. rx_audio.c's header has the
// stages; docs/02_rx_processing_pipeline.md and
// docs/dsp_design_notes/rx_audio_demod_design.md the design.

#ifndef RX_AUDIO_H
#define RX_AUDIO_H

#include <stdint.h>

// Which stage-3 ("single signal" selectivity) implementation is in use -
// see rx_audio_set_narrow_filter_impl() below.
enum rx_narrow_filter_impl {
	RX_NARROW_FILTER_ELLIPTIC, // 8-pole elliptic IIR from the pre-designed
	                            // bank, src/narrow_filter_bank.h
	RX_NARROW_FILTER_FFT,      // the shared FFT overlap-save filter
	                            // (src/rx_filter.c) - the default
};

// Call once at startup, after vfo_init_phase_table().
void rx_audio_init(void);

// 0-100. Scales the demodulated audio before it reaches the codec - the
// local speaker/headphones only (uac_out/WSJT-X is unaffected). 100%
// maps to rx_audio.c's RX_VOLUME_MAX, not unity gain; see that constant.
// Log (audio) taper: each 1% is 0.5dB (RX_VOLUME_RANGE_DB over the
// range), and 0% is a true mute.
void rx_audio_set_volume(int percent);

// Current volume, 0-100, in the same units rx_audio_set_volume() takes,
// so a client can read back a volume it didn't set (rigctld's "l AF").
int rx_audio_get_volume(void);

// Mute: 1 silences the local speaker without changing the volume, which
// returns on unmute. rx_audio_set_volume() leaves the mute as it is.
// Like the volume, uac_out/WSJT-X is unaffected.
void rx_audio_set_mute(int on);
int rx_audio_get_mute(void);

// Stage 3: the narrow, post-demodulation "single signal" selectivity
// filter, centred on the selected CW pitch - a separate stage from the wide
// image-reject filter upstream of it, deliberately independent of it
// (docs/dsp_design_notes/rx_audio_demod_design.md §7.6, §8). Two
// implementations, both always running so switching between them never
// thumps:
//   - the FFT filter (src/rx_filter.c, the shared overlap-save engine
//     tx_pipeline.c uses) - the default. Tunes pitch and width
//     continuously, and its stopband keeps falling where an elliptic
//     floors out near -50dB; its keyed attack is a little slower.
//   - an 8-pole elliptic IIR from a bank designed offline
//     (src/narrow_filter_bank.h): the faster attack, and the fallback for
//     a block that isn't RX_FILTER_BLOCK_LEN long.
// Pitch and width are chosen from the bank's values for both (below);
// there is no arbitrary-bandwidth setter, by design (rx_audio_demod_
// design.md §8.2). docs/dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md
// compares the two, §16 on the default.

// Enable (1, the default) or bypass (0) stage 3, the narrow filter
// above. The filter itself keeps running either way (its history stays
// warm) - only which signal reaches stage 4's AGC/output changes - so
// there's no settling-time thump when toggling back on. rigctld's
// "u"/"U NARROW" (hamlib.c).
//
// This is the operator's REQUEST. It is always remembered, but it only
// reaches the audio when the mode-level inhibit below is clear.
void rx_audio_set_narrow_filter(int enable);

// Whether stage 3 is in circuit right now, 0 or 1 - the request above AND
// no inhibit, so it answers "am I hearing the filter" rather than "what is
// the setting". Same "let a client read back what it didn't itself just
// set" reasoning as rx_audio_get_volume(); a client that sets the request
// and reads back 0 is being told the current mode is overriding it.
int rx_audio_get_narrow_filter(void);

// Suppress stage 3 regardless of the request above, for a mode where a
// ~300Hz filter centered on the CW pitch is not a narrow choice but a
// wrong one. radio.c's radio_set_mode() owns this: it is called on every
// mode change with (mode == RADIO_MODE_DIGITAL), which is the only mode
// that inhibits it today.
//
// Why the daemon has to enforce this rather than the control panel greying
// a checkbox: DIGITAL's whole point is uac_out (rx_audio_process() below),
// WSJT-X's audio feed, which is taken downstream of stage 3 - and WSJT-X
// sets the mode itself over CAT, so a panel-side guard would be bypassed
// by the very client that needs the protection. Leaving a CW session's
// 300Hz filter in hands the decoder a fraction of the ~2.7kHz window FT8
// occupies. docs/dsp_design_notes/rx_uac_out_digital_mode_bandwidth.md
// §2, §6, §11.
//
// The operator's request survives the inhibit, so returning to CW restores
// the filter they had rather than making them re-enable it.
void rx_audio_inhibit_narrow_filter(int inhibit);

// Whether the inhibit above is set, 0 or 1. Distinguishes "the operator
// turned stage 3 off" from "the mode took it away", which a control surface
// needs in order to say which one it is showing.
int rx_audio_narrow_filter_inhibited(void);

// Which stage-3 implementation is selected when the narrow filter above
// is enabled: 1 = the shared FFT filter (src/rx_filter.c, the default,
// always minimum phase), 0 = an elliptic IIR from the pre-designed bank.
//
// The FFT filter is the default because it tunes pitch and width
// continuously instead of to the bank's rungs. The bank is not deprecated:
// it is the faster of the two on a keyed element's attack, and it is what
// rx_audio_process() falls back to for a block that isn't
// RX_FILTER_BLOCK_LEN long.
//
// Both run every block regardless of which is selected, so switching never
// thumps - and so selecting the cheaper filter saves no CPU, which is worth
// knowing before reaching for it on a slow board.
// rx_narrow_filter_fft_vs_elliptic.md §17 has the measured costs.
//
// Wired to rigctld's "u"/"U FFTFILT" (hamlib.c) and the control panel.
void rx_audio_set_narrow_filter_impl(int use_fft);

// Current implementation selection, 0 or 1 (same meaning as
// rx_audio_set_narrow_filter_impl()'s argument) - same readback
// reasoning as rx_audio_get_narrow_filter().
int rx_audio_get_narrow_filter_impl(void);

// BENCH ONLY - no control surface reaches this, and the daemon never calls
// it. Selects which realization the FFT implementation uses: 1 = minimum
// phase (what the radio always runs), 0 = linear phase.
//
// Linear phase is not an operator choice. It has the same passband but
// 16.0ms of group delay against minimum phase's 4.5ms, and a keyed element
// takes 20.3ms to settle instead of 8.7ms (rx_narrow_filter_fft_vs_
// elliptic.md §11, §16). It stays for rx_audio_impulse_test.c, which uses
// linear phase as the extreme case that makes the AGC's group-delay
// misalignment visible at all. (rx_filter_test.c's Case E checks the
// minimum-phase transformation itself at rx_filter's own level,
// rx_filter_set_min_phase().)
//
// Not for the audio thread either way: switching re-designs the response,
// which allocates and builds FFTW plans.
void rx_audio_set_narrow_filter_min_phase(int min_phase);
int rx_audio_get_narrow_filter_min_phase(void);

// Stage 3's pitch and width, in Hz. Elliptic coefficients can't be
// designed at runtime, so the selectable values are a bank of filters
// pre-designed offline (src/narrow_filter_bank.h, generated by
// tools/gen_narrow_filters.py): six pitches, four widths, 24 sets.
// A request snaps to the nearest available value, and the setter RETURNS
// the value it actually selected - a caller asking for 725Hz gets 700 back
// and should display that, not what it asked for.
//
// Both settings apply to whichever stage-3 implementation is selected: the
// bank entry is loaded for the elliptic, and the FFT filter is retuned to
// the same passband, so switching implementations compares two filters
// aimed at the same place.
//
// Changing the pitch also moves the software BFO stage 2 mixes with, since
// that is what the operator actually hears - the filter alone would just
// detune off the tone. It does NOT touch hw_settings.ini's bfo_freq or
// where the radio transmits.
//
// PREFER radio_set_cw_pitch() (radio.h) over the pitch setter here. This
// one moves the receiver only, and a receiver whose pitch disagrees with
// cw.c's sidetone makes zero-beating by ear transmit off frequency by the
// difference. radio_set_cw_pitch() moves the sidetone and the TX IF shift
// with it and refuses mid-transmission; it calls this to do the RX half and
// to decide which pitch the request snaps to. The width setter has no such
// partner - width is receive-only - so call it directly.
//
// Not for the audio thread: selecting re-tunes the FFT filter, which
// allocates. Reached from rigctld's "l"/"L CWPITCH" (via radio.c) and
// "l"/"L CWWIDTH" (hamlib.c), i.e. on an interface thread.
int rx_audio_set_narrow_pitch(int hz);
int rx_audio_get_narrow_pitch(void);
int rx_audio_set_narrow_width(int hz);
int rx_audio_get_narrow_width(void);

// The bank's selectable values, so a client can present the real choices
// instead of hard-coding a list that could drift from the generated table.
// rx_audio_narrow_*_at() returns -1 for an out-of-range index.
int rx_audio_narrow_pitch_count(void);
int rx_audio_narrow_width_count(void);
int rx_audio_narrow_pitch_at(int index);
int rx_audio_narrow_width_at(int index);

// Which demodulator rx_audio_process() applies:
//   CW   keeps the upper side (as on most rigs) and mixes it up to the
//        selected pitch, so a station d Hz above dial is heard at
//        pitch + d and tuning up lowers its pitch.
//   USB  upper side, no BFO: audio Hz == RF - dial, the convention WSJT-X
//        and other SSB-based apps assume. Also used for DIGITAL.
//   LSB  lower side, no BFO: audio Hz == dial - RF.
//   CWR  CW-reverse - CW's BFO with LSB's side, so a station d Hz BELOW
//        dial is heard at pitch + d. Only the demodulator changes;
//        transmit is identical to CW (radio.h). CWR shares CW's BFO, so it
//        follows the pitch with no separate handling.
// Sideband selection accounts for the spectrally inverted I/Q (rx_audio.c's
// RX_IQ_SPECTRUM_INVERTED). History: docs/dsp_design_notes/
// rx_uac_out_digital_mode_bandwidth.md §10.
enum rx_demod {
    RX_DEMOD_CW = 0,
    RX_DEMOD_USB,      // also used for DIGITAL (FT8 etc. are USB by convention)
    RX_DEMOD_LSB,
    RX_DEMOD_CWR,
};

// Called by radio.c's radio_set_mode() - the one place every mode change
// (rigctld M, Kenwood MD, the control panel) already funnels through -
// so rx_audio.c itself stays free of any radio.h dependency (its test
// harnesses link without radio.c).
void rx_audio_set_demod(enum rx_demod d);
enum rx_demod rx_audio_get_demod(void);

// SPOT (auto zero-beat), the receive half. In CW and CWR, searches the
// last 1.5 s of audio for the strongest tone within the narrow filter's
// width of the pitch (300 Hz either side with the filter out) and gives
// the station's offset from the frequency being listened to, in Hz, + for
// above it. radio_spot() (radio.h) does the tuning.
//   RX_SPOT_NOT_CW     not in CW or CWR
//   RX_SPOT_NOT_YET    less than 1.5 s of audio since the last reset
//   RX_SPOT_NO_SIGNAL  no tone clearly above the noise (cw_spot.h)
// Any thread but the audio thread: it takes a few ms.
enum rx_spot_result {
    RX_SPOT_OK = 0,
    RX_SPOT_NOT_CW,
    RX_SPOT_NOT_YET,
    RX_SPOT_NO_SIGNAL,
};
enum rx_spot_result rx_audio_spot(double *offset_hz);

// Forget the audio SPOT holds, from the next block: for a retune or a
// change of what is heard. A pitch or mode change does this itself. Any
// thread.
void rx_audio_spot_reset(void);

// Demodulates one block's worth of already-mixed baseband I/Q (the same
// i_samples[]/q_samples[] sound.c's sound_process() already computes for
// hpsdr_send_iq()) into n real PCM samples ready for the codec's local
// monitor channel. Call once per audio block, same n as the I/Q block it
// was given.
//
// uac_out, if non-NULL, receives the SAME n samples one stage earlier -
// post-AGC (stage 4's makeup gain already applied) but pre-rx_volume (the
// operator's manual AF gain, applied only to out[]). It feeds the USB
// gadget's audio (WSJT-X) and TCI's receive audio, decimated to 48kHz by
// sound.c: a remote decoder's level mustn't change with the operator's
// listening volume, as a rig's line-out is independent of its volume knob,
// but it does want the AGC, which keeps the level decodable whatever the
// band. Units are the AGC's own scale (rx_audio.c's AGC_TARGET_AMPLITUDE),
// unnormalized and uncapped: each consumer scales and clamps to its own
// range (usb_gadget.c's UAC_RX_AUDIO_SCALE, tci_stream.c's
// TCI_RX_AUDIO_SCALE).
void rx_audio_process(const double *i_samples, const double *q_samples,
                       int n, int32_t *out, double *uac_out);

// Debug/test only - the AGC's current smoothed envelope estimate. It
// tracks the RAW input I/Q's magnitude, sampled before stage 1, so the
// AGC's gain doesn't depend on (and so doesn't cancel) any frequency-
// selective stage (rx_audio_demod_design.md §8.7-8.8). It is therefore
// frequency-independent - a steady tone reads the same whatever its
// frequency or side of the dial - and says nothing about stage 1's image
// rejection or stage 3's selectivity: measure rx_audio_process()'s out[]
// for those. Useful for checking the AGC's own gain math against a known
// input amplitude. Not needed by normal callers.
double rx_audio_debug_agc_envelope(void);

// Debug/test only - the METER envelope's current smoothed estimate (see
// rx_audio.c's signal-strength comment above rx_audio_get_strength_db()).
// Unlike rx_audio_debug_agc_envelope() above, this one DOES track stage
// 1's image rejection and stage 3's selectivity/bypass state, by design -
// it's tapped from narrowed (post-stage-3-or-bypass, pre-AGC-gain), not
// the raw input - so rx_audio_test.c can check that directly rather than
// through rx_audio_get_strength_db()'s rounded dB. Not needed by normal
// callers.
double rx_audio_debug_meter_envelope(void);

// Current signal-strength estimate for rigctld's "l STRENGTH" (hamlib.c) -
// a real Hamlib RIG_LEVEL, unlike NARROW above, so it's implemented in
// the standard convention real Hamlib clients expect: an integer number
// of dB relative to a nominal S9 reference (0 = S9, negative = below S9
// in 6dB/S-unit steps down toward S0, positive = "S9+N dB"). Built on the
// METER envelope (rx_audio_debug_meter_envelope() above), NOT the AGC's
// own agc_env - deliberately: this reads what's actually reaching the
// speaker (post image-rejection, post narrow-filter-or-bypass), not "how
// much energy is anywhere in the whole captured band" the way an
// agc_env-based reading would. Read-only and NOT wattmeter/signal-
// generator calibrated either way - see rx_audio.c and
// docs/dsp_design_notes/rx_gain_and_level_calibration.md §9 for what the
// reference point does and doesn't mean. No set_level equivalent, same as
// a real rig's S-meter.
int rx_audio_get_strength_db(void);

#endif /* RX_AUDIO_H */
