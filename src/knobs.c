// knobs.c - see knobs.h for what each knob does.

#include "knobs.h"
#include "encoder.h"
#include "radio.h"    // freq_hdr, in_tx, radio_tune_to()
#include "rx_audio.h" // volume and mute
#include <stdatomic.h>
#include <stdio.h>

static _Atomic int step_hz = KNOBS_STEP_DEFAULT_HZ;

// The steps a push moves through, in order.
static const int push_steps[] = {10, 100, 1000, 10000};
#define PUSH_STEPS ((int)(sizeof(push_steps) / sizeof(push_steps[0])))

// A fast turn: the time between clicks in one direction below which the
// step is multiplied, fastest first. 20 clicks a turn: 10 ms is five turns
// a second.
static const struct {
  int64_t below_ns;
  int times;
} speedup[] = {
    {10000000, 10},
    {20000000, 5},
    {40000000, 2},
};
#define SPEEDUPS ((int)(sizeof(speedup) / sizeof(speedup[0])))
#define SPEEDUP_BELOW_STEP_HZ 1000

// The tuning knob's last click, for the speed-up.
static int64_t last_click_ns = 0;
static int last_direction = 0;

// For knobs_log_settled(): when each knob last changed something, and
// whether that change is still to be reported.
static _Atomic int64_t changed_ns[RADIO_KNOBS];
static _Atomic int unreported[RADIO_KNOBS];

static void changed(enum radio_knob k, int64_t ts) {
  atomic_store(&changed_ns[k], ts);
  atomic_store(&unreported[k], 1);
}

int knobs_set_step(int hz) {
  if (hz < 1)
    hz = 1;
  if (hz > KNOBS_STEP_MAX_HZ)
    hz = KNOBS_STEP_MAX_HZ;
  atomic_store(&step_hz, hz);
  return hz;
}

int knobs_get_step(void) { return atomic_load(&step_hz); }

static void tune(int detents, int64_t ts) {
  int step = atomic_load(&step_hz);
  int times = 1;
  if (detents == last_direction && step < SPEEDUP_BELOW_STEP_HZ) {
    int64_t gap = ts - last_click_ns;
    for (int i = 0; i < SPEEDUPS; i++) {
      if (gap < speedup[i].below_ns) {
        times = speedup[i].times;
        break;
      }
    }
  }
  last_click_ns = ts;
  last_direction = detents;
  if (in_tx)
    return; // the dial holds still while transmitting
  long f = (long)freq_hdr + (long)detents * step * times;
  if (f < KNOBS_FREQ_MIN_HZ)
    f = KNOBS_FREQ_MIN_HZ;
  if (f > KNOBS_FREQ_MAX_HZ)
    f = KNOBS_FREQ_MAX_HZ;
  if (f != freq_hdr) {
    radio_tune_to((uint32_t)f);
    changed(RADIO_KNOB_TUNING, ts);
  }
}

static void next_step(void) {
  int step = atomic_load(&step_hz);
  int next = push_steps[0];
  for (int i = 0; i < PUSH_STEPS; i++) {
    if (push_steps[i] > step) {
      next = push_steps[i];
      break;
    }
  }
  atomic_store(&step_hz, next);
  printf("knobs: tuning step %d Hz\n", next);
}

void knobs_turn(enum radio_knob k, int detents, int64_t ts_ns) {
  if (k == RADIO_KNOB_TUNING) {
    tune(detents, ts_ns);
  } else if (k == RADIO_KNOB_VOLUME) {
    if (rx_audio_get_mute())
      rx_audio_set_mute(0);
    rx_audio_set_volume(rx_audio_get_volume() + detents * KNOBS_VOLUME_PER_CLICK);
    changed(RADIO_KNOB_VOLUME, ts_ns);
  }
}

void knobs_push(enum radio_knob k) {
  if (k == RADIO_KNOB_TUNING) {
    next_step();
  } else if (k == RADIO_KNOB_VOLUME) {
    rx_audio_set_mute(!rx_audio_get_mute());
    printf("knobs: speaker %s\n", rx_audio_get_mute() ? "muted" : "unmuted");
  }
}

int knobs_log_settled(int64_t now_ns) {
  int printed = 0;
  for (int k = 0; k < RADIO_KNOBS; k++) {
    if (!atomic_load(&unreported[k]) ||
        now_ns - atomic_load(&changed_ns[k]) < KNOBS_SETTLED_MS * 1000000LL)
      continue;
    atomic_store(&unreported[k], 0);
    if (k == RADIO_KNOB_TUNING)
      printf("knobs: tuned to %d Hz\n", freq_hdr);
    else
      printf("knobs: volume %d%%\n", rx_audio_get_volume());
    printed++;
  }
  return printed;
}

int knobs_start(void) { return encoder_start(knobs_turn, knobs_push); }
