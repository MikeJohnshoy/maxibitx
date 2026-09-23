// hw_settings.h
#ifndef HW_SETTINGS_H
#define HW_SETTINGS_H

void hw_settings_load(void);

// ---- Per-band TX power calibration -------------------------------------
//
// hw_settings.ini carries a repeated [tx_band] section per band, each
// with f_start/f_stop (Hz) and a 'scale' value - the exact same file,
// same section layout, same numbers real sbitx's own bands.ini-derived
// calibration (calibrate_tx_drive() in sbitx.c) writes, bench-measured
// against a real wattmeter/power bridge to flatten PA gain across HF
// (sbitx.c's own comment: "the PA gain varies across the band from 3.5
// MHz to 30 MHz, here we adjust the drive levels to keep it up, almost
// level"). Real sbitx applies it as tx_amp = tx_drive * band.scale,
// multiplied into its (very differently-scaled) FFT-domain TX samples -
// minibitx's simpler oscillator+envelope CW path can't reuse that exact
// numeric formula (the two codebases' internal sample magnitudes aren't
// in the same units), but the *relative* scale values across bands are
// real, physical, already-calibrated data for this exact board and are
// worth using instead of one flat, uncalibrated guess. See the
// TX_SAMPLE_HEADROOM comment in sound.c for how it's applied.

#define HW_MAX_TX_BANDS 16

struct tx_band_scale {
    int f_start;
    int f_stop;
    double scale;
};

extern struct tx_band_scale tx_band_scales[HW_MAX_TX_BANDS];
extern int tx_band_scale_count;

// Returns the calibrated scale for freq_hz (the [tx_band] entry whose
// f_start..f_stop contains it). Falls back to HW_DEFAULT_TX_SCALE if no
// band matches (out-of-table frequency) or none were loaded at all (no
// hw_settings.ini, or it has no [tx_band] sections).
#define HW_DEFAULT_TX_SCALE 0.00115   // 40m's calibrated value - also the
                                      // TX_SAMPLE_HEADROOM anchor in sound.c
double hw_settings_tx_scale(int freq_hz);

// 1 if freq_hz lies inside one of the loaded [tx_band] entries, so
// transmitting there is calibrated and permitted. radio_set_tx() refuses
// PTT when this is 0, which covers every PTT source at once - the key,
// rigctld, CAT and HPSDR MOX.
//
// Returns 1 for every frequency when no [tx_band] entries were loaded at
// all (no hw_settings.ini, or one with no [tx_band] sections). An empty
// table means "nothing is calibrated", not "nothing is allowed" -
// refusing everything would leave a board with no ini unable to
// transmit at all, which is a worse failure than an uncalibrated one.
int hw_settings_tx_allowed(int freq_hz);

// ---- TX power ceiling ---------------------------------------------------
//
// Two top-level watt values in hw_settings.ini, both about this board:
//
//   full_scale_power - the PEP a full-scale (amplitude 1.0) signal makes,
//                      once the [tx_band] scales above are calibrated to
//                      give that same power on every band. This board's
//                      rated output. A measurement.
//   max_power        - the ceiling tx_pipeline.c's limiter holds. Nothing
//                      may exceed it, whatever a CAT client asks for. A
//                      choice: lower it to run below rated power.
//
// sound.c turns them into the limiter's amplitude ceiling,
// sqrt(power_fraction * max_power / full_scale_power), and hands that to
// tx_pipeline_set_ceiling(). Equal values give a ceiling of 1.0, which is
// the normal setting: the radio makes rated power and the limiter holds
// it there. The limiter's working range comes from the drive side - mic
// gain and host audio levels can exceed full scale by a long way - not
// from setting max_power below rated. How to measure either:
// docs/dsp_design_notes/tx_test_tones_and_alc.md.
#define HW_DEFAULT_FULL_SCALE_POWER 5.0 // what the [tx_band] scales are
#define HW_DEFAULT_MAX_POWER 5.0        // calibrated against today

extern double tx_full_scale_power;
extern double tx_max_power;

// max_power / full_scale_power as an amplitude (the square root, since
// power goes as amplitude squared), clamped to (0, 1]. 1.0 when the two
// are equal or either is unusable.
double hw_settings_power_ratio(void);

#endif /* HW_SETTINGS_H */
