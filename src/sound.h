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

#endif /* SOUND_H */
