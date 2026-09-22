// hw_settings.c
//
// Reader for data/hw_settings.ini. That file is the same file sbitx
// uses, so 'cal' (the si5351 reference), 'bfo_freq' and the per-band
// 'scale' table carry over. 'xtal_filter_center' is new here (not read
// by real sbitx), added alongside bfo_freq for the same reason - all of
// them are board-specific measurements, not source-code constants.
// Without the file, the compiled-in defaults apply.

#include "hw_settings.h"
#include "si5351.h" // si5351_set_calibration() - the "cal" key below
#include "radio.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HW_SETTINGS_PATH "data/hw_settings.ini"

struct tx_band_scale tx_band_scales[HW_MAX_TX_BANDS];
int tx_band_scale_count = 0;

// Section state while scanning the file - only [tx_band] sections are
// acted on today; [tcxo] and any others are recognized (so their key=value
// lines aren't mistaken for top-level keys) but not yet applied.
enum hw_section { HW_SECTION_TOP, HW_SECTION_TCXO, HW_SECTION_TX_BAND, HW_SECTION_OTHER };

void hw_settings_load(void) {
  tx_band_scale_count = 0;

  FILE *f = fopen(HW_SETTINGS_PATH, "r");
  if (!f) {
    printf("init: %s not found, using compiled-in defaults\n", HW_SETTINGS_PATH);
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
    if (sscanf(p, "%63[^=]=%ld", key, &value) != 2)
      continue;

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
      }
      // ssb_val and any other top-level keys: read past, not applied yet.
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
}

double hw_settings_tx_scale(int freq_hz) {
  for (int i = 0; i < tx_band_scale_count; i++) {
    if (freq_hz >= tx_band_scales[i].f_start && freq_hz <= tx_band_scales[i].f_stop)
      return tx_band_scales[i].scale;
  }
  return HW_DEFAULT_TX_SCALE;
}
