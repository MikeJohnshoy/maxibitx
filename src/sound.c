// sound.c
// ALSA full-duplex audio thread and WM8731 codec control for maxibitx.
//
// Per ~10.7ms block (PERIOD_FRAMES at 96kHz):
//   capture:  L = RX IF from the crystal filter, R = mic
//   RX:       mix to I/Q, antialias, hand the I/Q to hpsdr_p1.c and
//             iq_stream.c, demodulate via rx_audio.c -> local speaker, and
//             (decimated to 48kHz) -> the USB audio gadget
//   TX:       CW tone, mic, or WSJT-X audio -> tx_pipeline.c -> exciter
//   playback: L = local speaker (RX audio, or TX sidetone/monitor),
//             R = exciter feed

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
#include "upsample48k.h"
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

// WM8731 'Line' is an on/off switch, not a gain (`amixer -c 0 sget
// 'Line'` shows cswitch only). The real analog gain is
// RX_CAPTURE_GAIN_PERCENT.
#define RX_LINE_INPUT_ON 1

// WM8731 'Capture': the only analog gain ahead of the ADC (there's no RF
// preamp). sound_mixer() maps percent onto the 0-31 raw range; 70 is step
// 21, ~-3.0dB. Bench-derived, not a final calibration - see
// docs/dsp_design_notes/rx_gain_and_level_calibration.md. rx_clip_check()
// warns if it turns out too hot.
#define RX_CAPTURE_GAIN_PERCENT 70

// WM8731 'Mic' gain for the R (mic) capture channel. A starting midpoint,
// not calibrated - and possibly inert on this board: 'Input Mux' is fixed
// to 'Line', so the mic most likely reaches the ADC on Line-In RIGHT,
// making 'Capture' the real mic level control (see
// sound_set_rx_capture()). Check with `amixer -c 0 sget 'Mic'`.
#define MIC_CAPTURE_GAIN_PERCENT 50

// WM8731 'Master' has independent L/R volumes, and here they feed
// different things:
//   L (FRONT_LEFT):  the local speaker/headphone amp - RX audio, and the
//                    TX sidetone/monitor
//   R (FRONT_RIGHT): the mainboard's diode mixer, i.e. the TX exciter feed
// so the two are never set together. L is set once at startup, to this
// value, and never touched again. R is 0 except during TX
// (sound_set_tx_drive(), radio.c's TX_MASTER_VOL). Listening volume is
// rx_audio.c's digital rx_volume (see RX_VOLUME_MAX), upstream of this
// analog stage - which is why this runs wide open: it costs no digital
// headroom.
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
// Fixed sidetone PCM peak (left channel only - never reaches the PA).
// Deliberately independent of TX_GAIN_CORRECTION; see
// docs/03_tx_processing_pipeline.md.
#define SIDETONE_PEAK_AMPLITUDE 10000000.0

// Fixed unit conversion: a raw S32_LE mic sample (full int32 range) to
// tx_pipeline.c's ~[-1,1] input, full scale = 1.0 - the mic counterpart to
// cw_get_sample()'s [-1,1] tone. Not a gain; that's mic_tx_gain below.
#define MIC_TX_INPUT_SCALE (1.0 / 2147483648.0)

// Live mic gain on top of MIC_TX_INPUT_SCALE (see sound.h). A plain
// double, no lock: written by rigctld's thread, read once per block by the
// audio thread - fine for a human-speed control, same as rx_audio.c's
// rx_volume.
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

// The shared TX pipeline (tx_pipeline.c): one instance for the process
// lifetime, since its FFTW plans are expensive to build. CW, USB/LSB and
// DIGITAL all use it; only the input source and the sideband differ (see
// audio_loop()).
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
    // Say so: a misspelled or absent element would otherwise turn every call
    // below into a silent no-op. sound_mixer_dump() shows what an element
    // actually supports.
    fprintf(stderr,
            "sound_mixer: '%s' not found on %s (check `amixer -c 0 scontrols`)\n",
            element, card_name);
    snd_mixer_close(handle);
    return;
  }

  // Independent ifs, not else-if: one ALSA simple element can have both a
  // switch and a volume (e.g. 'Master'), and each needs setting.
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

// Diagnostic: prints an element's capabilities and current values - a way
// to check what a control really supports on a given board, since a wrong
// name or an unexpected capability otherwise fails silently. Not called
// in normal operation; it opens its own mixer handle, so it's safe to call
// from anywhere while debugging.
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

// Sets one channel of a playback-volume element (sound_mixer() sets both).
// Needed for 'Master', whose L and R feed different things (see
// LOCAL_SPEAKER_GAIN_PERCENT). Warns and does nothing if the element has
// no playback volume.
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

// The capture-side counterpart of sound_mixer_channel(), for 'Capture',
// whose L (RX) and R (mic) channels must be set independently (see
// sound_set_rx_capture()). Warns if the element rejects a per-channel
// write, since a ganged register would defeat the point.
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
              0); // 'Line In' (bench-confirmed)
  sound_mixer("hw:0", "Line", RX_LINE_INPUT_ON); // un-mutes the line path (a switch)
  sound_mixer("hw:0", "Capture", RX_CAPTURE_GAIN_PERCENT); // both channels; afterwards only
                                                            // L changes (sound_set_rx_capture())
  sound_mixer("hw:0", "Mic", MIC_CAPTURE_GAIN_PERCENT); // possibly inert - see its #define

  // 'Master' L (local speaker) is set once, here. R (exciter feed) starts
  // muted and is raised only during TX - see LOCAL_SPEAKER_GAIN_PERCENT.
  sound_set_local_monitor(LOCAL_SPEAKER_GAIN_PERCENT);
  sound_mixer_channel("hw:0", "Master", SND_MIXER_SCHN_FRONT_RIGHT, 0);

  sound_mixer("hw:0", "Output Mixer HiFi", 1);
  sound_mixer("hw:0", "Output Mixer Line Bypass", 0);
  sound_mixer("hw:0", "Output Mixer Mic Sidetone", 0);
}

// Mute/restore the RX side of 'Capture' around a TX burst. radio.c's
// radio_tx_apply() mutes it before any TX RF exists and restores it once
// the relay has settled, protecting the ADC from TX energy
// (rx_gain_and_level_calibration.md §8).
//
// LEFT channel only: the mic reaches the ADC on the RIGHT channel of this
// same control, so muting both would silence USB/LSB TX audio
// (ARCHITECTURE.md §10 step 8). Relies on 'Capture' accepting per-channel
// writes; sound_mixer_capture_channel() warns if it doesn't.
void sound_set_rx_capture(int enable) {
  sound_mixer_capture_channel("hw:0", "Capture", SND_MIXER_SCHN_FRONT_LEFT,
                               enable ? RX_CAPTURE_GAIN_PERCENT : 0);
}

// Sets 'Master' LEFT only - the local speaker (see
// LOCAL_SPEAKER_GAIN_PERCENT). Called once at startup; public so a future
// physical volume control can use it.
void sound_set_local_monitor(int percent) {
  sound_mixer_channel("hw:0", "Master", SND_MIXER_SCHN_FRONT_LEFT, percent);
}

// Sets 'Master' RIGHT only - the exciter feed. radio_tx_apply() sets
// TX_MASTER_VOL for TX and 0 as the relay drops. Never touches LEFT, so
// there's nothing to restore after TX.
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

  /* 4 periods = ~43ms of buffer */
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

/* Recover from an ALSA xrun/suspend. Returns 0 on success. Callers must
 * stop retrying if it still fails (docs/08_troubleshooting_and_bringup.md).
 * drop()+prepare() is the fallback for when prepare() alone isn't
 * enough. */
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
            "'sudo setcap cap_sys_nice,cap_dac_override+ep ./maxibitx' "
            "(what 'make' runs), run as root, or "
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
// Clip guard on the raw ADC sample - the one signal RX_CAPTURE_GAIN_PERCENT
// acts on directly. Logs only the rising edge of a clip. Costs one fabs()
// and one compare per sample. See rx_gain_and_level_calibration.md §7.
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
  // Antialias filter state per rail (antialias.c), persistent across blocks.
  static struct antialias_state aa_i;
  static struct antialias_state aa_q;
  // 96kHz -> 48kHz decimation of the demodulated audio for the USB audio
  // gadget (usb_uac_decimation_design.md). hpsdr_p1.c and iq_stream.c get
  // the native 96kHz I/Q.
  static struct decim48k_state dec_audio;
  // rx_audio_process()'s uac_out tap (post-AGC, pre-volume), still at 96kHz.
  static double uac_audio[4096];

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

    // Mix to I/Q. The result is spectrally inverted (a station +d Hz above
    // dial lands at -d); consumers compensate - see rx_audio.c's
    // RX_IQ_SPECTRUM_INVERTED.
    i_samples[n] = rf * ((double)lo_i / 1073741824.0);
    q_samples[n] = rf * ((double)lo_q / 1073741824.0);

    // Antialias lowpass per rail (antialias_filter_design.md): removes the
    // self-image near +-48kHz without touching the crystal filter passband.
    i_samples[n] = antialias_apply(&aa_i, i_samples[n]);
    q_samples[n] = antialias_apply(&aa_q, q_samples[n]);
  }

  // Each I/Q consumer gets its own copy; any subset may be active.
  hpsdr_send_iq(i_samples, q_samples, n_samples);
  iq_stream_send(i_samples, q_samples, n_samples);

  // Demodulate: output_speaker gets the local speaker audio, uac_audio the
  // same audio before rx_volume. output_tx stays silent here; audio_loop()
  // drives the exciter.
  rx_audio_process(i_samples, q_samples, n_samples, output_speaker, uac_audio);
  memset(output_tx, 0, n_samples * sizeof(int32_t));

  // The USB audio gadget runs at 48kHz; decim48k_apply() yields a sample on
  // every other call.
  for (int n = 0; n < n_samples; n++) {
    double out_audio;
    if (decim48k_apply(&dec_audio, uac_audio[n], &out_audio))
      uac_push_audio_rx(out_audio);
  }
}

/* ------------------------------------------------------------------ */
/*  Per-block compute timing                                          */
/* ------------------------------------------------------------------ */
//
// Per-block timing of the audio loop's phases (read, process, write, and
// the whole loop period, which should sit at 10.667ms), summarized every
// 5s. Always measured (it's cheap); printed only when MAXIBITX_LOOP_TIMING
// is set. Used to trace the startup xrun flood (ARCHITECTURE.md §10 step
// 7 follow-ups).
#define BLOCK_TIMING_WINDOW_NS 5000000000L /* 5 seconds */
#define BLOCK_PERIOD_BUDGET_MS (1000.0 * PERIOD_FRAMES / SAMPLE_RATE) /* 10.667ms */

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

// Whether MAXIBITX_LOOP_TIMING is set. Cached, since this runs every block.
static int loop_timing_should_print(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *v = getenv("MAXIBITX_LOOP_TIMING");
    cached = (v && *v) ? 1 : 0;
  }
  return cached;
}

// Call once per loop iteration, before snd_pcm_readi(). Records the
// previous iteration's read/process/write durations (-1 = not applicable,
// e.g. write with no pcm_playback) and this iteration's period, then
// prints and resets once per BLOCK_TIMING_WINDOW_NS.
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
  int32_t play_buf[MAX_FRAMES * CHANNELS]; // WM8731 DAC: L local audio, R exciter

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
      // TX when the key/mic PTT is down (cw_tx_active()), or when in_tx is set
      // in DIGITAL - that mode's PTT is CAT-only, and cw_poll_key() ignores the
      // key GPIO there.
      if (cw_tx_active() || (in_tx && radio_get_mode() == RADIO_MODE_DIGITAL)) {
        // Per-band calibrated scale (see the TX_SAMPLE_HEADROOM
        // comment above) - looked up once per block, not per
        // sample, since freq_hdr doesn't change mid-block.
        double band_scale = hw_settings_tx_scale(freq_hdr);
        double amp = TX_SAMPLE_HEADROOM * TX_DRIVE * band_scale * TX_GAIN_CORRECTION;

        // This block's TX audio: tx_pipeline.c's input, and the local
        // sidetone/monitor below. The pipeline needs exactly TX_PIPELINE_BLOCK_LEN
        // new samples per call. Sized MAX_FRAMES like the other buffers; n never
        // exceeds it.
        static double tx_audio_buf[MAX_FRAMES];
        static float tx_pipe_in[TX_PIPELINE_BLOCK_LEN];
        static float tx_pipe_out[TX_PIPELINE_BLOCK_LEN];
        // DIGITAL: 48kHz gadget audio -> 96kHz. Persistent, since the upsampler
        // carries filter history.
        static struct upsample48k_state tx_upsampler;
        static double tx_audio_48k[MAX_FRAMES / 2 + 1];

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
        } else if (tx_mode == RADIO_MODE_DIGITAL) {
          // WSJT-X's audio from the USB gadget (48kHz), upsampled 2x;
          // ceil(n/2) inputs cover n outputs.
          int need_48k = (n + 1) / 2;
          int got_48k = uac_pull_audio_tx(tx_audio_48k, need_48k);
          int out_idx = 0;
          for (int i = 0; i < need_48k; i++) {
            // Past what the host sent (WSJT-X not running, or a gap), feed
            // silence rather than skip, keeping the filter history continuous.
            double in_sample = (i < got_48k) ? tx_audio_48k[i] : 0.0;
            double out2[2];
            upsample48k_apply(&tx_upsampler, in_sample, out2);
            if (out_idx < n)
              tx_audio_buf[out_idx++] = out2[0];
            if (out_idx < n)
              tx_audio_buf[out_idx++] = out2[1];
          }
          sideband = TX_PIPELINE_KEEP_UPPER; // FT8/digital convention:
                                              // always USB regardless of band
        } else {
          // USB/LSB: mic audio, through the fixed unit conversion and the
          // live mic gain. (cw_poll_key() only asserts TX for CW, USB and
          // LSB, so this branch is exactly USB/LSB.)
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
          // n equals TX_PIPELINE_BLOCK_LEN (== PERIOD_FRAMES) in normal
          // operation; this only fires on an abnormal short read. A
          // wrong-size block would misalign the pipeline's overlap-save
          // history for every later block, so skip it and send silence to
          // the exciter. The local monitor is unaffected.
          memset(tx_pipe_out, 0, sizeof(tx_pipe_out));
          // Logged once, in case the negotiated period itself differs.
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
          // R: exciter feed, at the wattmeter-calibrated amplitude.
          // tx_pipeline.c is unity-gain for a steady tone, so
          // TX_GAIN_CORRECTION's CW calibration carries over
          // (tx_power_calibration.md §8). USB/LSB speech hasn't been
          // checked on a wattmeter yet.
          double raw_tx = (i < TX_PIPELINE_BLOCK_LEN ? tx_pipe_out[i] : 0.0f) * amp;
          if (raw_tx > TX_SAMPLE_CLAMP)
            raw_tx = TX_SAMPLE_CLAMP;
          if (raw_tx < -TX_SAMPLE_CLAMP)
            raw_tx = -TX_SAMPLE_CLAMP;

          // L: local monitor - CW sidetone or a copy of the mic audio, at a
          // fixed level (SIDETONE_PEAK_AMPLITUDE). Never reaches the PA.
          double raw_side = tx_audio_buf[i] * SIDETONE_PEAK_AMPLITUDE;
          if (raw_side > TX_SAMPLE_CLAMP)
            raw_side = TX_SAMPLE_CLAMP;
          if (raw_side < -TX_SAMPLE_CLAMP)
            raw_side = -TX_SAMPLE_CLAMP;

          play_buf[i * 2] = (int32_t)raw_side;
          play_buf[i * 2 + 1] = (int32_t)raw_tx;
        }
      } else if (!in_tx) {
        // RX: demodulated audio on the local speaker; R (exciter) silent.
        for (int i = 0; i < n; i++) {
          play_buf[i * 2] = spk_buf[i];
          play_buf[i * 2 + 1] = 0;
        }
      } else {
        // TX without key/mic PTT and not DIGITAL (e.g. CAT/network MOX in
        // CW/USB/LSB): silence rather than RX audio while transmitting.
        memset(play_buf, 0, (size_t)n * 2 * sizeof(int32_t));
      }
      snd_pcm_sframes_t wframes = snd_pcm_writei(pcm_playback, play_buf, n);
      if (wframes < 0) {
        // On unrecoverable failure, disable playback rather than stop RX
        // too (docs/08_troubleshooting_and_bringup.md).
        xrun_note(&playback_xrun, "playback");
        if (xrun_recover(pcm_playback, (int)wframes) < 0) {
          fprintf(stderr, "sound: playback recovery failed - disabling local "
                          "audio and TX output (restart maxibitx to "
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

  // Playback carries the local speaker and the exciter feed. Not fatal if
  // it fails: audio_loop() checks pcm_playback, so RX still runs, just
  // without local audio or TX.
  pcm_playback = open_pcm(dev, SND_PCM_STREAM_PLAYBACK);
  if (!pcm_playback) {
    printf("sound: playback unavailable - no local audio or TX\n");
  }

  // Create the TX pipeline before priming playback below - see there.
  cw_tx_pipeline = tx_pipeline_new();

  if (pcm_playback) {
    // Prime playback with a full buffer of silence immediately before the
    // audio thread starts. Nothing slow (like tx_pipeline_new()'s FFTW
    // planning) may run between priming and pthread_create(), or the buffer
    // drains and the first periods underrun - the cause of the startup xrun
    // flood (ARCHITECTURE.md §10 step 7 follow-ups).
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
