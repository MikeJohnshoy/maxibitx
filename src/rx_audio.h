// rx_audio.h
//
// Demodulates a listenable CW tone out of the receiver's own I/Q, so the
// operator can hear a signal through the box's own local audio output -
// see rx_audio.c for the design.

#ifndef RX_AUDIO_H
#define RX_AUDIO_H

#include <stdint.h>

// Which stage-3 ("single signal" selectivity) implementation is in use -
// see rx_audio_set_narrow_filter_impl() below and rx_audio.c's file
// header ("docs/ARCHITECTURE.md build order step 6/7") for what each one
// is.
enum rx_narrow_filter_impl {
	RX_NARROW_FILTER_ELLIPTIC, // 8-pole elliptic IIR from the pre-designed
	                            // bank, src/narrow_filter_bank.h (default)
	RX_NARROW_FILTER_FFT,      // the shared FFT overlap-save filter (src/rx_filter.c)
};

// Call once at startup, after vfo_init_phase_table().
void rx_audio_init(void);

// 0-100. Scales the demodulated audio before it reaches the codec - the
// local speaker/headphones only (uac_out/WSJT-X is unaffected). 100%
// maps to rx_audio.c's RX_VOLUME_MAX, not unity gain; see that constant.
// Log (audio) taper: each 1% is 0.5dB (RX_VOLUME_RANGE_DB over the
// range), and 0% is a true mute.
void rx_audio_set_volume(int percent);

// Current volume, 0-100, in the same units rx_audio_set_volume() takes -
// added for hamlib.c's "l AF" (get_level) rigctld command, so a remote
// control panel can read back the volume it didn't itself just set (e.g.
// on connect, before ever calling set_volume).
int rx_audio_get_volume(void);

// The narrow, post-demodulation "single signal" selectivity filter,
// centered on the selected CW pitch - a separate stage from the wide
// image-reject filter upstream of it (see rx_audio.c's file header and
// docs/dsp_design_notes/rx_audio_demod_design.md §7/§8 for why those two
// are deliberately independent). Originally one fixed 8-pole elliptic
// design; pitch and width are now selectable from a pre-designed bank
// (rx_audio_set_narrow_pitch()/_width() below). An earlier revision had a
// rx_audio_set_filter_bw() here that took an arbitrary bandwidth; that one
// is gone deliberately, not an oversight - see rx_audio_demod_design.md
// §8.2 for why, and note that the bank answers it differently rather than
// reviving it, by offering only widths that were designed and measured
// offline:
// the SHAPE (coefficients) was fixed. Whether the operator hears it at
// all is a different, much cheaper question - rx_audio_set_narrow_filter()
// below just switches between the filter's output and its bypass, no
// coefficient math involved, so it doesn't reopen that earlier decision.
//
// A second stage-3 implementation sits alongside the elliptic bank -
// src/rx_filter.c/.h, the same shared FFT overlap-save engine
// tx_pipeline.c uses. It tunes its passband continuously rather than
// selecting from designed-in values, and its stopband keeps descending
// where an equiripple elliptic floors out around -50dB; it pays for both
// with a slower attack on keyed CW. Both
// implementations run continuously regardless of which one is currently
// selected (same "keep it warm so switching doesn't thump" reasoning the
// enable/bypass toggle already used, just extended to cover switching
// BETWEEN implementations too, not only on/off) - see
// rx_audio_set_narrow_filter_impl() below and rx_audio.c's own comment
// on why. The elliptic bank stays the default; the FFT path is there for an
// on-air listening comparison (§10 step 7), not as a cutover in waiting -
// the bank stays whatever that comparison concludes, since it is also the
// faster-attacking of the two.

// Enable (1, the default) or bypass (0) stage 3, the narrow filter
// above. The filter itself keeps running either way (its history stays
// warm) - only which signal reaches stage 4's AGC/output changes - so
// there's no settling-time thump when toggling back on. Wired to
// rigctld's "u"/"U NARROW" (hamlib.c) so the control panel (tools/
// rigctl_panel.py) can toggle it remotely - see rx_audio.c for why this
// is a plain on/off rather than a runtime-adjustable width.
void rx_audio_set_narrow_filter(int enable);

// Current narrow-filter enable state, 0 or 1 - same "let a client read
// back what it didn't itself just set" reasoning as
// rx_audio_get_volume().
int rx_audio_get_narrow_filter(void);

// Which stage-3 implementation is selected when the narrow filter above
// is enabled: 0 = the original elliptic IIR (the default - not yet
// verified on air), 1 = the new shared FFT filter (src/rx_filter.c,
// docs/ARCHITECTURE.md step 6). Wired to rigctld's "u"/"U FFTFILT"
// (hamlib.c) for the same remote on-air A/B comparison this exists for -
// see rx_audio.c's rx_audio_process() for how both implementations stay
// warm regardless of which is selected.
void rx_audio_set_narrow_filter_impl(int use_fft);

// Current implementation selection, 0 or 1 (same meaning as
// rx_audio_set_narrow_filter_impl()'s argument) - same readback
// reasoning as rx_audio_get_narrow_filter().
int rx_audio_get_narrow_filter_impl(void);

// Which realization the FFT implementation above uses: 1 = minimum phase
// (the default), 0 = linear phase. Same passband either way; what changes
// is the time domain, and on CW that difference is large - a keyed
// element reaches -3dB of its settled level in 8.7ms instead of 20.3ms,
// and the filter's group delay drops from 16.0ms to 4.5ms
// (rx_filter_test.c Case E). That attack, not the magnitude response, is
// what made the FFT filter sound duller than the elliptic despite
// measuring better on the bench - see
// docs/dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md.
//
// Minimum phase is the default because a narrow filter centered on the CW
// pitch exists to hear CW through. Linear phase stays reachable so
// the two can be compared on air, and because the step 6 bench numbers
// were all measured against it. No effect while the elliptic
// implementation is selected.
//
// Not for the audio thread: switching re-designs the response, which
// allocates and builds FFTW plans. Wired to rigctld's "u"/"U MINPHASE"
// (hamlib.c), i.e. called on an interface thread.
void rx_audio_set_narrow_filter_min_phase(int min_phase);

// Current realization, 0 or 1 (same meaning as the setter's argument) -
// same readback reasoning as rx_audio_get_narrow_filter().
int rx_audio_get_narrow_filter_min_phase(void);

// Stage 3's pitch and width, in Hz. Elliptic coefficients can't be
// designed at runtime, so the selectable values are a bank of filters
// pre-designed offline (src/narrow_filter_bank.h, generated by
// tools/gen_narrow_filters.py): three pitches, four widths, twelve sets.
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

// Demodulates one block's worth of already-mixed baseband I/Q (the same
// i_samples[]/q_samples[] sound.c's sound_process() already computes for
// hpsdr_send_iq()) into n real PCM samples ready for the codec's local
// monitor channel. Call once per audio block, same n as the I/Q block it
// was given.
//
// uac_out, if non-NULL, receives the SAME n samples one stage earlier -
// post-AGC (stage 4's makeup gain already applied, so it doesn't ride the
// input's own raw amplitude swings) but pre-rx_volume (the operator's
// manual AF gain, applied only to out[] below). First use:
// usb_gadget.c's WSJT-X audio bridge (docs/ARCHITECTURE.md) - decoded
// audio sent to a fixed remote consumer must not silently change level
// every time the operator touches their own listening volume, the same
// reasoning a real rig's "line out"/ACC jack is independent of its front-
// panel volume knob. Deliberately NOT also independent of the AGC:
// AGC is signal-conditioning (keeps the level in a decodable range
// regardless of band conditions), not an operator loudness preference -
// a remote decoder wants that, the same way it wants stage 1/3's
// filtering. Units match narrowed*gain's own natural scale (see
// rx_audio.c's AGC_TARGET_AMPLITUDE) - unnormalized, uncapped to int32 -
// deliberately: this is a different consumer with a different target
// range (16-bit PCM, not the codec's int32 convention out[] uses), so
// scaling/clamping into that range is usb_gadget.c's own job (its
// UAC_RX_AUDIO_SCALE), not something baked in here.
void rx_audio_process(const double *i_samples, const double *q_samples,
                       int n, int32_t *out, double *uac_out);

// Debug/test only - the AGC's current smoothed envelope estimate (see
// rx_audio.c). Per rx_audio_demod_design.md §8.7-8.8, this tracks the RAW input I/Q's
// magnitude, sampled before stage 1 even runs - deliberately moved there
// (via an intermediate stop at stage 2's output, which had its own
// problem - see the file header) so the AGC's makeup gain no longer
// depends on any frequency-selective stage's own behavior, and so no
// longer cancels out any of it in the final PCM output. That also means
// this envelope is frequency-independent by construction (a steady tone
// reads the same magnitude regardless of its frequency or which side of
// dial center it's on) - it is NOT a window into any single stage's
// selectivity any more, not stage 1's image rejection and not stage 3's
// narrow filter. Measure the actual PCM output (rms/peak of
// rx_audio_process()'s out[] array) for either of those instead, now
// that the AGC no longer erases them. What this envelope IS still useful
// for: confirming the AGC's own target-amplitude/gain math against a
// known input amplitude (test_rx_audio.c's cases A/B/C). Not needed by
// normal callers.
double rx_audio_debug_agc_envelope(void);

// Debug/test only - the METER envelope's current smoothed estimate (see
// rx_audio.c's signal-strength comment above rx_audio_get_strength_db()).
// Unlike rx_audio_debug_agc_envelope() above, this one DOES track stage
// 1's image rejection and stage 3's selectivity/bypass state, by design -
// it's tapped from narrowed (post-stage-3-or-bypass, pre-AGC-gain), not
// the raw input. Exists mainly so test_rx_audio.c can check that
// distinction directly rather than only indirectly through
// rx_audio_get_strength_db()'s rounded dB output. Not needed by normal
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
// docs/dsp_design_notes/rx_gain_and_level_calibration.md §9 for exactly
// what the reference point does and doesn't mean, and for the earlier,
// wideband version this replaced. No set_level equivalent, same as a
// real rig's S-meter.
int rx_audio_get_strength_db(void);

#endif /* RX_AUDIO_H */
