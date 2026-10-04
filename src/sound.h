/* sound.h - the ALSA audio thread and WM8731 codec control (sound.c). */

#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

/* Start the full-duplex ALSA audio thread (capture + playback).
   device_name is the ALSA PCM name, e.g. "hw:0,0".
   Returns 0 on success, -1 on failure. */
int  sound_thread_start(const char *device_name);

/* Stop the audio thread and release ALSA devices. */
void sound_thread_stop(void);

/* Sets an ALSA mixer element: switches on/off, volumes to make_on
   percent, enumerated controls to item make_on. */
void sound_mixer(char *card_name, char *element, int make_on);

/* Diagnostic: prints an element's actual capabilities (playback/capture
   volume and switch, enumerated) and current value(s) to stderr - a way
   to check ground truth on a given board when a control silently doesn't
   behave as expected. Not part of normal setup. */
void sound_mixer_dump(char *card_name, char *element);

/* Barebones WM8731 codec setup (input mux, capture levels, output
   routing). Call once, after the ALSA devices are otherwise ready. */
void setup_audio_codec(void);

/* Mute (enable=0) or restore (enable=1, RX_CAPTURE_GAIN_PERCENT) the LEFT
   (RX) channel of the WM8731 'Capture' gain around a TX burst, so TX
   energy can't overdrive the ADC. Called by radio.c's radio_tx_apply().
   The RIGHT (mic) channel is left alone. See
   docs/dsp_design_notes/rx_gain_and_level_calibration.md §8. */
void sound_set_rx_capture(int enable);

/* Sets the WM8731 'Master' LEFT channel only (0-100): the local
   speaker/headphone output (RX audio and TX sidetone). RIGHT is the
   exciter feed (sound_set_tx_drive()), so this never needs muting around
   TX. setup_audio_codec() calls it once; public for a future physical
   volume control. */
void sound_set_local_monitor(int percent);

/* Sets 'Master' RIGHT only (0-100) - the exciter feed. radio.c's
   radio_tx_apply() sets TX_MASTER_VOL for TX and 0 as the relay drops.
   Never touches LEFT (sound_set_local_monitor()). */
void sound_set_tx_drive(int percent);

/* Live TX mic gain for USB/LSB, on top of MIC_TX_INPUT_SCALE's fixed
   int32->float conversion (sound.c). Runtime-adjustable because mic level
   depends on the mic, preamp and operator, and setting it against a
   wattmeter takes many quick trials. Starts at 1.0; clamped to
   [0, SOUND_MIC_TX_GAIN_MAX]. Reached via rigctld's l/L MICGAIN
   (hamlib.c) and tools/rigctl_panel.py's Mic Gain slider. */
void sound_set_mic_tx_gain(double gain);
double sound_get_mic_tx_gain(void);

/* The operator's POWER setting, 0.0-1.0 of hw_settings.ini's max_power.
   It moves tx_pipeline.c's limiter ceiling, so the limiter holds whatever
   power is selected and max_power stays unreachable from here. Starts at
   1.0. Reached via rigctld's l/L RFPOWER (hamlib.c) and
   tools/rigctl_panel.py's Power slider.

   Not to be confused with sound_set_tx_drive() above, which is the
   codec's analog output - mute sequencing, not a power control. MICGAIN
   is what drives the signal into this ceiling. The whole chain:
   docs/03_tx_processing_pipeline.md, "Setting power". */
void sound_set_tx_power(double fraction);
double sound_get_tx_power(void);

/* Limiter gain reduction in dB, 0.0 when not limiting. Peak-held so it
   reads as a meter (rigctld's l ALC); the raw per-block value changes
   far faster than anyone can follow. This is the instrument for setting
   MICGAIN - a couple of dB on speech peaks is right. */
double sound_get_alc_db(void);

/* Re-derives the TX IF placement (the CW and SSB shifts) from this
   board's bfo_freq/xtal_filter_center and whatever tone cw.c is currently
   generating, so the two cancel to a key-down carrier on the dial. Called
   at startup, after every CW pitch change and after every
   xtal_filter_center change - radio_set_cw_pitch() and
   radio_set_xtal_filter_center() own those sequences, so don't call this
   on its own expecting the pitch or the center to move.
   Returns -1 if the board's values give no usable IF, leaving the previous
   placement in force. */
int sound_update_cw_if_placement(void);

/* 0 while the most recent sound_update_cw_if_placement() failed, so the
   placement in force doesn't match bfo_freq/xtal_filter_center and a
   transmission would be off frequency by the difference; 1 otherwise.
   radio_tx_allowed() refuses transmit on 0. */
int sound_tx_if_placed(void);

/* T/R timing instrumentation, on only when MAXIBITX_TR_TIMING is set.

   sound_tr_timing_enabled(): whether it is set (read once, cached).

   sound_tx_first_block(): for the transmission now starting, when the
   audio thread wrote its first TX block and when that block's first sample
   reaches the DAC as RF - the write time plus the playback queue ahead of
   it (snd_pcm_delay()) plus tx_pipeline.c's own delay, and how long that
   queue was. CLOCK_MONOTONIC nanoseconds throughout. Returns 0 if no TX block has been
   written since start-up. radio.c's TX worker compares these against the
   moment it unmutes the exciter drive:
   docs/dsp_design_notes/cw_keyer_design_study.md §8. */
int sound_tr_timing_enabled(void);
int sound_tx_first_block(int64_t *written_ns, int64_t *rf_at_dac_ns, int64_t *queue_ns);

#endif /* SOUND_H */
