// sound.c
// Minimal ALSA full-duplex driver for minibitx.

#include "antialias.h"
#include "cw.h"
#include "decim48k.h"
#include "hpsdr_p1.h"
#include "hw_settings.h"
#include "iq_stream.h"
#include "radio.h"
#include "rx_audio.h"
#include "sound.h"
#include "tx_pipeline.h"
#include "usb_gadget.h"
#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */
#define SAMPLE_RATE 96000
#define CHANNELS 2         /* stereo: L = RX / R = Mic (capture) */
#define PERIOD_FRAMES 1024 /* frames per period (matches old cfg) */
#define MAX_FRAMES 4096

// WM8731 "Line" input path is a plain on/off switch, not a gain control
// (bench-confirmed: `amixer -c 0 sget 'Line'` shows `Capabilities:
// cswitch` only) - kept as a named constant so it reads as a deliberate
// boolean rather than a stray literal. RX_CAPTURE_GAIN_PERCENT below is
// the real analog gain stage.
#define RX_LINE_INPUT_ON 1

// WM8731 'Capture' - the real analog gain stage ahead of the ADC (no RF
// preamp anywhere in this RX chain). sound_mixer() maps this ALSA
// percent (0-100) onto the control's real 0-31 raw range
// (`percent * 31 / 100`). 70 (raw step 21, ~-3.0dB) is a bench-derived
// choice, not yet a final calibration - see
// docs/dsp_design_notes/rx_gain_and_level_calibration.md for the data
// behind it, and rx_clip_check() below for the ongoing safety net if
// some future signal ever proves it too hot.
#define RX_CAPTURE_GAIN_PERCENT 70

// WM8731 'Mic' - the capture gain ahead of the R channel's mic input
// (mic_buf, audio_loop() below), needed now that USB/LSB TX actually
// reads it (see the cw_tx_active() branch below and MIC_TX_INPUT_SCALE
// above) - previously left at 0 (muted) since nothing consumed it.
// Picked as a plain starting midpoint, the same way RX_CAPTURE_GAIN_PERCENT
// started at a bench guess before real-hardware listening refined it;
// unlike "Line" above, 'Mic' hasn't been bench-confirmed yet to even be
// a real gain control rather than a switch (see RX_LINE_INPUT_ON's
// comment for that exact gotcha on a different control) - worth an
// `amixer -c 0 sget 'Mic'` check before trusting this number.
#define MIC_CAPTURE_GAIN_PERCENT 50

// WM8731 'Master' is a stereo control with independent L/R volume
// registers (confirmed both from the sbitx hardware docs and from
// sound_mixer_dump()'s readback) - and L/R genuinely go to two different
// physical destinations here, not two speakers of the same signal:
//   L (FRONT_LEFT):  local speaker/headphone audio amp - cw.c's TX
//                     sidetone and rx_audio.c's RX demod.
//   R (FRONT_RIGHT): the mainboard's diode mixer, which upconverts the
//                     DSP's low-IF TX carrier for the actual RF chain -
//                     see TX_MASTER_VOL below.
// There is no reason these should ever have shared one gain value, which
// is exactly the bug the old shared, single-value "Master" control had:
// every TX/RX transition clobbered whichever of these two purposes
// wasn't currently active. LOCAL_SPEAKER_GAIN_PERCENT only ever governs
// the L channel now, set once at startup (see setup_audio_codec()) and
// never touched again - not muted/restored around TX like the R channel
// legitimately needs to be (see sound_set_tx_drive() and radio.c's
// TX_MASTER_VOL), and NOT touched by rx_audio.c's rx_volume/the CAT `AG`
// command either - that's a separate, purely digital multiplier applied
// long before this analog stage (see rx_audio.c's rx_volume), so "100%"
// on that control has never meant "as loud as this codec output can go".
//
// Raised from the original 70 to 100 (2026-09, on-air report: "100%
// volume" still too quiet) - safe to run wide open here because it's
// pure analog output gain sitting downstream of rx_audio.c's own
// digital headroom (AGC_TARGET_AMPLITUDE rides at only ~25% of the
// int32 clamp specifically so peaks don't clip - see rx_audio.c) -
// removing attenuation at this stage doesn't touch that margin at all.
// If 100 still isn't loud enough on real hardware, the next lever is
// rx_audio.c's AGC_TARGET_AMPLITUDE itself (raising it trades away some
// of that peak headroom, so it needs on-air listening for clipping, not
// just a bench check).
#define LOCAL_SPEAKER_GAIN_PERCENT 100

/* ------------------------------------------------------------------ */
/*  TX sample scaling - see docs/03_tx_processing_pipeline.md          */
/*  "Adjusting power levels" for what each constant means and the      */
/*  bench data behind its value; hw_settings.h for the per-band        */
/*  'scale' calibration table TX_SAMPLE_HEADROOM is anchored against   */
/* ------------------------------------------------------------------ */
#define TX_DRIVE 50
#define TX_SAMPLE_HEADROOM (1000000000.0 / (TX_DRIVE * HW_DEFAULT_TX_SCALE))
#define TX_SAMPLE_CLAMP 2000000000.0 // stay well inside int32 range
#define TX_GAIN_CORRECTION 0.045
// Fixed sidetone PCM peak (left channel only - never reaches the PA),
// deliberately independent of TX_GAIN_CORRECTION - see
// docs/03_tx_processing_pipeline.md for why that coupling used to bite.
#define SIDETONE_PEAK_AMPLITUDE 10000000.0

// Converts a raw S32_LE mic capture sample (mic_buf[i], full int32
// range) into tx_pipeline.c's expected roughly-[-1,1] input range, for
// USB/LSB TX (see the cw_tx_active() branch below) - the mic-audio
// counterpart to cw.c's cw_get_sample(), which is already exactly
// [-1,1] by construction since it's a synthetic tone. Unlike that
// tone, real mic level depends on the physical mic, the analog gain
// ahead of the ADC, and how hard the operator talks - this maps
// full-scale straight to 1.0 as a fixed, purely mechanical unit
// conversion, NOT itself a tunable gain (that's mic_tx_gain below,
// same "keep the unit conversion and the operator-adjustable gain as
// two separate things" split rx_audio.c's rx_volume/AGC already use).
#define MIC_TX_INPUT_SCALE (1.0 / 2147483648.0)

// mic_tx_gain: live multiplier on top of MIC_TX_INPUT_SCALE above -
// see sound_set_mic_tx_gain()'s comment (sound.h) for why this is a
// runtime control rather than another #define needing a rebuild per
// trial. Plain double, no lock: written from rigctld's connection
// thread (hamlib.c's L MICGAIN), read once per TX audio block from the
// audio thread - the exact same "eventually consistent is fine for a
// human-timescale control knob" convention rx_audio.c's rx_volume
// already relies on (see its own comment), not a data race that
// matters at these update rates.
#define SOUND_MIC_TX_GAIN_MAX 64.0
static double mic_tx_gain = 1.0;

void sound_set_mic_tx_gain(double gain) {
  if (gain < 0.0) gain = 0.0;
  if (gain > SOUND_MIC_TX_GAIN_MAX) gain = SOUND_MIC_TX_GAIN_MAX;
  mic_tx_gain = gain;
}

double sound_get_mic_tx_gain(void) {
  return mic_tx_gain;
}

/* ------------------------------------------------------------------ */
/*  Module state                                                      */
/* ------------------------------------------------------------------ */
static snd_pcm_t *pcm_capture = NULL;
static snd_pcm_t *pcm_playback = NULL;
static pthread_t audio_thread;
static volatile int g_running = 0;

// The shared FFT TX pipeline (tx_pipeline.c) - one persistent instance,
// created once at sound_thread_start() and freed at sound_thread_stop(),
// same lifetime pattern as pcm_capture/pcm_playback above. Owns real
// FFTW plans (FFTW_MEASURE - a real one-time setup cost, see
// tx_pipeline.c/fft_filter.c's own comments on why), so it must not be
// created/destroyed per TX burst. CW and USB/LSB now share this one
// instance (docs/ARCHITECTURE.md build order step 8) - same passband
// filter either way (tx_pipeline.c's filter_tune() call has no
// sideband-dependent term), only the `i_sample` source (cw.c's tone vs
// real mic audio) and the sideband-zero direction passed to
// tx_pipeline_process_block() differ per audio_loop()'s TX branch
// below. DIGITAL's externally-generated audio is still future work.
static struct tx_pipeline *cw_tx_pipeline = NULL;

/* ------------------------------------------------------------------ */
/*  ALSA mixer helper                                                 */
/* ------------------------------------------------------------------ */
void sound_mixer(char *card_name, char *element, int make_on) {
  long min, max;
  snd_mixer_t *handle;
  snd_mixer_selem_id_t *sid;

  snd_mixer_open(&handle, 0);
  snd_mixer_attach(handle, card_name);
  snd_mixer_selem_register(handle, NULL, NULL);
  snd_mixer_load(handle);

  snd_mixer_selem_id_alloca(&sid);
  snd_mixer_selem_id_set_index(sid, 0);
  snd_mixer_selem_id_set_name(sid, element);
  snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);

  if (!elem) {
    // Silent no-op used to hide a real class of bug: a misspelled or
    // absent element name here means every call below is a no-op that
    // *looks* like it succeeded - see sound_mixer_dump() below for a
    // way to actually check what an element supports on a given board.
    fprintf(stderr,
            "sound_mixer: '%s' not found on %s (check `amixer -c 0 scontrols`)\n",
            element, card_name);
    snd_mixer_close(handle);
    return;
  }

  // Independent ifs, not else-if: a single ALSA "simple" element can
  // combine a mute switch AND a volume control - true of most
  // headphone/speaker outputs (e.g. "Master" here), which pairs a
  // "Playback Volume" register with a separate mute bit under one
  // simple-mixer name. Treating switch and volume as mutually exclusive
  // (the previous else-if chain) meant an element with both only ever
  // got its switch toggled - the volume register was silently never
  // touched, left wherever the codec's power-on reset put it, no matter
  // what percent was asked for. Every capability the element actually
  // has now gets set.
  if (snd_mixer_selem_has_playback_switch(elem))
    snd_mixer_selem_set_playback_switch_all(elem, make_on != 0);
  if (snd_mixer_selem_has_capture_switch(elem))
    snd_mixer_selem_set_capture_switch_all(elem, make_on != 0);
  if (snd_mixer_selem_has_playback_volume(elem)) {
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    snd_mixer_selem_set_playback_volume_all(elem, make_on * max / 100);
  }
  if (snd_mixer_selem_has_capture_volume(elem)) {
    snd_mixer_selem_get_capture_volume_range(elem, &min, &max);
    snd_mixer_selem_set_capture_volume_all(elem, make_on * max / 100);
  }
  if (snd_mixer_selem_is_enumerated(elem))
    snd_mixer_selem_set_enum_item(elem, 0, make_on);

  snd_mixer_close(handle);
}

// One-shot diagnostic dump of a mixer element's actual capabilities and
// current value(s) - not part of the normal control-setting path above,
// just a way to print ground truth about what a given ALSA element
// really supports on this specific board/kernel, since a wrong name or
// an unexpected capability combination otherwise fails (or half-
// succeeds) silently. Called once after setup_audio_codec() below for
// "Master" - safe to call anywhere else too, e.g. from a debugging
// session, since it opens/closes its own mixer handle each time.
void sound_mixer_dump(char *card_name, char *element) {
  snd_mixer_t *handle;
  snd_mixer_selem_id_t *sid;

  snd_mixer_open(&handle, 0);
  snd_mixer_attach(handle, card_name);
  snd_mixer_selem_register(handle, NULL, NULL);
  snd_mixer_load(handle);

  snd_mixer_selem_id_alloca(&sid);
  snd_mixer_selem_id_set_index(sid, 0);
  snd_mixer_selem_id_set_name(sid, element);
  snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);

  if (!elem) {
    fprintf(stderr, "sound_mixer_dump: '%s' not found on %s\n", element, card_name);
    snd_mixer_close(handle);
    return;
  }

  fprintf(stderr,
          "sound_mixer_dump: %s/%s - playback_volume=%d playback_switch=%d "
          "capture_volume=%d capture_switch=%d enumerated=%d\n",
          card_name, element, snd_mixer_selem_has_playback_volume(elem),
          snd_mixer_selem_has_playback_switch(elem),
          snd_mixer_selem_has_capture_volume(elem),
          snd_mixer_selem_has_capture_switch(elem),
          snd_mixer_selem_is_enumerated(elem));

  if (snd_mixer_selem_has_playback_volume(elem)) {
    long min, max, val_l = -1, val_r = -1;
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &val_l);
    snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT, &val_r);
    fprintf(stderr, "  playback volume: L=%ld R=%ld (range %ld-%ld)\n",
            val_l, val_r, min, max);
  }
  if (snd_mixer_selem_has_playback_switch(elem)) {
    int val = -1;
    snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &val);
    fprintf(stderr, "  playback switch: %s\n", val ? "on" : "off");
  }

  snd_mixer_close(handle);
}

// Sets one stereo channel of a playback-volume element independently -
// unlike sound_mixer()'s *_all() calls, which drive L and R together.
// "Master" is the reason this exists: its L and R outputs feed two
// completely different physical destinations (see LOCAL_SPEAKER_GAIN_
// PERCENT's comment above), so they need independent gain, not a shared
// one. Falls back to doing nothing (with a warning) if the element turns
// out not to have a playback volume at all - see sound_mixer_dump() to
// check that assumption on a given board.
static void sound_mixer_channel(char *card_name, char *element,
                                 snd_mixer_selem_channel_id_t channel,
                                 int percent) {
  long min, max;
  snd_mixer_t *handle;
  snd_mixer_selem_id_t *sid;

  snd_mixer_open(&handle, 0);
  snd_mixer_attach(handle, card_name);
  snd_mixer_selem_register(handle, NULL, NULL);
  snd_mixer_load(handle);

  snd_mixer_selem_id_alloca(&sid);
  snd_mixer_selem_id_set_index(sid, 0);
  snd_mixer_selem_id_set_name(sid, element);
  snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);

  if (!elem) {
    fprintf(stderr, "sound_mixer_channel: '%s' not found on %s\n", element, card_name);
    snd_mixer_close(handle);
    return;
  }

  if (snd_mixer_selem_has_playback_volume(elem)) {
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    snd_mixer_selem_set_playback_volume(elem, channel, percent * max / 100);
  } else {
    fprintf(stderr,
            "sound_mixer_channel: '%s' on %s has no playback volume - "
            "per-channel level not applied\n",
            element, card_name);
  }

  snd_mixer_close(handle);
}

// Same per-channel idea as sound_mixer_channel() above, but for a
// CAPTURE volume - needed now that "Capture" feeds two genuinely
// different destinations depending on channel (rx_buf/L vs mic_buf/R -
// see sound_set_rx_capture()'s comment below), the exact same reason
// "Master" needed a playback-side per-channel split already. Logs (and
// otherwise no-ops) if this element rejects an asymmetric per-channel
// write - e.g. a single shared/ganged capture register that ALSA
// exposes as stereo but doesn't actually let differ per channel - since
// that would silently defeat the whole point of calling this instead of
// sound_mixer()'s plain *_all() version.
static void sound_mixer_capture_channel(char *card_name, char *element,
                                          snd_mixer_selem_channel_id_t channel,
                                          int percent) {
  long min, max;
  snd_mixer_t *handle;
  snd_mixer_selem_id_t *sid;

  snd_mixer_open(&handle, 0);
  snd_mixer_attach(handle, card_name);
  snd_mixer_selem_register(handle, NULL, NULL);
  snd_mixer_load(handle);

  snd_mixer_selem_id_alloca(&sid);
  snd_mixer_selem_id_set_index(sid, 0);
  snd_mixer_selem_id_set_name(sid, element);
  snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);

  if (!elem) {
    fprintf(stderr, "sound_mixer_capture_channel: '%s' not found on %s\n", element, card_name);
    snd_mixer_close(handle);
    return;
  }

  if (snd_mixer_selem_has_capture_volume(elem)) {
    snd_mixer_selem_get_capture_volume_range(elem, &min, &max);
    int err = snd_mixer_selem_set_capture_volume(elem, channel, percent * max / 100);
    if (err < 0) {
      fprintf(stderr,
              "sound_mixer_capture_channel: '%s' on %s rejected a per-channel "
              "capture write (%s) - this control may be a single shared/ganged "
              "register rather than independent L/R; check `amixer -c 0 sget "
              "'%s'` before trusting per-channel capture behavior here\n",
              element, card_name, snd_strerror(err), element);
    }
  } else {
    fprintf(stderr,
            "sound_mixer_capture_channel: '%s' on %s has no capture volume - "
            "per-channel level not applied\n",
            element, card_name);
  }

  snd_mixer_close(handle);
}

/* ------------------------------------------------------------------ */
/*  Codec hardware setup - barebones WM8731 init                      */
/* ------------------------------------------------------------------ */
void setup_audio_codec(void) {
  sound_mixer("hw:0", "Input Mux",
              0); // 'Line In' (bench-confirmed - see RX_CAPTURE_GAIN_PERCENT's comment above)
  sound_mixer("hw:0", "Line", RX_LINE_INPUT_ON); // just un-mutes the line path - see comment above
  sound_mixer("hw:0", "Capture", RX_CAPTURE_GAIN_PERCENT); // both channels' starting gain - see
                                                            // sound_set_rx_capture()'s comment for
                                                            // why only L gets touched from here on
  sound_mixer("hw:0", "Mic", MIC_CAPTURE_GAIN_PERCENT); // mic input gain - see its comment above.
                                                         // Genuinely unconfirmed whether this does
                                                         // anything real on this board at all -
                                                         // 'Input Mux' is locked to 'Line' (above),
                                                         // and if that means the codec's OWN internal
                                                         // mic preamp/mux path is never connected to
                                                         // the ADC in the first place (common for a
                                                         // single-mux codec input stage), this
                                                         // control could be entirely inert - see
                                                         // sound_set_rx_capture()'s comment for why
                                                         // 'Capture' (not 'Mic') looks like the more
                                                         // likely real point of control for mic level
                                                         // on this specific board.

  // "Master" L/R are independent - see LOCAL_SPEAKER_GAIN_PERCENT's
  // comment above. L (local speaker/headphone) is set once, here, for
  // good - nothing in the TX path touches it again, though
  // sound_set_local_monitor() below is there for a future real volume
  // control. R (the exciter feed) starts muted; radio.c's
  // radio_tx_apply() is the only thing that ever raises it, only for the
  // duration of an actual TX burst - see sound_set_tx_drive() below.
  sound_set_local_monitor(LOCAL_SPEAKER_GAIN_PERCENT);
  sound_mixer_channel("hw:0", "Master", SND_MIXER_SCHN_FRONT_RIGHT, 0);

  sound_mixer("hw:0", "Output Mixer HiFi", 1);
  sound_mixer("hw:0", "Output Mixer Line Bypass", 0);
  sound_mixer("hw:0", "Output Mixer Mic Sidetone", 0);
}

// Mute/restore the WM8731 'Capture' gain around a TX burst - called
// from radio.c's radio_tx_apply() on every TX/RX transition. See
// docs/dsp_design_notes/rx_gain_and_level_calibration.md §8 for why
// (protects the ADC from whatever bleeds into RX during TX) and the
// exact ordering this depends on.
//
// LEFT (RX/Line-in) channel only, as of docs/ARCHITECTURE.md build
// order step 8's first real SSB test - deliberately NOT the whole
// element any more. Before this, this function called plain
// sound_mixer("hw:0", "Capture", ...), which uses *_all() and writes
// the identical value to BOTH channels - harmless while "Capture" only
// ever fed rx_buf/L, but step 8 made mic_buf/R a second, genuine
// consumer of this exact same ALSA element (the board almost certainly
// routes the physical microphone onto the codec's Line-In-RIGHT pin
// rather than through the codec's own separate internal mic preamp -
// 'Input Mux' being bench-confirmed locked to 'Line', not 'Mic', is
// what points at this - see setup_audio_codec()'s 'Mic' comment).
// Left unfixed, every single TX burst zeroed mic_buf's real analog
// level the instant it started (radio_tx_apply() calls this before
// PTT/the relay/either clock), silently defeating the entire USB/LSB
// mic path no matter what MIC_TX_INPUT_SCALE or MIC_CAPTURE_GAIN_PERCENT
// were set to - a real, first-on-air-test bug (ARCHITECTURE.md §10 step
// 8's on-air follow-up), not a hypothetical. RIGHT (mic) is now left
// alone through every TX/RX transition, in every mode - harmless for CW
// (which never reads mic_buf), and exactly what USB/LSB need. Depends on
// this ALSA element actually supporting independent per-channel capture
// volume rather than a single shared/ganged register -
// sound_mixer_capture_channel() logs a warning if a per-channel write is
// rejected, which would mean this needs a different fix (e.g. skipping
// the mute entirely in USB/LSB) instead.
void sound_set_rx_capture(int enable) {
  sound_mixer_capture_channel("hw:0", "Capture", SND_MIXER_SCHN_FRONT_LEFT,
                               enable ? RX_CAPTURE_GAIN_PERCENT : 0);
}

// Sets "Master"'s LEFT channel only - the local speaker/headphone output
// (see LOCAL_SPEAKER_GAIN_PERCENT's comment above). setup_audio_codec()
// calls this once at startup and nothing else calls it today; exposed as
// a real percent-taking function (not just an on/off enable) so a future
// physical volume control has a natural place to plug in, without
// needing to touch anything TX-related.
void sound_set_local_monitor(int percent) {
  sound_mixer_channel("hw:0", "Master", SND_MIXER_SCHN_FRONT_LEFT, percent);
}

// Sets "Master"'s RIGHT channel only - the exciter feed (see
// LOCAL_SPEAKER_GAIN_PERCENT's comment above for why this is the right
// channel and not the whole control). Called from radio.c's
// radio_tx_apply() with TX_MASTER_VOL while transmitting and 0 as the
// relay drops. Deliberately leaves the LEFT channel (local speaker/
// headphone - cw.c's sidetone, rx_audio.c's RX demod) completely alone;
// unlike the old shared-"Master" design, there's no restore-after-TX
// step needed here because nothing about TX ever touches it in the
// first place.
void sound_set_tx_drive(int percent) {
  sound_mixer_channel("hw:0", "Master", SND_MIXER_SCHN_FRONT_RIGHT, percent);
}

/* ------------------------------------------------------------------ */
/*  ALSA PCM helpers                                                  */
/* ------------------------------------------------------------------ */
static snd_pcm_t *open_pcm(const char *dev, snd_pcm_stream_t dir) {
  snd_pcm_t *pcm = NULL;
  int err;

  if ((err = snd_pcm_open(&pcm, dev, dir, 0)) < 0) {
    fprintf(stderr, "sound: cannot open %s (%s): %s\n", dev,
            dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", snd_strerror(err));
    return NULL;
  }

  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(pcm, hw);

  snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
  snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS);

  unsigned int rate = SAMPLE_RATE;
  snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);

  snd_pcm_uframes_t period = PERIOD_FRAMES;
  snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);

  /* 4 periods ~ 85 ms buffer - enough headroom for a Pi */
  snd_pcm_uframes_t buffer = period * 4;
  snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);

  if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
    fprintf(stderr, "sound: hw_params failed (%s): %s\n",
            dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", snd_strerror(err));
    snd_pcm_close(pcm);
    return NULL;
  }

  printf("sound: opened %s %s @ %u Hz, period %lu, buffer %lu\n", dev,
         dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", rate, (unsigned long)period,
         (unsigned long)buffer);

  return pcm;
}

/* Recover from an ALSA xrun/suspend. Returns 0 on success. Both call
 * sites must check this return value and stop retrying if it's still
 * negative - see docs/08_troubleshooting_and_bringup.md for the retry-
 * storm bug that taught us that, and why a plain prepare() isn't
 * always enough (the drop()+prepare() fallback below is for that). */
static int xrun_recover(snd_pcm_t *pcm, int err) {
  if (err == -EPIPE) { /* underrun / overrun */
    err = snd_pcm_prepare(pcm);
    if (err < 0) {
      snd_pcm_drop(pcm);
      err = snd_pcm_prepare(pcm);
    }
  } else if (err == -ESTRPIPE) { /* suspended */
    while ((err = snd_pcm_resume(pcm)) == -EAGAIN)
      usleep(10000);
    if (err < 0)
      err = snd_pcm_prepare(pcm);
  }
  return err;
}

/* ------------------------------------------------------------------ */
/*  xrun flood tracking                                                */
/* ------------------------------------------------------------------ */
//
// Rate-limits "xrun, recovering" logging and adds a one-time hint plus a
// short breather once a flood is detected - a flood here means the
// device itself isn't stuck (recovery keeps "succeeding"), just that an
// ordinary-priority audio thread can't keep up with real time. See
// docs/08_troubleshooting_and_bringup.md "Audio thread xruns" for the
// full failure mode and the SCHED_FIFO fix.
#define XRUN_FLOOD_WINDOW_NS 1000000000L /* 1 second */
#define XRUN_FLOOD_THRESHOLD 10          /* xruns within the window = "flooding" */

struct xrun_tracker {
  int count;
  struct timespec window_start;
  int hint_shown;
};

static void xrun_note(struct xrun_tracker *t, const char *label) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  long elapsed_ns = (now.tv_sec - t->window_start.tv_sec) * 1000000000L +
                    (now.tv_nsec - t->window_start.tv_nsec);
  if (t->count == 0 || elapsed_ns > XRUN_FLOOD_WINDOW_NS || elapsed_ns < 0) {
    t->window_start = now;
    t->count = 0;
  }
  t->count++;

  if (t->count <= XRUN_FLOOD_THRESHOLD) {
    fprintf(stderr, "sound: xrun, recovering (%s)\n", label);
    return;
  }

  if (!t->hint_shown) {
    fprintf(stderr,
            "sound: xrun flood on %s (>%d/sec) - each individual recovery "
            "is succeeding, so the device isn't stuck; the audio thread "
            "simply isn't keeping up with real time. If the startup log "
            "showed \"failed to set audio thread to SCHED_FIFO\", that is "
            "almost certainly why - grant real-time scheduling, e.g. "
            "'sudo setcap cap_sys_nice+ep ./minibitx', run as root, or "
            "raise the rtprio limit for this user via "
            "/etc/security/limits.d. Backing off and continuing to retry "
            "rather than spinning at full rate.\n",
            label, XRUN_FLOOD_THRESHOLD);
    t->hint_shown = 1;
  }
  usleep(20000); /* breather so this loop isn't itself pegging a core */
}

/* ------------------------------------------------------------------ */
/*  IQ mixing                                                         */
/* ------------------------------------------------------------------ */
// Permanent, always-compiled clip guard on the raw ADC sample ("rf"
// below, the one signal that reflects RX_CAPTURE_GAIN_PERCENT directly)
// - successor to a temporary bench diagnostic, see
// docs/dsp_design_notes/rx_gain_and_level_calibration.md §6-7 for that
// history and why this checks only for the rising edge of a clip
// (nothing periodic). One fabs() and one compare per sample - negligible
// next to the mixing/FIR-filter arithmetic already done below.
static int rx_clipping = 0;

static void rx_clip_check(double rf) {
  int clipped_now = fabs(rf) >= 0.999;
  if (clipped_now && !rx_clipping) {
    fprintf(stderr,
            "sound: *** CLIPPING *** freq=%d capture=%d%% - RF front "
            "end is overdriving the ADC, consider lowering "
            "RX_CAPTURE_GAIN_PERCENT\n",
            freq_hdr, RX_CAPTURE_GAIN_PERCENT);
  }
  rx_clipping = clipped_now;
}

static void sound_process(int32_t *input_rx, int32_t *input_mic, int32_t *output_speaker,
                          int32_t *output_tx, int n_samples) {
  static double i_samples[4096];
  static double q_samples[4096];
  static int vfo_ready = 0;
  // filter I and Q with independent history per rail, same coefficients (antialias.c) -
  // zero-initialized once, persists across calls (each call is one
  // ~10.7ms block, not a fresh signal).
  static struct antialias_state aa_i;
  static struct antialias_state aa_q;
  // 96kHz->48kHz decimation for usb_gadget.c's UAC2 gadget only (see
  // docs/dsp_design_notes/usb_uac_decimation_design.md) - independent
  // history/phase per rail, same as aa_i/aa_q above. hpsdr_p1.c keeps
  // getting native 96kHz IQ unchanged; only the USB path is decimated.
  static struct decim48k_state dec_i;
  static struct decim48k_state dec_q;

  (void)input_mic;
  if (n_samples > 4096)
    n_samples = 4096;

  if (!vfo_ready) {
    vfo_init_phase_table();
    // this init only matters if sound_process() is ever called before
    // main()'s own startup vfo_start()/radio_tune_to()
    vfo_start(&lo, RX_IF_FREQ_HZ, 0);
    vfo_ready = 1;
  }

  for (int n = 0; n < n_samples; n++) {
    int32_t s = input_rx[n];
    int lo_i, lo_q;
    vfo_read_iq(&lo, &lo_i, &lo_q);

    double rf = (double)s / 2147483648.0;
    rx_clip_check(rf);

    // mix to IQ
    i_samples[n] = rf * ((double)lo_i / 1073741824.0);
    q_samples[n] = rf * ((double)lo_q / 1073741824.0);

    // Anti-alias lowpass, applied to I and Q right after mixing (see
    // docs/dsp_design_notes/antialias_filter_design.md). Helps clean up
    // the self-image near the +-48kHz Nyquist edges, without touching the
    // real signal content well within the crystal filter's passband.
    i_samples[n] = antialias_apply(&aa_i, i_samples[n]);
    q_samples[n] = antialias_apply(&aa_q, q_samples[n]);
  }

  // hand the block's IQ to each consumer as its own copy - hpsdr_p1.c
  // (network), usb_gadget.c (USB Audio Class gadget), and iq_stream.c
  // (the lightweight multi-subscriber telemetry stream - see its own
  // file header) don't know about each other, and any subset of them
  // can be active without the others
  hpsdr_send_iq(i_samples, q_samples, n_samples);
  iq_stream_send(i_samples, q_samples, n_samples);

  // usb_gadget.c's UAC2 gadget is fixed at 48kHz (matches real UAC2
  // hosts like the QMX/Tab5 panadapter this was built to interoperate
  // with - see docs/dsp_design_notes/usb_uac_decimation_design.md),
  // but this block's i_samples[]/q_samples[] are still native 96kHz -
  // decim48k_apply() only emits a kept sample on every other call, so
  // uac_push_iq() is only called when both rails have one ready
  // (they always agree, since both are fed in lockstep every n here).
  for (int n = 0; n < n_samples; n++) {
    double out_i, out_q;
    int have_i = decim48k_apply(&dec_i, i_samples[n], &out_i);
    int have_q = decim48k_apply(&dec_q, q_samples[n], &out_q);
    if (have_i && have_q)
      uac_push_iq(out_i, out_q);
  }

  // output_speaker carries the RX audio demod (rx_audio.c) - the
  // receiver's own I/Q turned into an audible CW tone. output_tx
  // stays silent here; it's only driven by the CW sidetone/TX-IF
  // chain in audio_loop() below.
  rx_audio_process(i_samples, q_samples, n_samples, output_speaker);
  memset(output_tx, 0, n_samples * sizeof(int32_t));
}

/* ------------------------------------------------------------------ */
/*  Per-block compute timing                                          */
/* ------------------------------------------------------------------ */
//
// Wraps sound_process() (mixing/antialias/decim/streaming/rx_audio.c's
// stage 1-4, including both stage-3 filters - see rx_audio.c) with a
// clock_gettime()-based stopwatch and prints a periodic avg/max summary
// against the ~10.667ms real-time budget (96kHz/PERIOD_FRAMES) - added
// after a real-hardware xrun-flood report (docs/ARCHITECTURE.md §10 step
// 7's follow-up) to get an actual measured number from the board in
// question instead of guessing from a different, faster dev machine.
// Same "rate-limit the printf, don't spam" discipline as xrun_note()
// above, on a longer (5s) window since this is a periodic status report,
// not a per-occurrence warning.
#define BLOCK_TIMING_WINDOW_NS 5000000000L /* 5 seconds */
#define BLOCK_PERIOD_BUDGET_MS (1000.0 * PERIOD_FRAMES / SAMPLE_RATE) /* 10.667ms */

// A first version of this only timed sound_process() - a real-hardware
// run (docs/ARCHITECTURE.md §10 step 7's follow-up) showed that alone
// avg=1.9-2.7ms/max=3.3ms, comfortably under the 10.667ms/block budget,
// with NO visible spike even in the same 5s windows where the playback
// xrun flood was actively happening - ruling out sound_process() itself
// (the DSP path steps 6/7 touched) as the cause. Since the flood is real
// but invisible here, the missing time has to be hiding somewhere this
// wasn't looking: the blocking `snd_pcm_readi()` call (which should
// normally take ~one period's worth of time as its own natural pacing,
// but would reveal an upstream capture-side stall if it ever took much
// more or less than that), the `snd_pcm_writei()` call plus the
// buffer-fill work around it, or genuinely unaccounted time between one
// iteration's write and the next iteration's read (`cw_poll_key()`, or a
// scheduling gap this thread didn't get to run through). This widens the
// same periodic report to all of those phases, plus the whole loop
// iteration's own period (measured start-of-read to start-of-read, which
// is what a healthy system should hold near 10.667ms exactly, no more,
// no less), so whichever phase is actually where the missing time goes
// shows up directly instead of needing another guess-and-recompile
// round trip.
struct phase_stats {
  long sum_ns;
  long max_ns;
  int count;
};

static void phase_note(struct phase_stats *p, long elapsed_ns) {
  p->sum_ns += elapsed_ns;
  if (elapsed_ns > p->max_ns)
    p->max_ns = elapsed_ns;
  p->count++;
}

static void phase_reset(struct phase_stats *p) {
  p->sum_ns = 0;
  p->max_ns = 0;
  p->count = 0;
}

static void phase_print(const struct phase_stats *p, const char *label) {
  double avg_ms = p->count ? (p->sum_ns / (double)p->count) / 1e6 : 0.0;
  double max_ms = p->max_ns / 1e6;
  fprintf(stderr, "  %-8s avg=%.3fms max=%.3fms (n=%d)\n", label, avg_ms, max_ms, p->count);
}

struct loop_timing_tracker {
  struct phase_stats read, process, write, period;
  struct timespec window_start;
  struct timespec last_loop_top;
  int started;
};

// One call per loop iteration, right at the top (before snd_pcm_readi());
// records the read/process/write phases *from the previous* iteration
// (all three are already-elapsed durations the caller measured) and this
// iteration's period (time since the previous iteration's own top-of-loop
// timestamp) - then, once BLOCK_TIMING_WINDOW_NS has elapsed, prints all
// four and resets. Pass -1 for any phase not applicable this iteration
// (e.g. write when pcm_playback is NULL).
// Whether to actually print the periodic report below - checked once
// (getenv() itself is cheap, but this runs every single audio block, so
// caching beats re-checking ~94 times a second forever). Opt-in via
// MAXIBITX_LOOP_TIMING, not on by default: this instrumentation did its
// job (traced the real xrun-flood cause to tx_pipeline_new()'s FFTW_
// MEASURE search draining the primed playback buffer - see
// sound_thread_start()'s own comment and docs/ARCHITECTURE.md §10 step
// 7's follow-up entries - now fixed and confirmed clean on the user's
// real hardware), so a normal run no longer needs to print a status
// block every 5 seconds forever. The tracking itself (a handful of
// clock_gettime()/phase_note() calls per block) stays unconditional -
// its own overhead is negligible next to sound_process() - so this knob
// costs nothing to leave in place for whenever the next real hardware
// mystery shows up.
static int loop_timing_should_print(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *v = getenv("MAXIBITX_LOOP_TIMING");
    cached = (v && *v) ? 1 : 0;
  }
  return cached;
}

static void loop_timing_note(struct loop_timing_tracker *t, long read_ns, long process_ns,
                              long write_ns) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  if (!t->started) {
    t->window_start = now;
    t->last_loop_top = now;
    t->started = 1;
    return; // no previous top-of-loop timestamp yet to measure a period against
  }

  long period_ns = (now.tv_sec - t->last_loop_top.tv_sec) * 1000000000L +
                    (now.tv_nsec - t->last_loop_top.tv_nsec);
  t->last_loop_top = now;

  if (read_ns >= 0)
    phase_note(&t->read, read_ns);
  if (process_ns >= 0)
    phase_note(&t->process, process_ns);
  if (write_ns >= 0)
    phase_note(&t->write, write_ns);
  phase_note(&t->period, period_ns);

  long elapsed_window_ns = (now.tv_sec - t->window_start.tv_sec) * 1000000000L +
                            (now.tv_nsec - t->window_start.tv_nsec);
  if (elapsed_window_ns >= BLOCK_TIMING_WINDOW_NS && t->period.count > 0) {
    if (loop_timing_should_print()) {
      fprintf(stderr,
              "sound: loop timing over last %ds (budget %.3fms/block period):\n",
              (int)(BLOCK_TIMING_WINDOW_NS / 1000000000L), BLOCK_PERIOD_BUDGET_MS);
      phase_print(&t->read, "read");
      phase_print(&t->process, "process");
      phase_print(&t->write, "write");
      phase_print(&t->period, "period");
    }
    phase_reset(&t->read);
    phase_reset(&t->process);
    phase_reset(&t->write);
    phase_reset(&t->period);
    t->window_start = now;
  }
}

/* ------------------------------------------------------------------ */
/*  Audio thread - capture -> sound_process() -> playback             */
/* ------------------------------------------------------------------ */
static void *audio_loop(void *arg) {
  (void)arg;

  int32_t cap_buf[MAX_FRAMES * CHANNELS];
  int32_t rx_buf[MAX_FRAMES];
  int32_t mic_buf[MAX_FRAMES];
  int32_t spk_buf[MAX_FRAMES];
  int32_t tx_buf[MAX_FRAMES];
  int32_t play_buf[MAX_FRAMES * CHANNELS]; // CW sidetone -> WM8731 DAC

  static struct xrun_tracker capture_xrun = {0};
  static struct xrun_tracker playback_xrun = {0};
  static struct loop_timing_tracker loop_timing = {0};

  while (g_running) {
    struct timespec t_read0, t_read1;
    clock_gettime(CLOCK_MONOTONIC, &t_read0);
    snd_pcm_sframes_t frames = snd_pcm_readi(pcm_capture, cap_buf, PERIOD_FRAMES);
    clock_gettime(CLOCK_MONOTONIC, &t_read1);
    long read_ns = (t_read1.tv_sec - t_read0.tv_sec) * 1000000000L +
                   (t_read1.tv_nsec - t_read0.tv_nsec);
    if (frames < 0) {
      xrun_note(&capture_xrun, "capture");
      if (xrun_recover(pcm_capture, (int)frames) < 0) {
        fprintf(stderr, "sound: capture recovery failed\n");
        break;
      }
      continue;
    }

    int n = (int)frames;
    if (n > MAX_FRAMES)
      n = MAX_FRAMES;

    for (int i = 0; i < n; i++) {
      rx_buf[i] = cap_buf[i * 2];
      mic_buf[i] = cap_buf[i * 2 + 1];
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    sound_process(rx_buf, mic_buf, spk_buf, tx_buf, n);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long process_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);

    // Once per audio block - checks the key, manages the CW keying
    // burst's hang timer, and asserts/releases PTT via radio_set_tx()
    // (see cw.c). Runs every iteration, TX or not, since this is what
    // actually notices the key going down in the first place.
    cw_poll_key();

    // Feed pcm_playback every block, TX or not, not just during a CW
    // burst - see docs/08_troubleshooting_and_bringup.md for why
    // (ALSA underrun detection is tied to the hardware clock, not to
    // whether writei() is called).
    long write_ns = -1; // stays -1 (not counted) if pcm_playback is NULL
    struct timespec t_write0, t_write1;
    if (pcm_playback) {
      clock_gettime(CLOCK_MONOTONIC, &t_write0);
      if (cw_tx_active()) {
        // Per-band calibrated scale (see the TX_SAMPLE_HEADROOM
        // comment above) - looked up once per block, not per
        // sample, since freq_hdr doesn't change mid-block.
        double band_scale = hw_settings_tx_scale(freq_hdr);
        double amp = TX_SAMPLE_HEADROOM * TX_DRIVE * band_scale * TX_GAIN_CORRECTION;

        // tx_audio_buf is this block's i_sample source for
        // tx_pipeline.c, one full TX_PIPELINE_BLOCK_LEN-sample block at
        // a time (that pipeline owns persistent overlap-save history
        // across calls, so it needs exactly this many new samples every
        // call - see docs/ARCHITECTURE.md §10 step 4); it also drives
        // the local sidetone/monitor channel directly below, same
        // "one signal, two uses" pattern real sbitx's own
        // `output_speaker[j] = i_sample * sidetone` uses (see cw.h).
        // Sized MAX_FRAMES, matching every other per-block buffer in
        // this function (mic_buf/rx_buf/etc. above) - n is bounded by
        // MAX_FRAMES already (see the clamp right after snd_pcm_readi()
        // above), so this can never overrun regardless of what n turns
        // out to be.
        static double tx_audio_buf[MAX_FRAMES];
        static float tx_pipe_in[TX_PIPELINE_BLOCK_LEN];
        static float tx_pipe_out[TX_PIPELINE_BLOCK_LEN];

        enum radio_mode tx_mode = radio_get_mode();
        enum tx_pipeline_sideband sideband = TX_PIPELINE_KEEP_UPPER;

        if (tx_mode == RADIO_MODE_CW) {
          // cw_get_sample() owns the envelope advance for this sample -
          // must be called exactly once per real audio sample (its
          // envelope timing depends on elapsed samples, not on how the
          // pipeline below batches them).
          for (int i = 0; i < n; i++)
            tx_audio_buf[i] = cw_get_sample();
          sideband = TX_PIPELINE_KEEP_UPPER; // CW groups with USB - see
                                              // tx_pipeline.h's enum comment
        } else {
          // USB/LSB (docs/ARCHITECTURE.md build order step 8): real mic
          // audio (mic_buf, captured above) is the i_sample source
          // instead of cw.c's tone - MIC_TX_INPUT_SCALE does the fixed
          // int32->float unit conversion, mic_tx_gain is the live,
          // operator-adjustable multiplier on top of it (see both
          // comments above/sound.h) - first on-air result (audible on
          // the local monitor, but no measurable power out) is exactly
          // what mic_tx_gain exists to bisect against a real wattmeter
          // without a rebuild per trial. cw_poll_key() (cw.c) only ever
          // asserts TX for this mode pair (plus CW) today, so this else
          // covers exactly USB/LSB in practice.
          double mic_gain = MIC_TX_INPUT_SCALE * mic_tx_gain;
          for (int i = 0; i < n; i++)
            tx_audio_buf[i] = mic_buf[i] * mic_gain;
          sideband = (tx_mode == RADIO_MODE_LSB) ? TX_PIPELINE_KEEP_LOWER
                                                  : TX_PIPELINE_KEEP_UPPER;
        }

        if (n == TX_PIPELINE_BLOCK_LEN) {
          for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++)
            tx_pipe_in[i] = (float)tx_audio_buf[i];
          tx_pipeline_process_block(cw_tx_pipeline, sideband, tx_pipe_in, tx_pipe_out);
        } else {
          // n is negotiated once at snd_pcm_hw_params_set_period_size_
          // near() and PERIOD_FRAMES==TX_PIPELINE_BLOCK_LEN by design
          // (see fft_filter.c's comment on why those two numbers
          // match), and snd_pcm_readi() above is called with exactly
          // PERIOD_FRAMES as its size, so in ordinary operation n
          // always equals TX_PIPELINE_BLOCK_LEN here; this branch only
          // fires on a genuinely abnormal read (short/interrupted, or
          // the negotiated hardware period turning out to differ from
          // what was requested). Feeding anything other than exactly
          // TX_PIPELINE_BLOCK_LEN new samples into the pipeline would
          // corrupt its overlap-save history's alignment for every
          // block after this one - far worse than one silent block -
          // so this skips the pipeline entirely and outputs silence on
          // the exciter channel this block (the local monitor channel
          // below is unaffected either way, since it doesn't share that
          // state).
          memset(tx_pipe_out, 0, sizeof(tx_pipe_out));
          // Logged once, not every occurrence: if this ever fires from
          // a genuinely mismatched negotiated period (rather than a
          // rare one-off short read), it would otherwise repeat on
          // every single TX block forever.
          static int warned = 0;
          if (!warned) {
            fprintf(stderr,
                    "sound: TX block size %d != %d (tx_pipeline's fixed "
                    "block length) - skipping tx_pipeline until this "
                    "resolves, local monitor unaffected (further "
                    "occurrences not logged)\n",
                    n, TX_PIPELINE_BLOCK_LEN);
            warned = 1;
          }
        }

        for (int i = 0; i < n; i++) {
          // R = the WM8731's PA-feeding channel - tx_pipeline.c's IF-
          // placed TX waveform, at the full wattmeter-calibrated
          // amplitude. tx_pipeline.c is unity-gain by construction for
          // a steady full-scale tone (bench-verified,
          // docs/ARCHITECTURE.md §10 step 4) - TX_GAIN_CORRECTION
          // below is carried over unchanged from the old direct-NCO
          // scheme on that basis, confirmed on a wattmeter for CW
          // (tx_power_calibration.md §8); USB/LSB's real mic-driven
          // envelope is a different amplitude statistics story
          // (speech isn't a steady tone) and has NOT been checked on a
          // wattmeter yet - worth doing before relying on this for a
          // real SSB transmission.
          double raw_tx = (i < TX_PIPELINE_BLOCK_LEN ? tx_pipe_out[i] : 0.0f) * amp;
          if (raw_tx > TX_SAMPLE_CLAMP)
            raw_tx = TX_SAMPLE_CLAMP;
          if (raw_tx < -TX_SAMPLE_CLAMP)
            raw_tx = -TX_SAMPLE_CLAMP;

          // L = local monitor only (on-board speaker) - the CW sidetone
          // pitch in CW mode, or a monitor copy of the operator's own
          // mic audio in USB/LSB (tx_audio_buf holds whichever one this
          // block is using - see the tx_mode branch above), at a fixed
          // comfort level - see SIDETONE_PEAK_AMPLITUDE above. Never
          // reaches the PA, and no longer moves when TX_GAIN_CORRECTION
          // does.
          double raw_side = tx_audio_buf[i] * SIDETONE_PEAK_AMPLITUDE;
          if (raw_side > TX_SAMPLE_CLAMP)
            raw_side = TX_SAMPLE_CLAMP;
          if (raw_side < -TX_SAMPLE_CLAMP)
            raw_side = -TX_SAMPLE_CLAMP;

          play_buf[i * 2] = (int32_t)raw_side;
          play_buf[i * 2 + 1] = (int32_t)raw_tx;
        }
      } else if (!in_tx) {
        // RX: play back rx_audio.c's demodulated CW tone (spk_buf)
        // on the local monitor channel - same channel cw.c's TX
        // sidetone uses. R stays silent; nothing drives the PA
        // while RX.
        for (int i = 0; i < n; i++) {
          play_buf[i * 2] = spk_buf[i];
          play_buf[i * 2 + 1] = 0;
        }
      } else {
        // in_tx but not cw_tx_active (e.g. PTT asserted between key
        // presses, or via CAT/network MOX without a CW burst) - stay
        // silent rather than play back RX audio while transmitting.
        memset(play_buf, 0, (size_t)n * 2 * sizeof(int32_t));
      }
      snd_pcm_sframes_t wframes = snd_pcm_writei(pcm_playback, play_buf, n);
      if (wframes < 0) {
        // Must check xrun_recover()'s return value here (unlike an
        // earlier version that didn't - see
        // docs/08_troubleshooting_and_bringup.md for the retry-
        // storm bug that caused) and disable playback gracefully
        // on real failure, rather than killing RX along with it.
        xrun_note(&playback_xrun, "playback");
        if (xrun_recover(pcm_playback, (int)wframes) < 0) {
          fprintf(stderr, "sound: playback recovery failed - disabling CW "
                          "sidetone/TX audio output (restart minibitx to "
                          "retry)\n");
          snd_pcm_close(pcm_playback);
          pcm_playback = NULL;
        }
      }
      clock_gettime(CLOCK_MONOTONIC, &t_write1);
      write_ns = (t_write1.tv_sec - t_write0.tv_sec) * 1000000000L +
                 (t_write1.tv_nsec - t_write0.tv_nsec);
    }

    loop_timing_note(&loop_timing, read_ns, process_ns, write_ns);
  }

  return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */
int sound_thread_start(const char *device_name) {
  const char *dev = device_name ? device_name : "hw:0,0";

  pcm_capture = open_pcm(dev, SND_PCM_STREAM_CAPTURE);
  if (!pcm_capture)
    return -1;

  // Playback: CW sidetone output only (see cw.c) - RX IQ still goes
  // out over the network/UAC2, not through here. Not a hard failure
  // if it doesn't open; audio_loop() checks pcm_playback before
  // writing to it, so minibitx still runs (just without CW TX audio).
  pcm_playback = open_pcm(dev, SND_PCM_STREAM_PLAYBACK);
  if (!pcm_playback) {
    printf("sound: playback unavailable, CW sidetone output disabled\n");
  }

  // The shared TX pipeline (tx_pipeline.c) - one persistent instance for
  // this run's lifetime, same as pcm_capture/pcm_playback above (its
  // FFTW plan is a real setup cost that must not be paid per TX burst -
  // see tx_pipeline.c/fft_filter.c's own comments). Deliberately created
  // BEFORE priming/starting playback below, not after - see that block's
  // own comment for why the ordering here matters now, not just the
  // FFTW_MEASURE-vs-ESTIMATE choice tx_pipeline_new() itself makes.
  cw_tx_pipeline = tx_pipeline_new();

  if (pcm_playback) {
    // Prime the playback ring buffer with a full buffer's worth of
    // silence right before starting the audio thread that's actually
    // going to keep it fed - added after a real-hardware xrun-flood
    // report (docs/ARCHITECTURE.md §10 step 7's follow-up) whose
    // loop_timing_note() read/process/write/period breakdown traced it
    // to a short burst of underruns in only the first few periods after
    // this device opens, never recurring once steady state was reached.
    // A first attempt primed the buffer immediately after opening
    // pcm_playback, ABOVE tx_pipeline_new() - that didn't help, because
    // tx_pipeline_new()'s own FFTW_MEASURE search (a synchronous, real
    // cost, same class as rx_filter_new()'s that motivated switching RX
    // to FFTW_ESTIMATE) ran for the whole gap between that priming and
    // the audio thread's creation below - long enough, on real hardware,
    // to drain the entire primed buffer before the thread that's
    // supposed to keep refilling it ever got to run, reproducing the
    // exact same startup burst regardless of the priming. Moving the
    // priming to HERE - after tx_pipeline_new() has already paid its
    // setup cost, immediately before pthread_create() - closes that gap
    // instead of trying to out-buffer it; tx_pipeline_new() switching to
    // FFTW_ESTIMATE (see its own comment) shrinks that cost further, for
    // the same reason rx_filter_new() did.
    int32_t silence[PERIOD_FRAMES * CHANNELS] = {0};
    for (int i = 0; i < 4; i++) // 4 periods == open_pcm()'s own negotiated buffer size
      snd_pcm_writei(pcm_playback, silence, PERIOD_FRAMES);
    printf("sound: primed playback with %d frames of silence\n", 4 * PERIOD_FRAMES);
  }

  g_running = 1;

  // Real-time priority, requested up front via pthread_attr_t so
  // pthread_create() fails fast if unavailable rather than silently
  // falling back later - see docs/08_troubleshooting_and_bringup.md
  // for why the audio thread needs SCHED_FIFO. Not fatal if it fails;
  // warns once and retries with default scheduling.
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
  struct sched_param sch = {.sched_priority = sched_get_priority_max(SCHED_FIFO)};
  pthread_attr_setschedparam(&attr, &sch);

  int rc = pthread_create(&audio_thread, &attr, audio_loop, NULL);
  if (rc != 0) {
    fprintf(stderr,
            "sound: WARNING - failed to set audio thread to SCHED_FIFO (%s). "
            "Falling back to normal scheduling.\n",
            strerror(rc));
    rc = pthread_create(&audio_thread, NULL, audio_loop, NULL);
  }
  pthread_attr_destroy(&attr);

  if (rc != 0) {
    fprintf(stderr, "sound: pthread_create failed\n");
    snd_pcm_close(pcm_capture);
    pcm_capture = NULL;
    g_running = 0;
    return -1;
  }

  printf("sound: running (%s)\n", dev);
  return 0;
}

void sound_thread_stop(void) {
  if (!g_running)
    return;

  g_running = 0;
  pthread_join(audio_thread, NULL);

  if (pcm_capture) {
    snd_pcm_drop(pcm_capture);
    snd_pcm_close(pcm_capture);
  }
  if (pcm_playback) {
    snd_pcm_drop(pcm_playback);
    snd_pcm_close(pcm_playback);
  }
  pcm_capture = pcm_playback = NULL;

  if (cw_tx_pipeline) {
    tx_pipeline_free(cw_tx_pipeline);
    cw_tx_pipeline = NULL;
  }

  printf("sound: stopped\n");
}
