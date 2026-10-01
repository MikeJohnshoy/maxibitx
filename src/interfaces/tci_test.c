// tci_test.c
//
// Bench harness for the TCI server (tci.c, tci_stream.c, tci_ws.c) over
// loopback, with the radio stubbed: scripted client sessions checked
// against the rules in tci_design_study.md §4 - a JTDX-style start-up
// (lowercase, one command per message, start before ready, within 1.5 s),
// echoes of every set to every client, reads answered to the asker alone,
// Hamlib's uppercase commands, the PTT rules, receive audio in each format,
// rate and channel count, I/Q at 96 and 48 kHz, transmit audio pulled
// with TX_CHRONO the way JTDX answers it, and CW text - cw_macros with
// prosigns and speed changes, cw_msg with callsign corrections, stopping,
// terminal mode, a paddle taking over - read back from what the real keyer
// keys. The harness plays the audio thread itself.
//
// Build and run: make test-tci
// ./test-tci --serve [port] instead runs the stubbed server with a test
// tone as receive audio and I/Q, printing what clients do - for trying a
// client (tools/tci_client.py, JTDX) without a radio.

#define _GNU_SOURCE
#include "tci.h"
#include "tci_ws.h"
#include "ws_test_client.h"
#include "cw.h"
#include "hw_settings.h"
#include "keyer.h"
#include "morse.h"
#include "radio.h"
#include "rx_audio.h"
#include "sound.h"
#include <ctype.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

/* ---- Radio stubs ---------------------------------------------------------- */

int freq_hdr = 14074000;
int in_tx;
static enum radio_mode stub_mode = RADIO_MODE_USB;
static int stub_rit, stub_rit_on, stub_volume = 67, stub_tx_allowed = 1;
static int stub_key_holds_tx, stub_verbose;
static double stub_power = 1.0;

void radio_tune_to(uint32_t f) {
  freq_hdr = (int)f;
  stub_rit = stub_rit_on = 0;
  if (stub_verbose)
    printf("radio: tuned to %u Hz\n", f);
}
int radio_set_tx(int on) {
  if (on && !stub_tx_allowed)
    return -1;
  in_tx = on != 0;
  if (stub_verbose)
    printf("radio: %s\n", on ? "TX" : "RX");
  return 0;
}
void radio_set_rit(int hz) {
  stub_rit = hz;
  stub_rit_on = hz != 0;
}
int radio_get_rit(void) { return stub_rit; }
void radio_set_rit_enabled(int on) { stub_rit_on = on != 0; }
int radio_rit_enabled(void) { return stub_rit_on; }
void radio_set_mode(enum radio_mode m) {
  stub_mode = m;
  if (stub_verbose)
    printf("radio: mode %d\n", m);
}
enum radio_mode radio_get_mode(void) { return stub_mode; }
int hw_settings_tx_allowed(int f) {
  (void)f;
  return stub_tx_allowed;
}
int cw_tx_active(void) { return stub_key_holds_tx; }
static int stub_text_queued, stub_hold;
void cw_text_queued(void) { stub_text_queued++; }
void cw_hold_tx(int h) { stub_hold = h; }
int rx_audio_get_volume(void) { return stub_volume; }
void rx_audio_set_volume(int p) { stub_volume = p; }
int rx_audio_get_strength_db(void) { return 0; }
double sound_get_tx_power(void) { return stub_power; }
void sound_set_tx_power(double f) { stub_power = f < 0 ? 0 : f > 1 ? 1 : f; }

/* ---- Checks and the client side ----------------------------------------- */

static int failures;
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (cond) {                                                                                    \
      printf("  ok   ");                                                                           \
    } else {                                                                                       \
      printf("  FAIL ");                                                                           \
      failures++;                                                                                  \
    }                                                                                              \
    printf(__VA_ARGS__);                                                                           \
    printf("\n");                                                                                  \
  } while (0)

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Every text message any client receives is checked here: one command,
// ending in ';', keyword in lowercase.
static int text_seen, text_bad;
static char text_bad_example[128];

static void check_text_form(const char *t) {
  text_seen++;
  size_t n = strlen(t);
  int ok = n >= 2 && t[n - 1] == ';' && strchr(t, ';') == t + n - 1;
  for (const char *p = t; ok && *p && *p != ':' && *p != ';'; p++)
    ok = !isupper((unsigned char)*p);
  if (!ok && !text_bad++)
    snprintf(text_bad_example, sizeof(text_bad_example), "%s", t);
}

struct frame {
  int op;
  uint8_t *data;
  size_t len;
};

// Next frame within timeout_ms; op -1 if none.
static struct frame next_frame(int fd, int timeout_ms) {
  struct frame f = {-1, NULL, 0};
  struct pollfd p = {fd, POLLIN, 0};
  if (poll(&p, 1, timeout_ms) <= 0)
    return f;
  f.op = recv_frame(fd, &f.data, &f.len, NULL);
  if (f.op == 1)
    check_text_form((const char *)f.data);
  return f;
}

static int send_text(int fd, const char *t) { return send_frame(fd, 1, 1, t, strlen(t), 1); }

// Waits for the exact text, skipping everything else. Returns 1 if seen.
static int expect(int fd, const char *want, int timeout_ms) {
  long long end = now_ms() + timeout_ms;
  for (long long left; (left = end - now_ms()) > 0;) {
    struct frame f = next_frame(fd, (int)left);
    if (f.op < 0)
      return 0;
    int hit = f.op == 1 && !strcmp((char *)f.data, want);
    free(f.data);
    if (hit)
      return 1;
  }
  return 0;
}

// Collects text messages arriving within ms into buf (';'-joined).
static void gather(int fd, int ms, char *buf, size_t cap) {
  buf[0] = '\0';
  long long end = now_ms() + ms;
  for (long long left; (left = end - now_ms()) > 0;) {
    struct frame f = next_frame(fd, (int)left);
    if (f.op < 0)
      break;
    if (f.op == 1 && strlen(buf) + f.len + 1 < cap)
      strcat(buf, (char *)f.data);
    free(f.data);
  }
}

static void drain(int fd) {
  char junk[16384];
  gather(fd, 60, junk, sizeof(junk));
}

static uint32_t u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static float f32(const uint8_t *p) {
  uint32_t v = u32(p);
  float f;
  memcpy(&f, &v, 4);
  return f;
}

// Next binary frame of the given stream type within timeout_ms.
static struct frame next_stream(int fd, uint32_t type, int timeout_ms) {
  long long end = now_ms() + timeout_ms;
  for (long long left; (left = end - now_ms()) > 0;) {
    struct frame f = next_frame(fd, (int)left);
    if (f.op < 0)
      break;
    if (f.op == 2 && f.len >= 64 && u32(f.data + 24) == type)
      return f;
    free(f.data);
  }
  struct frame none = {-1, NULL, 0};
  return none;
}

static int connect_ready(int *init_ms, char *log, size_t cap) {
  long long t0 = now_ms();
  int fd = ws_connect();
  if (fd < 0)
    return -1;
  log[0] = '\0';
  int count = 0;
  for (;;) {
    struct frame f = next_frame(fd, 1500);
    if (f.op < 0)
      break;
    if (f.op == 1) {
      count++;
      if (strlen(log) + f.len + 2 < cap) {
        strcat(log, (char *)f.data);
        strcat(log, "\n");
      }
      int ready = !strcmp((char *)f.data, "ready;");
      free(f.data);
      if (ready)
        break;
    } else {
      free(f.data);
    }
  }
  if (init_ms)
    *init_ms = (int)(now_ms() - t0);
  return fd;
}

/* ---- The audio thread's side, played by the harness ---------------------- */

static void push_audio(double freq, double amp, int n, int *phase) {
  double block[512];
  for (int done = 0; done < n;) {
    int m = n - done < 512 ? n - done : 512;
    for (int k = 0; k < m; k++)
      block[k] = amp * sin(2.0 * M_PI * freq * (*phase + k) / 48000.0);
    tci_push_audio_rx(block, m);
    *phase += m;
    done += m;
    usleep(2000);
  }
}

static void push_iq(double freq, double amp, int n, int *phase) {
  double bi[1024], bq[1024];
  for (int done = 0; done < n;) {
    int m = n - done < 1024 ? n - done : 1024;
    for (int k = 0; k < m; k++) {
      double a = 2.0 * M_PI * freq * (*phase + k) / 96000.0;
      bi[k] = amp * cos(a);
      bq[k] = amp * sin(a);
    }
    tci_push_iq(bi, bq, m);
    *phase += m;
    done += m;
    usleep(2000);
  }
}

#define SCALE (1.0 / (500000000.0 * 5.6234133))

// Output samples skipped before measuring a tone: a frame part-filled by
// the previous tone, and the filter's settling.
#define SETTLE 2048

/* ---- Tests ---------------------------------------------------------------- */

static int a, b; // two clients

static void test_startup(void) {
  printf("start-up, as JTDX and Hamlib see it\n");
  char log[8192];
  int ms = 0;
  a = connect_ready(&ms, log, sizeof(log));
  int lines = 0;
  for (char *p = log; *p; p++)
    lines += *p == '\n';
  CHECK(a >= 0 && strstr(log, "ready;\n"), "ready; received, %d messages in %d ms", lines, ms);
  CHECK(ms < 1500 && lines < 256, "within JTDX's 1.5 s and Hamlib's 256 messages");
  CHECK(!strncmp(log, "protocol:ExpertSDR3,2.0;\ndevice:maxibitx;\n", 42),
        "protocol:ExpertSDR3,2.0; then device:maxibitx; first");
  char *start = strstr(log, "start;\n"), *ready = strstr(log, "ready;\n");
  CHECK(start && ready && start < ready, "start; before ready;");
  CHECK(strstr(log, "vfo:0,0,14074000;\n") && strstr(log, "modulation:0,usb;\n") &&
            strstr(log, "drive:0,100;\n") && strstr(log, "trx:0,false;\n") &&
            strstr(log, "modulations_list:USB,LSB,CW,CWR,DIGU;\n") &&
            strstr(log, "tx_enable:0,true;\n") && strstr(log, "volume:-17;\n"),
        "state: vfo, modulation, drive, trx, modes, tx_enable, volume");
  CHECK(!strstr(log, "tx_sensors"), "no tx_sensors");
  b = connect_ready(NULL, log, sizeof(log));
  CHECK(b >= 0, "a second client connects alongside");
}

static void test_control(void) {
  printf("control: echoes, reads, several clients\n");
  send_text(a, "vfo:0,0,7074000;");
  CHECK(expect(a, "vfo:0,0,7074000;", 2000) && freq_hdr == 7074000, "vfo set echoed to the setter");
  CHECK(expect(b, "vfo:0,0,7074000;", 2000), "and to the other client");
  drain(a);
  drain(b);
  send_text(a, "vfo:0,0,7074000;");
  CHECK(expect(a, "vfo:0,0,7074000;", 2000), "an unchanged set is echoed too");
  send_text(a, "vfo:0,0,50;");
  CHECK(expect(a, "vfo:0,0,7074000;", 2000), "out-of-range set answered with the frequency in force");
  drain(a);
  drain(b);

  char got[4096];
  send_text(a, "VFO:0,0;");
  gather(a, 200, got, sizeof(got));
  CHECK(!strcmp(got, "vfo:0,0,7074000;"), "Hamlib's uppercase read answered in lowercase: %s", got);
  gather(b, 200, got, sizeof(got));
  CHECK(got[0] == '\0', "a read is answered to the asker alone");

  send_text(a, "vfo:0,0;modulation:0;trx:0;");
  gather(a, 200, got, sizeof(got));
  CHECK(!strcmp(got, "vfo:0,0,7074000;modulation:0,usb;trx:0,false;"),
        "three commands in one message, three replies: %s", got);

  send_text(a, "modulation:0,DIGU;");
  CHECK(expect(b, "modulation:0,digu;", 1000) && stub_mode == RADIO_MODE_DIGITAL,
        "modulation set, echoed lowercase to all");
  send_text(a, "modulation:0,am;");
  CHECK(expect(a, "modulation:0,digu;", 1000) && stub_mode == RADIO_MODE_DIGITAL,
        "unknown mode answered with the mode in force");
  send_text(a, "modulation:0,cwr;");
  CHECK(expect(a, "modulation:0,cwr;", 1000) && stub_mode == RADIO_MODE_CWR, "cwr");
  send_text(a, "modulation:0,digu;");
  drain(a);
  drain(b);

  send_text(a, "drive:0,40;");
  CHECK(expect(b, "drive:0,40;", 1000) && fabs(stub_power - 0.4) < 1e-9, "drive 40 -> 0.40 of max_power");
  send_text(a, "volume:-12;");
  CHECK(expect(a, "volume:-12;", 1000) && stub_volume == 76, "volume -12 dB -> 76%%, echoed exactly");
  send_text(a, "mute:true;");
  gather(a, 200, got, sizeof(got));
  CHECK(strstr(got, "mute:true;") && strstr(got, "volume:-60;") && stub_volume == 0, "mute: %s", got);
  send_text(a, "mute:false;");
  gather(a, 200, got, sizeof(got));
  CHECK(strstr(got, "mute:false;") && strstr(got, "volume:-12;") && stub_volume == 76,
        "unmute restores -12 dB");
  send_text(a, "rit_offset:0,500;");
  gather(a, 200, got, sizeof(got));
  CHECK(!strcmp(got, "rit_offset:0,500;") && stub_rit == 500 && !stub_rit_on,
        "rit_offset leaves RIT off");
  send_text(a, "rit_enable:0,true;");
  CHECK(expect(a, "rit_enable:0,true;", 1000) && stub_rit_on, "rit_enable");
  send_text(a, "cw_macros_speed:25;");
  gather(a, 200, got, sizeof(got));
  CHECK(strstr(got, "cw_macros_speed:25;") && strstr(got, "cw_keyer_speed:25;") && keyer_get_wpm() == 25,
        "speed 25 WPM, both speeds echoed");

  send_text(a, "split_enable:false;");
  CHECK(expect(a, "split_enable:0,false;", 600), "WSJT-X Improved's bare split_enable:false;");
  send_text(a, "SPLIT_ENABLE:0,true;");
  CHECK(expect(a, "split_enable:0,false;", 600), "split refused: echoed false");
  send_text(a, "tune:0,true;");
  CHECK(expect(a, "tune:0,false;", 600) && !in_tx, "tune: echoed false, no TX");
  send_text(a, "RX_NB_ENABLE:0;");
  CHECK(expect(a, "rx_nb_enable:0,false;", 600), "missing DSP switch read as off");
  send_text(a, "audio_start:0;");
  CHECK(expect(a, "audio_start:0;", 600), "audio_start echoed (JTDX waits 500 ms)");
  send_text(a, "audio_stop:0;");
  drain(a);
  drain(b);

  freq_hdr = 3573000; // changed elsewhere - by rigctld, say
  CHECK(expect(a, "vfo:0,0,3573000;", 500) && expect(b, "vfo:0,0,3573000;", 500),
        "a change made elsewhere reaches every client");
  stub_volume = 80;
  CHECK(expect(a, "volume:-10;", 500), "so does a volume change");

  send_text(a, "rx_sensors_enable:true,100;");
  CHECK(expect(a, "rx_sensors:0,-73.0;", 500) && expect(a, "rx_channel_sensors:0,0,-73.0;", 500),
        "rx sensors, with a decimal point");
  send_text(a, "rx_sensors_enable:false;");
  send_text(a, "tx_sensors_enable:true,100;");
  gather(a, 400, got, sizeof(got));
  CHECK(!strstr(got, "tx_sensors:") && !strstr(got, "rx_sensors:0"), "no tx_sensors; rx sensors off");
}

static void test_rx_audio(void) {
  printf("receive audio\n");
  int phase = 0;
  send_text(a, "audio_start:0;");
  expect(a, "audio_start:0;", 600);
  push_audio(1000.0, 5e8, 2048, &phase);
  struct frame f = next_stream(a, 1, 1000);
  int ok = f.op == 2 && u32(f.data + 4) == 48000 && u32(f.data + 8) == 3 &&
           u32(f.data + 20) == 2048 && u32(f.data + 28) == 2 && f.len == 64 + 2048 * 4;
  CHECK(ok, "default: 48 kHz, float32, stereo, length 2048 (JTDX's assumption)");
  double worst = 0.0;
  for (int k = 0; ok && k < 1024; k++) {
    double want = 5e8 * sin(2.0 * M_PI * 1000.0 * k / 48000.0) * SCALE;
    double l = f32(f.data + 64 + 8 * k), r = f32(f.data + 68 + 8 * k);
    worst = fmax(worst, fmax(fabs(l - want), fabs(r - want)));
  }
  CHECK(ok && worst < 1e-6, "samples exact, left = right, AGC target at -15 dBFS (error %.1e)", worst);
  free(f.data);
  send_text(a, "audio_stop:0;");
  drain(a);

  static const struct {
    int rate, type, channels;
    const char *tname;
    double pass_hz, stop_hz;
  } cases[] = {
      {24000, 0, 1, "int16", 1000, 15000},
      {12000, 0, 1, "int16", 1000, 8000},
      {8000, 1, 2, "int24", 1000, 5000},
      {12000, 2, 1, "int32", 1500, 7000},
  };
  for (int c = 0; c < 4; c++) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "audio_samplerate:%d;audio_stream_sample_type:%s;"
             "audio_stream_channels:%d;audio_start:0;",
             cases[c].rate, cases[c].tname, cases[c].channels);
    send_text(a, cmd);
    drain(a);
    double level[2];
    for (int t = 0; t < 2; t++) {
      double hz = t ? cases[c].stop_hz : cases[c].pass_hz;
      push_audio(hz, 5e8, 48000 / 2, &phase);
      double sum = 0.0;
      int count = 0, frames_ok = 1;
      struct frame s;
      long long end = now_ms() + 300;
      while (now_ms() < end && (s = next_stream(a, 1, 100)).op == 2) {
        int len = (int)u32(s.data + 20);
        frames_ok &= u32(s.data + 4) == (uint32_t)cases[c].rate &&
                     u32(s.data + 8) == (uint32_t)cases[c].type &&
                     u32(s.data + 28) == (uint32_t)cases[c].channels;
        int bps = cases[c].type == 0 ? 2 : cases[c].type == 1 ? 3 : 4;
        for (int k = 0; k < len; k += cases[c].channels) {
          const uint8_t *p = s.data + 64 + k * bps;
          double v = cases[c].type == 0   ? (int16_t)(p[0] | p[1] << 8) / 32767.0
                     : cases[c].type == 1 ? ((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 |
                                                       (uint32_t)p[2] << 24) >> 8) / 8388607.0
                                          : (int32_t)u32(p) / 2147483647.0;
          if (count++ >= SETTLE) // past what's left of the last tone
            sum += v * v;
        }
        free(s.data);
      }
      level[t] = 10.0 * log10(sum / (count > SETTLE ? count - SETTLE : 1) + 1e-30);
      if (!frames_ok)
        level[t] = 999;
    }
    double expect_db = 20.0 * log10(5e8 * SCALE / sqrt(2.0));
    CHECK(fabs(level[0] - expect_db) < 0.2 && level[1] - level[0] < -60.0,
          "%d Hz %s %s: %.0f Hz at %+.2f dB, %.0f Hz (would alias) at %.1f dB", cases[c].rate,
          cases[c].tname, cases[c].channels == 1 ? "mono" : "stereo", cases[c].pass_hz,
          level[0] - expect_db, cases[c].stop_hz, level[1] - level[0]);
    send_text(a, "audio_stop:0;");
    drain(a);
  }
  send_text(a, "audio_samplerate:48000;audio_stream_sample_type:float32;audio_stream_channels:2;");
  drain(a);
}

static void test_iq(void) {
  printf("I/Q\n");
  int phase = 0;
  send_text(a, "iq_start:0;");
  expect(a, "iq_start:0;", 600);
  push_iq(10000.0, 0.5, 4096, &phase);
  struct frame f = next_stream(a, 0, 1000);
  int ok = f.op == 2 && u32(f.data + 4) == 96000 && u32(f.data + 8) == 3 &&
           u32(f.data + 20) == 4096 && u32(f.data + 28) == 2 && f.len == 64 + 4096 * 4;
  CHECK(ok, "96 kHz float32 complex, length 4096 (2048 samples, 21 ms)");
  double worst = 0.0;
  for (int k = 0; ok && k < 2048; k++) {
    double ang = 2.0 * M_PI * 10000.0 * k / 96000.0;
    worst = fmax(worst, fabs(f32(f.data + 64 + 8 * k) - 0.5 * cos(ang)));
    worst = fmax(worst, fabs(f32(f.data + 68 + 8 * k) + 0.5 * sin(ang)));
  }
  CHECK(ok && worst < 1e-6, "conjugated (Q negated): a raw -f tone reads +f (error %.1e)", worst);
  free(f.data);

  send_text(a, "iq_samplerate:192000;");
  CHECK(expect(a, "iq_samplerate:96000;", 600), "192 kHz answered with the 96 kHz in force");
  send_text(a, "iq_samplerate:48000;");
  CHECK(expect(a, "iq_samplerate:48000;", 600), "48 kHz accepted");
  drain(a);
  double level[4];
  const double tones[4] = {-5000.0, 15000.0, 20000.0, 35000.0}; // raw, so read as -f
  for (int t = 0; t < 4; t++) {
    push_iq(tones[t], 0.5, 48000, &phase);
    double power = 0;
    int n = 0, good = 1;
    struct frame s;
    long long end = now_ms() + 300;
    while (now_ms() < end && (s = next_stream(a, 0, 100)).op == 2) {
      good &= u32(s.data + 4) == 48000 && u32(s.data + 20) == 2048;
      for (int k = 0; k < 1024; k++) {
        double i = f32(s.data + 64 + 8 * k), q = f32(s.data + 68 + 8 * k);
        if (n >= SETTLE)
          power += i * i + q * q;
        n++;
      }
      free(s.data);
    }
    level[t] = good ? 10.0 * log10(power / (n - SETTLE) / 0.25 + 1e-30) : 999;
  }
  CHECK(fabs(level[0]) < 0.1 && fabs(level[1]) < 0.1 && level[3] < -60.0,
        "48 kHz: +5 and -15 kHz at %+.2f and %+.2f dB (-20 kHz, near the edge, %+.2f dB); "
        "35 kHz, which would alias, at %.1f dB",
        level[0], level[1], level[2], level[3]);
  send_text(a, "iq_stop:0;");
  drain(a);
}

// Answers a TX_CHRONO the way JTDX does: the chrono's rate, format and
// length copied, channels left as junk, twice the payload needed.
static void answer_chrono(int fd, const struct frame *chrono, float *next_value) {
  uint32_t length = u32(chrono->data + 20);
  size_t len = 64 + (size_t)length * 4 * 2;
  uint8_t *out = calloc(1, len);
  memcpy(out, chrono->data, 64);
  out[24] = 2; // TX_AUDIO_STREAM
  memset(out + 28, 0xAB, 4);
  for (uint32_t k = 0; k < length; k += 2) {
    float v = *next_value;
    *next_value += 1e-4f;
    memcpy(out + 64 + 4 * k, &v, 4);
    memcpy(out + 68 + 4 * k, &v, 4);
  }
  memset(out + 64 + 4 * length, 0x7F, len - 64 - 4 * length); // junk past length
  send_frame(fd, 1, 2, out, len, 1);
  free(out);
}

static int chronos_within(int fd, int ms, struct frame *keep, int max_keep) {
  int count = 0;
  long long end = now_ms() + ms;
  struct frame f;
  while (now_ms() < end && (f = next_stream(fd, 3, (int)(end - now_ms()))).op == 2) {
    int ok = u32(f.data + 4) == 48000 && u32(f.data + 8) == 3 && u32(f.data + 20) == 2048 &&
             u32(f.data + 28) == 2 && f.len == 64;
    if (!ok)
      count = -1000;
    if (keep && count >= 0 && count < max_keep)
      keep[count] = f;
    else
      free(f.data);
    count++;
  }
  return count;
}

static void test_tx(void) {
  char txt[4096];
  printf("transmit and PTT\n");
  drain(a);
  drain(b);
  send_text(a, "tx_stream_audio_buffering:50;");
  expect(a, "tx_stream_audio_buffering:50;", 600);
  send_text(a, "trx:0,true,tci;");
  CHECK(expect(b, "trx:0,true;", 1000) && in_tx && tci_tx_audio_owned(),
        "trx with source tci: TX on, audio from the client's stream");
  struct frame chronos[8];
  int n = chronos_within(a, 150, chronos, 8);
  CHECK(n == 4, "%d TX_CHRONO (48 kHz float32 stereo, length 2048) - 50 ms + one request ahead", n);
  CHECK(chronos_within(b, 100, NULL, 0) == 0, "none to the client that didn't key");
  float value = 0.0f;
  for (int k = 0; k < n && k < 8; k++) {
    answer_chrono(a, &chronos[k], &value);
    free(chronos[k].data);
  }
  usleep(100000);
  double out[4096];
  int got = tci_pull_audio_tx(out, 4096);
  double worst = 0.0;
  for (int k = 0; k < got; k++)
    worst = fmax(worst, fabs(out[k] - (float)(k * 1e-4f)));
  CHECK(got == 4096 && worst < 1e-3, "%d samples queued for the audio thread, left channel, in order",
        got);
  n = chronos_within(a, 150, NULL, 0);
  CHECK(n == 4, "as the audio thread uses them, %d more requested", n);
  n = chronos_within(a, 500, NULL, 0);
  CHECK(n >= 4, "unanswered requests are forgotten and asked again (%d in 500 ms)", n);

  send_text(a, "trx:0,false;");
  CHECK(expect(b, "trx:0,false;", 1000) && !in_tx && !tci_tx_audio_owned(), "trx false: TX off");
  struct frame late = {2, calloc(1, 64), 64};
  late.data[20] = 0;
  answer_chrono(a, &late, &value); // a late answer
  free(late.data);
  usleep(50000);
  tci_tx_audio_idle();
  CHECK(tci_pull_audio_tx(out, 4096) == 0, "late audio after TX off never reaches the next TX");
  drain(a);
  drain(b);

  stub_mode = RADIO_MODE_DIGITAL;
  send_text(b, "TRX:0,true,Vac;");
  CHECK(expect(a, "trx:0,true;", 1000) && in_tx && !tci_tx_audio_owned(),
        "Hamlib's TRX:0,true,Vac: TX on, audio stays with the USB gadget");
  in_tx = 0; // released elsewhere - rigctld T 0, say
  CHECK(expect(a, "trx:0,false;", 500), "TX released elsewhere reaches every client");

  stub_tx_allowed = 0;
  send_text(a, "trx:0,true,tci;");
  CHECK(expect(a, "trx:0,false;", 1000) && !in_tx && !tci_tx_audio_owned(),
        "outside the TX bands: refused, echoed false");
  stub_tx_allowed = 1;

  stub_key_holds_tx = 1;
  in_tx = 1;
  drain(a);
  send_text(a, "trx:0,false;");
  CHECK(expect(a, "trx:0,true;", 1000) && in_tx, "while the local key holds TX, trx is ignored");
  stub_key_holds_tx = 0;
  in_tx = 0;
  drain(a);
  drain(b);

  send_text(a, "trx:0,true,tci;");
  expect(b, "trx:0,true;", 1000);
  drain(a);
  send_text(b, "trx:0,true,tci;");
  send_text(b, "trx:0,false;");
  gather(b, 200, txt, sizeof(txt));
  CHECK(in_tx && !strcmp(txt, "trx:0,true;trx:0,true;") && chronos_within(a, 100, NULL, 0) > 0,
        "while one client holds TX, another's trx on and off are ignored");
  send_text(a, "trx:0,false;");
  CHECK(expect(b, "trx:0,false;", 1000) && !in_tx, "and the holder releases it");
  drain(a);
  drain(b);

  send_text(b, "trx:0,true,tci;");
  expect(b, "trx:0,true;", 1000);
  close(b);
  b = -1;
  long long end = now_ms() + 1000;
  while (in_tx && now_ms() < end)
    usleep(5000);
  CHECK(!in_tx && !tci_tx_audio_owned(), "a client that disconnects holding TX releases it");
  CHECK(expect(a, "trx:0,false;", 500), "and the others are told");
}

/* ---- CW text, read back from the keyer ----------------------------------- */

// The keyer, run in real time as the audio thread runs it; inject asks for a
// dot paddle tap in the next block.
static uint8_t cw_key[96000 * 40];
static atomic_long cw_pos;
static atomic_int cw_run, cw_inject;
static pthread_t cw_thread;

static void *keyer_main(void *arg) {
  (void)arg;
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  while (atomic_load(&cw_run)) {
    next.tv_nsec += 10666667;
    if (next.tv_nsec >= 1000000000L) {
      next.tv_nsec -= 1000000000L;
      next.tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    struct key_event ev[2];
    int k = 0;
    if (atomic_exchange(&cw_inject, 0)) {
      ev[k++] = (struct key_event){.offset = 100, .contact = KEY_TIP, .paddle = KEY_DOT, .closed = 1};
      ev[k++] = (struct key_event){.offset = 400, .contact = KEY_TIP, .paddle = KEY_DOT, .closed = 0};
    }
    long at = atomic_load(&cw_pos);
    if (at + 1024 <= (long)sizeof(cw_key)) {
      keyer_run_block(ev, k, cw_key + at, 1024);
      atomic_store(&cw_pos, at + 1024);
    }
  }
  return NULL;
}

// Reads key[from..to) back as text. A mark is a dash if longer than twice
// base_T (speeds stay within a step of the base); a gap under 2T of the mark
// before it joins elements, under 5T ends a character, longer ends a word.
// Prosigns read as <XX>. Each character's speed, from its first element,
// goes in wpm[] (spaces get 0).
static int decode(long from, long to, long base_T, char *out, int max, int *wpm) {
  int no = 0, nw = 0;
  char pat[16];
  int np = 0, char_wpm = 0;
  long i = from;
  while (i < to && !cw_key[i])
    i++;
  while (i < to) {
    long s0 = i;
    while (i < to && cw_key[i])
      i++;
    long len = i - s0;
    int dash = len > 2 * base_T;
    long T = dash ? len / 3 : len;
    if (np == 0)
      char_wpm = (int)((115200.0 / T) + 0.5);
    if (np < 15)
      pat[np++] = dash ? '-' : '.';
    pat[np] = 0;
    long g = i;
    while (i < to && !cw_key[i])
      i++;
    long gap = i - g;
    if (i < to && gap < 2 * T)
      continue;
    const char *name = NULL;
    char c = '?';
    for (int k = 32; k < MORSE_PROSIGN_END; k++)
      if (morse_pattern((unsigned char)k) && !strcmp(morse_pattern((unsigned char)k), pat)) {
        c = (char)k;
        name = morse_prosign_name((unsigned char)k);
        break;
      }
    if (name && no + 4 < max) {
      no += snprintf(out + no, (size_t)(max - no), "<%s>", name);
    } else if (no < max - 1) {
      out[no++] = c;
    }
    wpm[nw++] = char_wpm;
    if (i < to && gap >= 5 * T && no < max - 1) {
      out[no++] = ' ';
      wpm[nw++] = 0;
    }
    np = 0;
  }
  out[no] = 0;
  return nw;
}

// 1 if every mark in key[from..to) is T or 3T and every space between marks
// T, 3T or 7T, exactly.
static int timing_exact(long from, long to, long T) {
  long i = from;
  while (i < to && !cw_key[i])
    i++;
  while (i < to) {
    long s0 = i;
    while (i < to && cw_key[i])
      i++;
    if (i - s0 != T && i - s0 != 3 * T)
      return 0;
    long g = i;
    while (i < to && !cw_key[i])
      i++;
    if (i < to && i - g != T && i - g != 3 * T && i - g != 7 * T)
      return 0;
  }
  return 1;
}

// Waits until the keyer has sent everything and been idle for 300 ms.
static void wait_cw_idle(int timeout_ms) {
  long long end = now_ms() + timeout_ms, quiet_since = now_ms();
  while (now_ms() < end) {
    if (keyer_text_busy() || keyer_text_queued())
      quiet_since = now_ms();
    else if (now_ms() - quiet_since > 300)
      return;
    usleep(10000);
  }
}

static void test_cw(void) {
  printf("CW text\n");
  char got[256], txt[4096];
  int wpm[128];
  const long T40 = (115200 + 20) / 40;
  stub_mode = RADIO_MODE_CW;
  send_text(a, "cw_macros_speed:40;");
  expect(a, "cw_macros_speed:40;", 500);
  keyer_reset();
  atomic_store(&cw_pos, 0);
  atomic_store(&cw_run, 1);
  pthread_create(&cw_thread, NULL, keyer_main, NULL);
  drain(a);

  long p0 = atomic_load(&cw_pos);
  int queued_before = stub_text_queued;
  send_text(a, "cw_macros:0,tu >5nn< |sk|;");
  wait_cw_idle(8000);
  int n = decode(p0, atomic_load(&cw_pos), T40, got, sizeof(got), wpm);
  CHECK(!strcmp(got, "TU 5NN <SK>") && stub_text_queued > queued_before,
        "cw_macros:0,tu >5nn< |sk|; keys \"%s\" and asks for TX", got);
  CHECK(n == 8 && wpm[0] == 40 && wpm[1] == 40 && wpm[3] == 45 && wpm[5] == 45 && wpm[7] == 40,
        "speeds %d %d / %d %d %d / %d WPM: > raises 5NN by 5, < lowers it back", wpm[0], wpm[1],
        wpm[3], wpm[4], wpm[5], wpm[7]);
  p0 = atomic_load(&cw_pos);
  send_text(a, "cw_macros:0,e;");
  wait_cw_idle(4000);
  decode(p0, atomic_load(&cw_pos), T40, got, sizeof(got), wpm);
  CHECK(!strcmp(got, "E") && wpm[0] == 40, "the next macro starts at the keyer's speed again");

  p0 = atomic_load(&cw_pos);
  send_text(a, "cw_msg:0,R,K1$2,5;");
  send_text(a, "cw_msg:W1AW;");
  CHECK(expect(a, "callsign_send:W1AW;", 8000), "callsign_send:W1AW; once the callsign is out");
  send_text(a, "cw_msg:XX9X;"); // too late: ignored
  wait_cw_idle(8000);
  decode(p0, atomic_load(&cw_pos), T40, got, sizeof(got), wpm);
  CHECK(!strcmp(got, "R W1AW W1AW 5"),
        "cw_msg:0,R,K1$2,5; corrected at once to W1AW keys \"%s\"; a later correction is ignored",
        got);
  CHECK(timing_exact(p0, atomic_load(&cw_pos), T40),
        "fed a character at a time, every mark and space is still exact");

  p0 = atomic_load(&cw_pos);
  send_text(a, "cw_msg:0,_,AB1CD,_;");
  usleep(290000); // A and most of the gap after it, at 40 WPM
  send_text(a, "cw_msg:AB9XY;");
  wait_cw_idle(8000);
  decode(p0, atomic_load(&cw_pos), T40, got, sizeof(got), wpm);
  CHECK(strlen(got) == 5 && !strncmp(got, "AB", 2) && !strcmp(got + 3, "XY"),
        "a correction mid-callsign changes what isn't yet started: \"%s\"", got);
  CHECK(expect(a, "callsign_send:AB9XY;", 1000), "and callsign_send reports the corrected one");

  p0 = atomic_load(&cw_pos);
  send_text(a, "cw_msg:0,R,K1,5;");
  send_text(a, "cw_macros:0, EE;");
  wait_cw_idle(8000);
  decode(p0, atomic_load(&cw_pos), T40, got, sizeof(got), wpm);
  CHECK(!strcmp(got, "R K1 5 EE"), "cw_macros during a cw_msg waits for it: \"%s\"", got);

  p0 = atomic_load(&cw_pos);
  send_text(a, "cw_macros:0,TTTTTTTTTT;");
  usleep(400000);
  send_text(a, "cw_msg:0,_,K1,_;");
  wait_cw_idle(8000);
  decode(p0, atomic_load(&cw_pos), T40, got, sizeof(got), wpm);
  size_t nt = strspn(got, "T");
  CHECK(nt >= 2 && nt < 10 && !strcmp(got + nt, " K1"),
        "cw_msg during cw_macros stops it, a word space before: \"%s\"", got);

  p0 = atomic_load(&cw_pos);
  send_text(a, "cw_macros:0,EEEEEEEEEE;");
  usleep(300000);
  send_text(a, "cw_macros_stop;");
  wait_cw_idle(4000);
  decode(p0, atomic_load(&cw_pos), T40, got, sizeof(got), wpm);
  nt = strspn(got, "E");
  CHECK(nt >= 2 && nt < 10 && got[nt] == 0, "cw_macros_stop: %zu of 10 sent", nt);

  drain(a);
  send_text(a, "cw_terminal:true;");
  CHECK(expect(a, "cw_terminal:true;", 500), "cw_terminal:true; echoed");
  send_text(a, "cw_macros:0,EE <;"); // a trailing space and code don't delay it
  long long t0 = now_ms();
  CHECK(expect(a, "cw_macros_empty;", 3000) && stub_hold == 1 && now_ms() - t0 < 200,
        "terminal mode: TX held, cw_macros_empty; after %lld ms (as the last E starts)",
        now_ms() - t0);
  wait_cw_idle(4000);
  send_text(a, "cw_macros:0,EEEEEEEEEE;");
  usleep(150000);
  atomic_store(&cw_inject, 1);
  wait_cw_idle(4000);
  usleep(50000);
  CHECK(stub_hold == 0, "a paddle tap in terminal mode ends the TX hold");
  send_text(a, "cw_terminal:0,false;");
  CHECK(expect(a, "cw_terminal:false;", 500) && stub_hold == 0, "cw_terminal:0,false; releases it");
  send_text(a, "cw_macros:0,E;");
  wait_cw_idle(4000);
  gather(a, 200, txt, sizeof(txt));
  CHECK(!strstr(txt, "cw_macros_empty"), "no cw_macros_empty outside terminal mode");

  p0 = atomic_load(&cw_pos);
  send_text(a, "cw_msg:0,TEST,K1,TU;");
  send_text(a, "cw_macros:0, EE;");
  usleep(400000);
  atomic_store(&cw_inject, 1); // the operator touches the paddle
  wait_cw_idle(4000);
  usleep(300000);
  decode(p0, atomic_load(&cw_pos), T40, got, sizeof(got), wpm);
  CHECK(strncmp(got, "TEST", 4) && !strstr(got, "K1") && !strstr(got, "EE"),
        "a paddle tap ends the message and what waited behind it: \"%s\"", got);

  p0 = atomic_load(&cw_pos);
  send_text(a, "cw_msg:0,CQ,K1;");
  wait_cw_idle(8000);
  decode(p0, atomic_load(&cw_pos), T40, got, sizeof(got), wpm);
  CHECK(!strcmp(got, "CQ K1"), "cw_msg without a suffix field: \"%s\"", got);
  p0 = atomic_load(&cw_pos);
  send_text(a, "cw_msg:0,TU,_,73;");
  wait_cw_idle(8000);
  decode(p0, atomic_load(&cw_pos), T40, got, sizeof(got), wpm);
  CHECK(!strcmp(got, "TU 73"), "cw_msg with an empty callsign: \"%s\"", got);
  queued_before = stub_text_queued;
  send_text(a, "cw_macros:0,#;");
  usleep(100000);
  CHECK(stub_text_queued == queued_before && !keyer_text_busy(),
        "a macro with nothing to send doesn't ask for TX");

  // A message replacing a macro that fills the keyer's queue waits for room
  // rather than being refused.
  static char big[600];
  snprintf(big, sizeof(big), "cw_macros:0,%0511d;", 0);
  for (char *q = big + 12; *q == '0'; q++)
    *q = 'E';
  p0 = atomic_load(&cw_pos);
  send_text(a, big);
  usleep(50000);
  send_text(a, "cw_msg:0,R,K1,_;");
  wait_cw_idle(8000);
  decode(p0, atomic_load(&cw_pos), T40, got, sizeof(got), wpm);
  nt = strspn(got, "E");
  CHECK(nt >= 1 && nt < 20 && !strcmp(got + nt, " R K1"),
        "a cw_msg behind a full queue still goes: \"%s\"", got);

  stub_mode = RADIO_MODE_USB;
  int before = keyer_text_queued() + keyer_text_busy();
  send_text(a, "cw_macros:0,EEE;");
  usleep(100000);
  CHECK(before == 0 && !keyer_text_busy() && !keyer_text_queued(), "cw_macros in USB is ignored");
  stub_mode = RADIO_MODE_CW;

  atomic_store(&cw_run, 0);
  pthread_join(cw_thread, NULL);
  drain(a);
}

static void test_counts(void) {
  CHECK(text_seen > 100 && text_bad == 0,
        "all %d text messages: one command, lowercase keyword%s%s", text_seen,
        text_bad ? " - e.g. " : "", text_bad ? text_bad_example : "");
}

/* ---- Serve mode ------------------------------------------------------------- */

static volatile sig_atomic_t stop_serving;
static void on_sigint(int s) {
  (void)s;
  stop_serving = 1;
}

static int serve(int port) {
  stub_verbose = 1;
  signal(SIGINT, on_sigint);
  if (tci_init("", port, TCI_DEFAULT_MAX_CLIENTS) < 0) {
    fprintf(stderr, "can't listen on TCP %d\n", port);
    return 1;
  }
  printf("stubbed TCI server on TCP %d: receive audio is a 1000 Hz tone, I/Q a tone 1500 Hz\n"
         "above the dial, CW text is printed as the keyer keys it; Ctrl-C stops\n", port);
  int ph_a = 0, ph_iq = 0, tx_blocks = 0, idle_blocks = 0;
  long burst_from = -1;
  double tx_sum = 0;
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  while (!stop_serving) {
    next.tv_nsec += 10666667;
    if (next.tv_nsec >= 1000000000L) {
      next.tv_nsec -= 1000000000L;
      next.tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    double au[512], iq_i[1024], iq_q[1024], txb[512];
    for (int k = 0; k < 512; k++, ph_a++)
      au[k] = 5e8 * sin(2.0 * M_PI * 1000.0 * ph_a / 48000.0);
    for (int k = 0; k < 1024; k++, ph_iq++) {
      double ang = -2.0 * M_PI * 1500.0 * ph_iq / 96000.0; // raw I/Q is inverted
      iq_i[k] = 0.1 * cos(ang);
      iq_q[k] = 0.1 * sin(ang);
    }
    tci_push_iq(iq_i, iq_q, 1024);
    tci_push_audio_rx(au, 512);
    if (in_tx && tci_tx_audio_owned()) {
      int got = tci_pull_audio_tx(txb, 512);
      for (int k = 0; k < got; k++)
        tx_sum += txb[k] * txb[k];
      if (++tx_blocks % 94 == 0) {
        printf("tx audio: %.1f dBFS rms over the last second\n",
               10.0 * log10(tx_sum / (94 * 512) + 1e-20));
        tx_sum = 0;
      }
    } else {
      tci_tx_audio_idle();
      tx_blocks = 0;
    }

    // The keyer, as the audio thread runs it; each burst of CW text is
    // printed once the keyer has been idle a third of a second.
    long at = atomic_load(&cw_pos);
    if (at + 1024 > (long)sizeof(cw_key)) {
      at = 0;
      burst_from = -1;
    }
    keyer_run_block(NULL, 0, cw_key + at, 1024);
    atomic_store(&cw_pos, at + 1024);
    if (keyer_text_busy()) {
      if (burst_from < 0)
        burst_from = at;
      idle_blocks = 0;
    } else if (burst_from >= 0 && ++idle_blocks > 30) {
      char text[512];
      int wpm[256], w = keyer_get_wpm();
      decode(burst_from, at, (115200 + w / 2) / w, text, sizeof(text), wpm);
      printf("keyed: %s\n", text);
      burst_from = -1;
    }
  }
  tci_stop();
  return 0;
}

int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "--serve"))
    return serve(argc >= 3 ? atoi(argv[2]) : TCI_DEFAULT_PORT);

  for (ws_test_port = 45951; ws_test_port < 46000; ws_test_port++)
    if (tci_init("127.0.0.1", ws_test_port, 4) == 0)
      break;
  printf("TCI bench harness, loopback port %d\n", ws_test_port);
  test_startup();
  test_control();
  test_rx_audio();
  test_iq();
  test_tx();
  test_cw();
  test_counts();
  close(a);
  tci_stop();
  printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
  return failures ? 1 : 0;
}
