// tci.c
//
// The TCI protocol over tci_ws.c: the initialization burst each client
// gets on connecting, the commands, and keeping every client in step.
//
// What clients need that the spec doesn't say (tci_design_study.md §4):
// keywords are sent in lowercase, one command per WebSocket message; every
// set is echoed to every client with the value actually in force, even when
// nothing changed (JTDX waits for the echo, and compares it as a string);
// start; comes before ready; and the burst stays well under the 256
// messages Hamlib reads looking for ready. Commands are read in either case,
// several to a message. maxibitx identifies as protocol ExpertSDR3, which
// JTDX keys its TCI behaviour to, and as device maxibitx.
//
// One receiver, one VFO: vfo channel B and dds report channel A's frequency,
// if is always 0, and split, XIT, tune and the DSP switches maxibitx lacks
// report off. No tx_sensors are sent: a DE board has no power or SWR
// measurement. Changes made elsewhere (rigctld, CAT, the panel, the key)
// have no notification, so a service thread samples the published state
// every 50 ms and sends what changed; the same thread runs tci_stream.c.
//
// PTT follows the rules rigctld T and CAT TX follow - refused outside the
// [tx_band] table, ignored while the local key holds TX - plus: trx with
// source "tci" takes the transmit audio from that client's stream; while one
// TCI client holds TX, the others' trx is ignored; and a client that
// disconnects while holding TX releases it.

#define _GNU_SOURCE
#include "tci.h"
#include "cw.h"
#include "hw_settings.h"
#include "keyer.h"
#include "radio.h"
#include "rx_audio.h"
#include "sound.h"
#include "tci_stream.h"
#include "tci_ws.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define TCI_TICK_MS 5
#define TCI_STATE_TICKS 10 // state sampled every 50 ms
#define TCI_VFO_MIN 100000
#define TCI_VFO_MAX 30000000
#define TCI_MAX_ARGS 8
#define TCI_UNMUTE_DEFAULT_PERCENT 67

struct client {
  int in_use, id;
  struct tci_stream_cfg cfg;
  int buffering_ms;
  int rx_sensors, rx_sensors_ms;
  long long rx_sensors_due;
};

// State as last sent to the clients.
struct state {
  int freq, mode, tx, drive, volume_db, mute, rit_on, rit, tx_enable, wpm;
};

static struct client clients[WS_MAX_CLIENTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct state pub;
static int tx_owner = -1;    // slot whose trx holds TX, -1 none
static int unmute_percent = TCI_UNMUTE_DEFAULT_PERCENT;
static int trace;            // MAXIBITX_TCI_TRACE: print every message
static atomic_int running;
static pthread_t service_thread;

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- Sending ------------------------------------------------------------ */

static void send_to(int slot, const char *fmt, ...) {
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (trace)
    printf("tci: -> %d %s\n", slot, msg);
  ws_send_text(clients[slot].id, msg);
}

static void broadcast(const char *fmt, ...) {
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (trace)
    printf("tci: -> all %s\n", msg);
  for (int s = 0; s < WS_MAX_CLIENTS; s++)
    if (clients[s].in_use)
      ws_send_text(clients[s].id, msg);
}

/* ---- The radio's state, in TCI's terms ------------------------------------ */

static const char *mode_name(int m) {
  switch (m) {
  case RADIO_MODE_CW:
    return "cw";
  case RADIO_MODE_CWR:
    return "cwr";
  case RADIO_MODE_LSB:
    return "lsb";
  case RADIO_MODE_DIGITAL:
    return "digu";
  default:
    return "usb";
  }
}

static int mode_from_name(const char *s, enum radio_mode *m) {
  static const struct {
    const char *name;
    enum radio_mode mode;
  } names[] = {{"usb", RADIO_MODE_USB},
               {"lsb", RADIO_MODE_LSB},
               {"cw", RADIO_MODE_CW},
               {"cwr", RADIO_MODE_CWR},
               {"digu", RADIO_MODE_DIGITAL}};
  for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
    if (!strcasecmp(s, names[k].name)) {
      *m = names[k].mode;
      return 1;
    }
  }
  return 0;
}

// TCI volume is dB, -60..0 (-60 silent); rx_audio's is 0-100 percent at
// 0.5 dB per percent with 0 a true mute (rx_audio.h). Integer dB from -49 to
// 0 round-trip exactly.
static int volume_db(int percent) { return percent <= 0 ? -60 : (int)lround((percent - 100) / 2.0); }
static int volume_percent(int db) { return db <= -50 ? 0 : db >= 0 ? 100 : 100 + 2 * db; }

static void snapshot(struct state *s) {
  s->freq = freq_hdr;
  s->mode = radio_get_mode();
  s->tx = in_tx != 0;
  s->drive = (int)lround(sound_get_tx_power() * 100.0);
  int percent = rx_audio_get_volume();
  s->volume_db = volume_db(percent);
  s->mute = percent == 0;
  s->rit_on = radio_rit_enabled() != 0;
  s->rit = radio_get_rit();
  s->tx_enable = hw_settings_tx_allowed(freq_hdr) != 0;
  s->wpm = keyer_get_wpm();
}

static const char *tf(int b) { return b ? "true" : "false"; }

// Each sends one piece of state to every client and records it as sent.
static void pub_freq(const struct state *s) {
  broadcast("dds:0,%d;", s->freq);
  broadcast("vfo:0,0,%d;", s->freq);
  broadcast("vfo:0,1,%d;", s->freq);
  pub.freq = s->freq;
}
static void pub_mode(const struct state *s) {
  broadcast("modulation:0,%s;", mode_name(s->mode));
  pub.mode = s->mode;
}
static void pub_tx(const struct state *s) {
  broadcast("trx:0,%s;", tf(s->tx));
  pub.tx = s->tx;
}
static void pub_drive(const struct state *s) {
  broadcast("drive:0,%d;", s->drive);
  pub.drive = s->drive;
}
static void pub_volume(const struct state *s) {
  broadcast("volume:%d;", s->volume_db);
  pub.volume_db = s->volume_db;
}
static void pub_mute(const struct state *s) {
  broadcast("mute:%s;", tf(s->mute));
  broadcast("rx_mute:0,%s;", tf(s->mute));
  pub.mute = s->mute;
}
static void pub_rit_on(const struct state *s) {
  broadcast("rit_enable:0,%s;", tf(s->rit_on));
  pub.rit_on = s->rit_on;
}
static void pub_rit(const struct state *s) {
  broadcast("rit_offset:0,%d;", s->rit);
  pub.rit = s->rit;
}
static void pub_tx_enable(const struct state *s) {
  broadcast("tx_enable:0,%s;", tf(s->tx_enable));
  pub.tx_enable = s->tx_enable;
}
static void pub_wpm(const struct state *s) {
  broadcast("cw_macros_speed:%d;", s->wpm);
  broadcast("cw_keyer_speed:%d;", s->wpm);
  pub.wpm = s->wpm;
}

// Sends whatever changed since it was last sent (a change made elsewhere).
static void publish_changes(void) {
  struct state s;
  snapshot(&s);
  if (s.freq != pub.freq)
    pub_freq(&s);
  if (s.tx_enable != pub.tx_enable)
    pub_tx_enable(&s);
  if (s.mode != pub.mode)
    pub_mode(&s);
  if (s.tx != pub.tx)
    pub_tx(&s);
  if (s.drive != pub.drive)
    pub_drive(&s);
  if (s.volume_db != pub.volume_db)
    pub_volume(&s);
  if (s.mute != pub.mute)
    pub_mute(&s);
  if (s.rit_on != pub.rit_on)
    pub_rit_on(&s);
  if (s.rit != pub.rit)
    pub_rit(&s);
  if (s.wpm != pub.wpm)
    pub_wpm(&s);
  if (!s.mute)
    unmute_percent = rx_audio_get_volume();
}

/* ---- PTT ------------------------------------------------------------------ */

static void tx_release_owner(void) {
  tx_owner = -1;
  tci_stream_tx_end();
}

static void cmd_trx(int slot, int on, const char *source) {
  if (cw_tx_active()) {
    printf("tci: trx from client %d ignored - the local key holds TX\n", slot);
  } else if (tx_owner >= 0 && tx_owner != slot) {
    printf("tci: trx from client %d ignored - client %d holds TX\n", slot, tx_owner);
  } else if (on) {
    int own_audio = !strcasecmp(source, "tci");
    if (own_audio)
      tci_stream_tx_begin(slot, clients[slot].buffering_ms);
    if (radio_set_tx(1) < 0) {
      if (own_audio)
        tci_stream_tx_end();
      printf("tci: TX from client %d refused - %d Hz is outside the calibrated TX bands\n", slot,
             freq_hdr);
    } else {
      if (!own_audio)
        tci_stream_tx_end(); // this client's earlier trx may have had its own audio
      tx_owner = slot;
      printf("tci: client %d TX on (audio from %s)\n", slot,
             own_audio                                  ? "its TCI stream"
             : radio_get_mode() == RADIO_MODE_DIGITAL ? "the USB gadget"
                                                        : "no TCI stream");
    }
  } else {
    radio_set_tx(0);
    if (tx_owner >= 0)
      printf("tci: client %d TX off\n", slot);
    tx_release_owner();
  }
  struct state s;
  snapshot(&s);
  pub_tx(&s);
}

/* ---- Commands ------------------------------------------------------------- */

static int parse_bool(const char *s, int *v) {
  if (!strcasecmp(s, "true") || !strcmp(s, "1")) {
    *v = 1;
    return 1;
  }
  if (!strcasecmp(s, "false") || !strcmp(s, "0")) {
    *v = 0;
    return 1;
  }
  return 0;
}

static int parse_int(const char *s, long long *v) {
  char *end;
  if (!*s)
    return 0;
  *v = strtoll(s, &end, 10);
  return *end == '\0';
}

static void apply_stream(int slot) { tci_stream_set(slot, &clients[slot].cfg); }

// Replies with a fixed "off" state to a per-receiver switch maxibitx lacks.
static const char *const fixed_off[] = {
    "tune",           "xit_enable",    "sql_enable",    "rx_nb_enable", "rx_nr_enable",
    "rx_anf_enable",  "rx_anc_enable", "rx_bin_enable", "rx_apf_enable", "rx_dse_enable",
    "rx_nf_enable",   "lock",          "vfo_lock"};

static void handle(int slot, char *cmd) {
  // "name" or "name:arg,arg,..."; spaces around any part are ignored
  char *argv[TCI_MAX_ARGS];
  int argc = 0;
  char *colon = strchr(cmd, ':');
  if (colon) {
    *colon = '\0';
    char *save = NULL;
    for (char *a = strtok_r(colon + 1, ",", &save); a && argc < TCI_MAX_ARGS;
         a = strtok_r(NULL, ",", &save)) {
      while (isspace((unsigned char)*a))
        a++;
      char *e = a + strlen(a);
      while (e > a && isspace((unsigned char)e[-1]))
        *--e = '\0';
      argv[argc++] = a;
    }
  }
  char *name = cmd;
  while (isspace((unsigned char)*name))
    name++;
  for (char *e = name + strlen(name); e > name && isspace((unsigned char)e[-1]);)
    *--e = '\0';
  for (char *p = name; *p; p++)
    *p = (char)tolower((unsigned char)*p);
  if (!*name)
    return;

  struct client *c = &clients[slot];
  struct state s;
  long long v;
  int b;
  // Most commands name a receiver/transceiver first; only 0 exists.
  int trx0 = argc >= 1 && parse_int(argv[0], &v) && v == 0;

  if (!strcmp(name, "start")) {
    send_to(slot, "start;");
  } else if (!strcmp(name, "stop")) {
    // maxibitx has no stopped state; clients send this as they close
  } else if (!strcmp(name, "vfo") && trx0 && argc >= 2) {
    long long ch = 0;
    parse_int(argv[1], &ch);
    if (argc >= 3) {
      if (ch == 0 && parse_int(argv[2], &v) && v >= TCI_VFO_MIN && v <= TCI_VFO_MAX)
        radio_tune_to((uint32_t)v);
      snapshot(&s);
      pub_freq(&s); // echo, with the frequency in force
    } else {
      send_to(slot, "vfo:0,%lld,%d;", ch, freq_hdr);
    }
  } else if (!strcmp(name, "dds") && trx0) {
    if (argc >= 2) {
      if (parse_int(argv[1], &v) && v >= TCI_VFO_MIN && v <= TCI_VFO_MAX)
        radio_tune_to((uint32_t)v);
      snapshot(&s);
      pub_freq(&s);
    } else {
      send_to(slot, "dds:0,%d;", freq_hdr);
    }
  } else if (!strcmp(name, "if") && trx0 && argc >= 2) {
    if (argc >= 3)
      broadcast("if:0,%s,0;", argv[1]); // the offset is always 0
    else
      send_to(slot, "if:0,%s,0;", argv[1]);
  } else if (!strcmp(name, "modulation") && trx0) {
    enum radio_mode m;
    if (argc >= 2 && mode_from_name(argv[1], &m))
      radio_set_mode(m);
    snapshot(&s);
    if (argc >= 2)
      pub_mode(&s);
    else
      send_to(slot, "modulation:0,%s;", mode_name(s.mode));
  } else if (!strcmp(name, "trx") && trx0) {
    if (argc >= 2 && parse_bool(argv[1], &b))
      cmd_trx(slot, b, argc >= 3 ? argv[2] : "");
    else
      send_to(slot, "trx:0,%s;", tf(in_tx));
  } else if (!strcmp(name, "drive") && trx0) {
    if (argc >= 2 && parse_int(argv[1], &v) && v >= 0 && v <= 100)
      sound_set_tx_power((double)v / 100.0);
    snapshot(&s);
    if (argc >= 2)
      pub_drive(&s);
    else
      send_to(slot, "drive:0,%d;", s.drive);
  } else if (!strcmp(name, "tune_drive") && trx0) {
    snapshot(&s);
    send_to(slot, "tune_drive:0,%d;", s.drive);
  } else if (!strcmp(name, "rit_enable") && trx0) {
    if (argc >= 2 && parse_bool(argv[1], &b))
      radio_set_rit_enabled(b);
    snapshot(&s);
    if (argc >= 2)
      pub_rit_on(&s);
    else
      send_to(slot, "rit_enable:0,%s;", tf(s.rit_on));
  } else if (!strcmp(name, "rit_offset") && trx0) {
    if (argc >= 2 && parse_int(argv[1], &v) && v >= -RIT_MAX_HZ && v <= RIT_MAX_HZ) {
      int on = radio_rit_enabled(); // TCI sets the offset and the switch separately
      radio_set_rit((int)v);
      radio_set_rit_enabled(on);
    }
    snapshot(&s);
    if (argc >= 2) {
      pub_rit(&s);
      if (s.rit_on != pub.rit_on)
        pub_rit_on(&s);
    } else {
      send_to(slot, "rit_offset:0,%d;", s.rit);
    }
  } else if (!strcmp(name, "xit_offset") && trx0) {
    if (argc >= 2)
      broadcast("xit_offset:0,0;");
    else
      send_to(slot, "xit_offset:0,0;");
  } else if (!strcmp(name, "split_enable")) {
    // Always off. WSJT-X Improved sets it with no transceiver number.
    int bare_set = argc == 1 && (!strcasecmp(argv[0], "true") || !strcasecmp(argv[0], "false"));
    if (bare_set || (argc >= 2 && trx0))
      broadcast("split_enable:0,false;");
    else if (argc == 0 || trx0)
      send_to(slot, "split_enable:0,false;");
  } else if (!strcmp(name, "volume")) {
    if (argc >= 1 && parse_int(argv[0], &v) && v >= -60 && v <= 0) {
      int percent = volume_percent((int)v);
      rx_audio_set_volume(percent);
      if (percent)
        unmute_percent = percent;
    }
    snapshot(&s);
    if (argc >= 1) {
      pub_volume(&s);
      if (s.mute != pub.mute)
        pub_mute(&s);
    } else {
      send_to(slot, "volume:%d;", s.volume_db);
    }
  } else if (!strcmp(name, "mute") || (!strcmp(name, "rx_mute") && trx0)) {
    int at = !strcmp(name, "mute") ? 0 : 1;
    if (argc > at && parse_bool(argv[at], &b)) {
      int percent = rx_audio_get_volume();
      if (b && percent > 0) {
        unmute_percent = percent;
        rx_audio_set_volume(0);
      } else if (!b && percent == 0) {
        rx_audio_set_volume(unmute_percent);
      }
    }
    snapshot(&s);
    if (argc > at) {
      pub_mute(&s);
      if (s.volume_db != pub.volume_db)
        pub_volume(&s);
    } else if (at == 0) {
      send_to(slot, "mute:%s;", tf(s.mute));
    } else {
      send_to(slot, "rx_mute:0,%s;", tf(s.mute));
    }
  } else if (!strcmp(name, "cw_macros_speed") || !strcmp(name, "cw_keyer_speed")) {
    if (argc >= 1 && parse_int(argv[0], &v) && v >= KEYER_WPM_MIN && v <= KEYER_WPM_MAX)
      keyer_set_wpm((int)v);
    snapshot(&s);
    if (argc >= 1)
      pub_wpm(&s);
    else
      send_to(slot, "%s:%d;", name, s.wpm);
  } else if (!strcmp(name, "rx_enable") && argc >= 1 && parse_int(argv[0], &v)) {
    send_to(slot, "rx_enable:%lld,%s;", v, tf(v == 0));
  } else if (!strcmp(name, "rx_channel_enable") && argc >= 2 && parse_int(argv[0], &v)) {
    long long ch = -1;
    parse_int(argv[1], &ch);
    send_to(slot, "rx_channel_enable:%lld,%lld,%s;", v, ch, tf(v == 0 && ch == 0));
  } else if (!strcmp(name, "rx_smeter") && trx0) {
    send_to(slot, "rx_smeter:0,0,%d;", -73 + rx_audio_get_strength_db());
  } else if (!strcmp(name, "rx_sensors_enable") && argc >= 1 && parse_bool(argv[0], &b)) {
    c->rx_sensors = b;
    c->rx_sensors_ms = 200;
    if (argc >= 2 && parse_int(argv[1], &v))
      c->rx_sensors_ms = v < 30 ? 30 : v > 1000 ? 1000 : (int)v;
    c->rx_sensors_due = now_ms();
  } else if (!strcmp(name, "tx_sensors_enable")) {
    // accepted; nothing is sent (a DE board has no power or SWR measurement)
  } else if ((!strcmp(name, "audio_start") || !strcmp(name, "audio_stop")) && trx0) {
    c->cfg.audio_on = !strcmp(name, "audio_start");
    apply_stream(slot);
    send_to(slot, "%s:0;", name);
  } else if ((!strcmp(name, "iq_start") || !strcmp(name, "iq_stop")) && trx0) {
    c->cfg.iq_on = !strcmp(name, "iq_start");
    apply_stream(slot);
    send_to(slot, "%s:0;", name);
  } else if (!strcmp(name, "audio_samplerate")) {
    if (argc >= 1 && parse_int(argv[0], &v) && (v == 8000 || v == 12000 || v == 24000 || v == 48000)) {
      c->cfg.audio_rate = (int)v;
      apply_stream(slot);
    }
    send_to(slot, "audio_samplerate:%d;", c->cfg.audio_rate);
  } else if (!strcmp(name, "iq_samplerate")) {
    if (argc >= 1 && parse_int(argv[0], &v) && (v == 48000 || v == 96000)) {
      c->cfg.iq_rate = (int)v;
      apply_stream(slot);
    }
    send_to(slot, "iq_samplerate:%d;", c->cfg.iq_rate);
  } else if (!strcmp(name, "audio_stream_sample_type")) {
    static const char *const types[] = {"int16", "int24", "int32", "float32"};
    for (int k = 0; argc >= 1 && k < 4; k++)
      if (!strcasecmp(argv[0], types[k]))
        c->cfg.audio_type = k;
    apply_stream(slot);
    send_to(slot, "audio_stream_sample_type:%s;", types[c->cfg.audio_type]);
  } else if (!strcmp(name, "audio_stream_channels")) {
    if (argc >= 1 && parse_int(argv[0], &v) && (v == 1 || v == 2)) {
      c->cfg.audio_channels = (int)v;
      apply_stream(slot);
    }
    send_to(slot, "audio_stream_channels:%d;", c->cfg.audio_channels);
  } else if (!strcmp(name, "audio_stream_samples")) {
    if (argc >= 1 && parse_int(argv[0], &v) && v >= 100 && v <= 2048) {
      c->cfg.audio_samples = (int)v;
      apply_stream(slot);
    }
    int shown = c->cfg.audio_samples;
    if (!shown)
      shown = c->cfg.audio_rate == 8000    ? 256
              : c->cfg.audio_rate == 12000 ? 512
              : c->cfg.audio_rate == 24000 ? 1024
                                           : 2048;
    send_to(slot, "audio_stream_samples:%d;", shown);
  } else if (!strcmp(name, "tx_stream_audio_buffering")) {
    if (argc >= 1 && parse_int(argv[0], &v) && v >= 50 && v <= 500)
      c->buffering_ms = (int)v;
    send_to(slot, "tx_stream_audio_buffering:%d;", c->buffering_ms);
  } else if (!strcmp(name, "agc_mode") && trx0) {
    send_to(slot, "agc_mode:0,normal;");
  } else if (!strcmp(name, "sql_level") && trx0) {
    send_to(slot, "sql_level:0,-140;");
  } else if ((!strcmp(name, "digl_offset") || !strcmp(name, "digu_offset"))) {
    send_to(slot, "%s:0;", name);
  } else {
    for (size_t k = 0; k < sizeof(fixed_off) / sizeof(fixed_off[0]); k++) {
      if (!strcmp(name, fixed_off[k]) && trx0) {
        if (argc >= 2)
          broadcast("%s:0,false;", name);
        else
          send_to(slot, "%s:0,false;", name);
        return;
      }
    }
    // Anything else - CW macros, spots, commands for other receivers - is
    // ignored, as the spec says of a command a server doesn't know.
  }
}

/* ---- Connection events ------------------------------------------------------ */

static void send_init(int slot) {
  struct state s;
  snapshot(&s);
  send_to(slot, "protocol:ExpertSDR3,2.0;");
  send_to(slot, "device:maxibitx;");
  send_to(slot, "receive_only:false;");
  send_to(slot, "trx_count:1;");
  send_to(slot, "channel_count:1;");
  send_to(slot, "vfo_limits:%d,%d;", TCI_VFO_MIN, TCI_VFO_MAX);
  send_to(slot, "if_limits:-48000,48000;");
  send_to(slot, "modulations_list:USB,LSB,CW,CWR,DIGU;");
  send_to(slot, "iq_samplerate:%d;", clients[slot].cfg.iq_rate);
  send_to(slot, "audio_samplerate:%d;", clients[slot].cfg.audio_rate);
  send_to(slot, "dds:0,%d;", s.freq);
  send_to(slot, "if:0,0,0;");
  send_to(slot, "vfo:0,0,%d;", s.freq);
  send_to(slot, "vfo:0,1,%d;", s.freq);
  send_to(slot, "modulation:0,%s;", mode_name(s.mode));
  send_to(slot, "rx_enable:0,true;");
  send_to(slot, "tx_enable:0,%s;", tf(s.tx_enable));
  send_to(slot, "trx:0,%s;", tf(s.tx));
  send_to(slot, "tune:0,false;");
  send_to(slot, "drive:0,%d;", s.drive);
  send_to(slot, "tune_drive:0,%d;", s.drive);
  send_to(slot, "split_enable:0,false;");
  send_to(slot, "rit_enable:0,%s;", tf(s.rit_on));
  send_to(slot, "rit_offset:0,%d;", s.rit);
  send_to(slot, "xit_enable:0,false;");
  send_to(slot, "xit_offset:0,0;");
  send_to(slot, "volume:%d;", s.volume_db);
  send_to(slot, "mute:%s;", tf(s.mute));
  send_to(slot, "rx_mute:0,%s;", tf(s.mute));
  send_to(slot, "cw_macros_speed:%d;", s.wpm);
  send_to(slot, "cw_keyer_speed:%d;", s.wpm);
  send_to(slot, "start;");
  send_to(slot, "ready;");
}

static void on_open(int id, const char *peer) {
  int slot = ws_client_slot(id);
  pthread_mutex_lock(&lock);
  struct client *c = &clients[slot];
  memset(c, 0, sizeof(*c));
  c->in_use = 1;
  c->id = id;
  c->cfg.audio_rate = 48000;
  c->cfg.audio_type = TCI_FLOAT32;
  c->cfg.audio_channels = 2;
  c->cfg.iq_rate = 96000;
  c->buffering_ms = 50;
  tci_stream_open(slot, id);
  apply_stream(slot);
  printf("tci: client %d connected from %s\n", slot, peer);
  send_init(slot);
  pthread_mutex_unlock(&lock);
}

static void on_text(int id, const char *msg, size_t len) {
  (void)len;
  int slot = ws_client_slot(id);
  pthread_mutex_lock(&lock);
  if (clients[slot].in_use && clients[slot].id == id) {
    if (trace)
      printf("tci: <- %d %s\n", slot, msg);
    char *copy = strdup(msg), *save = NULL;
    for (char *cmd = copy ? strtok_r(copy, ";", &save) : NULL; cmd; cmd = strtok_r(NULL, ";", &save))
      handle(slot, cmd);
    free(copy);
  }
  pthread_mutex_unlock(&lock);
}

static void on_binary(int id, const uint8_t *data, size_t len) {
  int slot = ws_client_slot(id);
  pthread_mutex_lock(&lock);
  if (clients[slot].in_use && clients[slot].id == id)
    tci_stream_tx_frame(slot, data, len);
  pthread_mutex_unlock(&lock);
}

static void on_close(int id) {
  int slot = ws_client_slot(id);
  pthread_mutex_lock(&lock);
  if (clients[slot].in_use && clients[slot].id == id) {
    if (tx_owner == slot) {
      if (in_tx && !cw_tx_active())
        radio_set_tx(0);
      tx_release_owner();
      printf("tci: client %d disconnected while holding TX - TX released\n", slot);
    }
    tci_stream_close(slot);
    clients[slot].in_use = 0;
    printf("tci: client %d disconnected\n", slot);
  }
  pthread_mutex_unlock(&lock);
}

/* ---- Service thread and lifetime ------------------------------------------ */

static void rx_sensors(void) {
  long long now = now_ms();
  for (int s = 0; s < WS_MAX_CLIENTS; s++) {
    struct client *c = &clients[s];
    if (!c->in_use || !c->rx_sensors || now < c->rx_sensors_due)
      continue;
    double dbm = -73.0 + rx_audio_get_strength_db(); // S9 = -73 dBm; uncalibrated
    send_to(s, "rx_sensors:0,%.1f;", dbm);
    send_to(s, "rx_channel_sensors:0,0,%.1f;", dbm);
    c->rx_sensors_due = now + c->rx_sensors_ms;
  }
}

static void *service_main(void *arg) {
  (void)arg;
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  for (int tick = 0; atomic_load(&running); tick++) {
    next.tv_nsec += TCI_TICK_MS * 1000000L;
    if (next.tv_nsec >= 1000000000L) {
      next.tv_nsec -= 1000000000L;
      next.tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    pthread_mutex_lock(&lock);
    tci_stream_service();
    if (tick % TCI_STATE_TICKS == 0) {
      if (tx_owner >= 0 && !in_tx)
        tx_release_owner(); // TX dropped elsewhere
      publish_changes();
      rx_sensors();
    }
    pthread_mutex_unlock(&lock);
  }
  return NULL;
}

int tci_init(const char *bind_addr, int port, int max_clients) {
  static const struct ws_callbacks cb = {on_open, on_text, on_binary, on_close};
  const char *t = getenv("MAXIBITX_TCI_TRACE");
  trace = t && *t && strcmp(t, "0");
  tci_stream_init();
  snapshot(&pub);
  if (pub.volume_db > -60)
    unmute_percent = rx_audio_get_volume();
  if (ws_server_start(bind_addr, port, max_clients, &cb) < 0)
    return -1;
  atomic_store(&running, 1);
  if (pthread_create(&service_thread, NULL, service_main, NULL) != 0) {
    atomic_store(&running, 0);
    ws_server_stop();
    return -1;
  }
  return 0;
}

void tci_stop(void) {
  if (!atomic_load(&running))
    return;
  ws_server_stop();
  atomic_store(&running, 0);
  pthread_join(service_thread, NULL);
}
