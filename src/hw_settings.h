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

#endif /* HW_SETTINGS_H */
