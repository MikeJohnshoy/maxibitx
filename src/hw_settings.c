// hw_settings.c
//
// Reader for data/hw_settings.ini. That file is the same file sbitx
// uses, so 'cal' (the si5351 reference), 'bfo_freq' and the per-band
// 'scale' table carry over. 'xtal_filter_center' is new here (not read
// by real sbitx), added alongside bfo_freq for the same reason - all of
// them are board-specific measurements, not source-code constants.
// 'sbitx_version' names the radio board, and is required: maxibitx.c
// will not start without it (radio_hw_select_board()). Other keys the
// file leaves out take their compiled-in defaults.

#include "hw_settings.h"
#include "si5351.h" // si5351_set_calibration() - the "cal" key below
#include "radio.h"
#include "tci.h" // TCI_DEFAULT_PORT, TCI_DEFAULT_MAX_CLIENTS
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HW_SETTINGS_PATH "data/hw_settings.ini"

struct tx_band_scale tx_band_scales[HW_MAX_TX_BANDS];
int tx_band_scale_count = 0;

double tx_full_scale_power = HW_DEFAULT_FULL_SCALE_POWER;
double tx_max_power = HW_DEFAULT_MAX_POWER;
int tx_ext_ptt_delay_ms = HW_DEFAULT_EXT_PTT_DELAY_MS;
int key_debounce_ms = HW_DEFAULT_KEY_DEBOUNCE_MS;
int tci_port = TCI_DEFAULT_PORT;
int tci_max_clients = TCI_DEFAULT_MAX_CLIENTS;
char tci_bind[64] = "";
char sbitx_version[32] = "";
int hw_i2c_bus = -1;

// Section state while scanning the file - only [tx_band] sections are
// acted on today; [tcxo] and any others are recognized (so their key=value
// lines aren't mistaken for top-level keys) but not yet applied.
enum hw_section { HW_SECTION_TOP, HW_SECTION_TCXO, HW_SECTION_TX_BAND, HW_SECTION_OTHER };

// Keys that only take effect above the first [section]. One written further
// down - most easily by appending it to the end of the file, which is inside
// the last [tx_band] - would otherwise be skipped without a word.
static int is_top_level_key(const char *key) {
  static const char *const keys[] = { "bfo_freq", "xtal_filter_center", "full_scale_power",
                                      "max_power", "ext_ptt_delay_ms", "key_debounce_ms",
                                      "tci_port", "tci_max_clients", "tci_bind",
                                      "sbitx_version", "i2c_bus" };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
    if (!strcmp(key, keys[i]))
      return 1;
  return 0;
}

double hw_settings_power_ratio(void) {
  if (tx_full_scale_power <= 0.0 || tx_max_power <= 0.0)
    return 1.0;
  double ratio = tx_max_power / tx_full_scale_power;
  if (ratio >= 1.0)
    return 1.0; // a ceiling above full scale is just full scale
  return sqrt(ratio);
}

void hw_settings_load(void) {
  tx_band_scale_count = 0;
  sbitx_version[0] = '\0';
  hw_i2c_bus = -1;

  FILE *f = fopen(HW_SETTINGS_PATH, "r");
  if (!f) {
    printf("init: %s not found\n", HW_SETTINGS_PATH);
    return;
  }
  printf("init: %s found, reading calibration settings\n", HW_SETTINGS_PATH);

  enum hw_section section = HW_SECTION_TOP;
  char line[256];

  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    while (isspace((unsigned char)*p))
      p++;

    if (*p == '#' || *p == '\0' || *p == '\n')
      continue; // comment or blank line

    if (*p == '[') {
      char name[32];
      if (sscanf(p, "[%31[^]]]", name) == 1) {
        if (!strcmp(name, "tx_band")) {
          section = HW_SECTION_TX_BAND;
          if (tx_band_scale_count < HW_MAX_TX_BANDS) {
            memset(&tx_band_scales[tx_band_scale_count], 0, sizeof(tx_band_scales[0]));
          } else {
            printf("init: %s has more than %d [tx_band] sections, "
                   "ignoring the rest\n",
                   HW_SETTINGS_PATH, HW_MAX_TX_BANDS);
          }
        } else if (!strcmp(name, "tcxo")) {
          section = HW_SECTION_TCXO; // where sbitx's own file puts 'cal'
        } else {
          section = HW_SECTION_OTHER;
        }
      }
      continue;
    }

    char key[64];
    long value;
    char text[64];
    // tci_bind and sbitx_version are the keys whose values aren't numbers
    if (sscanf(p, " %63[^= \t] = %63s", key, text) == 2 &&
        (!strcmp(key, "tci_bind") || !strcmp(key, "sbitx_version"))) {
      if (section != HW_SECTION_TOP) {
        printf("init: %s in %s is inside a [section], so it is ignored - move it above "
               "the first [section]\n", key, HW_SETTINGS_PATH);
      } else if (!strcmp(key, "tci_bind")) {
        memcpy(tci_bind, text, sizeof(tci_bind));
        printf("init: tci_bind loaded from %s: %s\n", HW_SETTINGS_PATH, tci_bind);
      } else {
        memcpy(sbitx_version, text, sizeof(sbitx_version) - 1); // text is NUL-terminated
        sbitx_version[sizeof(sbitx_version) - 1] = '\0';
        printf("init: sbitx_version loaded from %s: %s\n", HW_SETTINGS_PATH, sbitx_version);
      }
      continue;
    }
    // Lines that look like an attempt at naming the board, for the message
    // maxibitx.c prints when sbitx_version is missing.
    if (sscanf(p, " %63[^= \t] =", key) == 1 && section == HW_SECTION_TOP) {
      if (!strcmp(key, "hw"))
        printf("init: %s has zbitx's hw= key, which maxibitx doesn't read - it needs an "
               "sbitx_version line\n", HW_SETTINGS_PATH);
      else if (strstr(key, "version") || strstr(key, "sbitx"))
        printf("init: %s has a key \"%s\" that maxibitx doesn't read - a misspelling of "
               "sbitx_version?\n", HW_SETTINGS_PATH, key);
    }
    if (sscanf(p, "%63[^=]=%ld", key, &value) != 2)
      continue;
    // "key = value" as well as "key=value": the scan above stops at '=', so
    // any space before it is still on the key, where it would stop the
    // comparisons below from matching and the line would be skipped silently.
    for (size_t k = strlen(key); k > 0 && isspace((unsigned char)key[k - 1]); k--)
      key[k - 1] = '\0';

    if (!strcmp(key, "cal") && (section == HW_SECTION_TCXO || section == HW_SECTION_TOP)) {
      // The si5351's reference frequency as measured on this board, the
      // same key and units sbitx uses (nominal 25,000,000 for the TCXO).
      // sbitx's own file puts it under [tcxo]; accepted at the top level
      // too. Every clock is derived from it, so an error here moves TX and
      // RX in opposite directions, both proportional to frequency. Procedure:
      // dsp_design_notes/tx_test_tones_and_alc.md, "Frequency calibration".
      si5351_set_calibration((int32_t)value);
      printf("init: si5351 reference calibration loaded from %s: %ld Hz "
             "(%+.2f ppm from nominal)\n",
             HW_SETTINGS_PATH, value, (value - 25000000.0) / 25.0);
    } else if (section == HW_SECTION_TOP) {
      if (!strcmp(key, "bfo_freq")) {
        bfo_freq = (int)value;
        printf("init: bfo_freq loaded from %s: %d Hz\n", HW_SETTINGS_PATH, bfo_freq);
      } else if (!strcmp(key, "xtal_filter_center")) {
        xtal_filter_center = (int)value;
        printf("init: xtal_filter_center loaded from %s: %d Hz\n",
               HW_SETTINGS_PATH, xtal_filter_center);
      } else if (!strcmp(key, "ext_ptt_delay_ms")) {
        long ms = value;
        if (ms < 0)
          ms = 0;
        if (ms > HW_MAX_EXT_PTT_DELAY_MS)
          ms = HW_MAX_EXT_PTT_DELAY_MS;
        tx_ext_ptt_delay_ms = (int)ms;
        if (ms != value)
          printf("init: ext_ptt_delay_ms=%ld in %s is outside 0-%d - using %ld ms\n", value,
                 HW_SETTINGS_PATH, HW_MAX_EXT_PTT_DELAY_MS, ms);
        else if (ms == 0)
          printf("init: ext_ptt_delay_ms loaded from %s: 0 ms - on an sBitx, no settling "
                 "time for an amplifier on EXT_PTT, only safe with nothing connected there\n",
                 HW_SETTINGS_PATH);
        else
          printf("init: ext_ptt_delay_ms loaded from %s: %ld ms\n", HW_SETTINGS_PATH, ms);
      } else if (!strcmp(key, "key_debounce_ms")) {
        long ms = value;
        if (ms < 0)
          ms = 0;
        if (ms > HW_MAX_KEY_DEBOUNCE_MS)
          ms = HW_MAX_KEY_DEBOUNCE_MS;
        key_debounce_ms = (int)ms;
        if (ms != value)
          printf("init: key_debounce_ms=%ld in %s is outside 0-%d - using %ld ms\n", value,
                 HW_SETTINGS_PATH, HW_MAX_KEY_DEBOUNCE_MS, ms);
        else
          printf("init: key_debounce_ms loaded from %s: %ld ms\n", HW_SETTINGS_PATH, ms);
      } else if (!strcmp(key, "i2c_bus")) {
        if (value >= 0 && value < 256) {
          hw_i2c_bus = (int)value;
          printf("init: i2c_bus loaded from %s: %d\n", HW_SETTINGS_PATH, hw_i2c_bus);
        } else {
          printf("init: i2c_bus=%ld in %s isn't a bus number - using the board's\n", value,
                 HW_SETTINGS_PATH);
        }
      } else if (!strcmp(key, "tci_port")) {
        if (value >= 0 && value <= 65535) {
          tci_port = (int)value;
          printf("init: tci_port loaded from %s: %d%s\n", HW_SETTINGS_PATH, tci_port,
                 tci_port ? "" : " - TCI server off");
        } else {
          printf("init: tci_port=%ld in %s isn't a TCP port - using %d\n", value, HW_SETTINGS_PATH,
                 tci_port);
        }
      } else if (!strcmp(key, "tci_max_clients")) {
        long n = value < 1 ? 1 : value > HW_MAX_TCI_CLIENTS ? HW_MAX_TCI_CLIENTS : value;
        tci_max_clients = (int)n;
        if (n != value)
          printf("init: tci_max_clients=%ld in %s is outside 1-%d - using %ld\n", value,
                 HW_SETTINGS_PATH, HW_MAX_TCI_CLIENTS, n);
        else
          printf("init: tci_max_clients loaded from %s: %ld\n", HW_SETTINGS_PATH, n);
      } else if (!strcmp(key, "full_scale_power") || !strcmp(key, "max_power")) {
        // Watts, and fractional on a real board (5.5) - re-parse as a
        // double, the %ld above only captured the integer truncation.
        // What each one means: hw_settings.h, "TX power ceiling".
        double watts;
        if (sscanf(p, "%63[^=]=%lf", key, &watts) == 2 && watts > 0.0) {
          if (!strcmp(key, "full_scale_power"))
            tx_full_scale_power = watts;
          else
            tx_max_power = watts;
          printf("init: %s loaded from %s: %.2f W\n", key, HW_SETTINGS_PATH, watts);
        }
      }
      // ssb_val and any other top-level keys: read past, not applied yet.
    } else if (is_top_level_key(key)) {
      printf("init: %s in %s is inside a [section], so it is ignored - move it above "
             "the first [section]\n", key, HW_SETTINGS_PATH);
    } else if (section == HW_SECTION_TX_BAND && tx_band_scale_count < HW_MAX_TX_BANDS) {
      struct tx_band_scale *b = &tx_band_scales[tx_band_scale_count];
      if (!strcmp(key, "f_start")) {
        b->f_start = (int)value;
      } else if (!strcmp(key, "f_stop")) {
        b->f_stop = (int)value;
      } else if (!strcmp(key, "scale")) {
        // scale is a fraction (e.g. 0.00115) - re-parse as a double,
        // the %ld above only captured its integer truncation.
        double dval;
        if (sscanf(p, "%63[^=]=%lf", key, &dval) == 2) {
          b->scale = dval;
          // A [tx_band] section's three keys (f_start, f_stop,
          // scale) always appear together in hw_settings.ini,
          // with 'scale' last - commit the entry once we have it.
          if (b->f_start || b->f_stop)
            tx_band_scale_count++;
        }
      }
    }
    // ssb_val and any other top-level keys: read past, not applied yet.
    // HW_SECTION_OTHER (e.g. [tcxo]): keys read past, not applied yet.
  }

  fclose(f);

  if (tx_band_scale_count > 0) {
    printf("init: %d TX band scale entries loaded from %s\n", tx_band_scale_count,
           HW_SETTINGS_PATH);
  }

  if (tx_max_power > tx_full_scale_power) {
    printf("init: max_power (%.2f W) is above full_scale_power (%.2f W) - "
           "the limiter can only reduce, so the ceiling is rated output\n",
           tx_max_power, tx_full_scale_power);
  }
  double ratio = hw_settings_power_ratio();
  if (ratio >= 1.0) {
    printf("init: TX ceiling is rated output, %.2f W - the limiter holds full "
           "scale and pulls back anything driven past it\n",
           tx_full_scale_power);
  } else {
    printf("init: TX ceiling %.2f W of %.2f W rated (limiter at %.3f of full "
           "amplitude, %.1f dB below rated)\n",
           tx_max_power, tx_full_scale_power, ratio, -20.0 * log10(ratio));
  }
}

double hw_settings_tx_scale(int freq_hz) {
  for (int i = 0; i < tx_band_scale_count; i++) {
    if (freq_hz >= tx_band_scales[i].f_start && freq_hz <= tx_band_scales[i].f_stop)
      return tx_band_scales[i].scale;
  }
  return HW_DEFAULT_TX_SCALE;
}

int hw_settings_tx_allowed(int freq_hz) {
  if (tx_band_scale_count == 0)
    return 1; // nothing calibrated, so nothing to enforce - hw_settings.h
  for (int i = 0; i < tx_band_scale_count; i++) {
    if (freq_hz >= tx_band_scales[i].f_start && freq_hz <= tx_band_scales[i].f_stop)
      return 1;
  }
  return 0;
}
