// radio_hw_test.c
//
// Bench harness for the board layer: hw_settings.c's sbitx_version and
// i2c_bus keys, and radio_hw.c's two board profiles, with gpio.c and i2c.c
// stubbed so every line request and write is recorded rather than made.
// Checks which pins each board claims and at what level, LPF selection on a
// retune, the relay half of the T/R sequence in both directions, that the
// zBitx can neither be permitted to transmit nor raise TX_LINE, and each
// board's front-panel knobs: their pins, and that no knob pin is one the
// board drives, the key jack's, or the sBitx si5351's I2C pins.
// docs/dsp_design_notes/zbitx_port_study.md §5 and §9.
//
//   make test-radio-hw && ./test-radio-hw

#include "gpio.h"
#include "hw_settings.h"
#include "i2c.h"
#include "radio.h"
#include "radio_hw.h"
#include "si5351.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- Stubs ------------------------------------------------------------------

int bfo_freq = 40035000;
int xtal_filter_center = 40012400;
void si5351_set_calibration(int32_t cal) { (void)cal; }

#define PINS 28
static int claimed[PINS];  // 1 once requested
static int level[PINS];    // last level requested or written
static int writes[64][2];  // {pin, level} in order, since the last reset
static int n_writes;

static void reset_pins(void) {
  memset(claimed, 0, sizeof(claimed));
  memset(level, 0, sizeof(level));
  n_writes = 0;
}

int gpio_request_output(unsigned int bcm_gpio, int initial_value, const char *consumer) {
  (void)consumer;
  if (bcm_gpio >= PINS || claimed[bcm_gpio])
    return -1;
  claimed[bcm_gpio] = 1;
  level[bcm_gpio] = initial_value;
  return 100 + (int)bcm_gpio;
}

int gpio_write(int line, int value) {
  int pin = line - 100;
  level[pin] = value;
  if (n_writes < 64) {
    writes[n_writes][0] = pin;
    writes[n_writes][1] = value;
    n_writes++;
  }
  return 0;
}

int32_t i2c_write_i2c_block_data(uint8_t a, uint8_t c, uint8_t n, const uint8_t *v) {
  (void)a; (void)c; (void)n; (void)v;
  return -1;
}
int32_t i2c_read_i2c_block_data(uint8_t a, uint8_t c, uint8_t n, uint8_t *v) {
  (void)a; (void)c; (void)n; (void)v;
  return -1;
}

// ---- Checks -----------------------------------------------------------------

static int failures;

static void check(int ok, const char *what) {
  printf("  %s  %s\n", ok ? "pass" : "FAIL", what);
  if (!ok)
    failures++;
}

// Pins at level 1 among the claimed ones, as a bitmask.
static unsigned high_pins(void) {
  unsigned m = 0;
  for (int p = 0; p < PINS; p++)
    if (claimed[p] && level[p])
      m |= 1u << p;
  return m;
}

static unsigned claimed_pins(void) {
  unsigned m = 0;
  for (int p = 0; p < PINS; p++)
    if (claimed[p])
      m |= 1u << p;
  return m;
}

#define BIT(p) (1u << (p))

// Writes data/hw_settings.ini under a fresh directory, changes into it and
// loads it. NULL: no file at all.
static void load_ini(const char *text) {
  char dir[] = "/tmp/radio_hw_test_XXXXXX";
  if (!mkdtemp(dir) || chdir(dir) != 0) {
    perror("radio_hw_test: temp dir");
    exit(2);
  }
  if (text) {
    mkdir("data", 0755);
    FILE *f = fopen("data/hw_settings.ini", "w");
    fputs(text, f);
    fclose(f);
  }
  hw_settings_load();
  unlink("data/hw_settings.ini");
  rmdir("data");
  if (chdir("/") != 0 || rmdir(dir) != 0)
    perror("radio_hw_test: removing temp dir");
}

int main(void) {
  printf("A. hw_settings.ini: naming the board\n");
  load_ini("sbitx_version = SBITX_V4\ni2c_bus = 5\n[tcxo]\ncal=25000000\n");
  check(!strcmp(sbitx_version, "SBITX_V4"), "\"sbitx_version = SBITX_V4\" is read");
  check(hw_i2c_bus == 5, "\"i2c_bus = 5\" is read");
  load_ini("sbitx_version=SBITX_V3\n");
  check(!strcmp(sbitx_version, "SBITX_V3") && hw_i2c_bus == -1,
        "\"sbitx_version=SBITX_V3\" is read, i2c_bus absent is -1");
  load_ini("sbitx_verrsion = SBITX_V4\n");
  check(sbitx_version[0] == 0, "a misspelt key names no board");
  load_ini("hw=4\n");
  check(sbitx_version[0] == 0, "zbitx's hw=4 names no board");
  load_ini("[tx_band]\nsbitx_version = SBITX_V4\n");
  check(sbitx_version[0] == 0, "sbitx_version inside a [section] is ignored");
  load_ini(NULL);
  check(sbitx_version[0] == 0, "no hw_settings.ini names no board");

  printf("B. Selecting the board\n");
  check(radio_hw_select_board("") < 0, "an empty name is refused");
  check(radio_hw_select_board("SBITX_V5") < 0, "an unknown name is refused");
  check(radio_hw_select_board("sbitx_v4") < 0, "names are exact, case included");

  printf("C. zBitx (SBITX_V4)\n");
  hw_i2c_bus = -1;
  check(radio_hw_select_board("SBITX_V4") == 0, "SBITX_V4 is accepted");
  check(!strcmp(radio_hw_board_name(), "zBitx"), "it is the zBitx");
  check(radio_hw_i2c_bus() == 3, "the si5351 on bus 3 by default");
  hw_i2c_bus = 1;
  check(radio_hw_i2c_bus() == 1, "i2c_bus overrides it");
  hw_i2c_bus = -1;
  check(!radio_hw_tx_permitted(), "transmit is not permitted");
  reset_pins();
  check(radio_hw_gpio_init() == 0, "GPIO lines claimed");
  check(claimed_pins() ==
            (BIT(23) | BIT(16) | BIT(15) | BIT(24) | BIT(25) | BIT(8) | BIT(7) | BIT(12)),
        "claims TX_LINE 23, TX_POWER 16, RX_LINE 15, LPFs 24, 25, 8, 7 and BCM 12 (held low)");
  check(high_pins() == BIT(15), "only RX_LINE is high: receiver connected, all else off");
  n_writes = 0;
  radio_hw_tune(21200000);
  radio_hw_tune(7030000);
  check(n_writes == 0 && high_pins() == BIT(15), "a retune selects no LPF (not in the RX path)");
  check(radio_hw_relays_tx(1, 14060000) < 0, "the relays refuse to go to transmit");
  check(n_writes == 0 && high_pins() == BIT(15), "... and nothing was switched");
  check(radio_hw_relays_tx(0, 14060000) == 0, "the relays go to receive");
  check(high_pins() == BIT(15) && level[23] == 0, "... TX_LINE low, LPFs off, RX_LINE high");
  check(radio_hw_tx_settle_ms() == 10, "relay wait is the LPF's 10 ms");
  check(radio_hw_knob(RADIO_KNOB_TUNING).a_pin < 0 && radio_hw_knob(RADIO_KNOB_VOLUME).a_pin < 0,
        "no knobs on the Pi's GPIO (the RP2040 panel has them)");

  printf("D. sBitx (SBITX_V3)\n");
  check(radio_hw_select_board("SBITX_V3") == 0, "SBITX_V3 is accepted");
  check(radio_hw_i2c_bus() == 22, "the si5351 on bus 22 by default");
  check(radio_hw_tx_permitted(), "transmit is permitted");
  reset_pins();
  check(radio_hw_gpio_init() == 0, "GPIO lines claimed");
  check(claimed_pins() == (BIT(23) | BIT(16) | BIT(12) | BIT(24) | BIT(25) | BIT(8) | BIT(7)),
        "claims TX_LINE 23, TX_POWER 16, EXT_PTT 12 and LPFs 24, 25, 8, 7");
  check(high_pins() == 0, "all low");
  static const struct { int hz; unsigned high; const char *what; } tunes[] = {
      {3573000, BIT(7), "3.573 MHz: LPF_D (7)"},
      {7030000, BIT(8), "7.030 MHz: LPF_C (8)"},
      {14060000, BIT(25), "14.060 MHz: LPF_B (25)"},
      {21060000, BIT(24), "21.060 MHz: LPF_A (24), above 18.5 MHz"},
      {28060000, BIT(24), "28.060 MHz: LPF_A (24)"},
      {31000000, 0, "31 MHz: none"},
      {7074000, BIT(8), "back to 7.074 MHz: LPF_C (8)"},
  };
  for (size_t i = 0; i < sizeof(tunes) / sizeof(tunes[0]); i++) {
    radio_hw_tune(tunes[i].hz);
    check(high_pins() == tunes[i].high, tunes[i].what);
  }
  tx_ext_ptt_delay_ms = 0;
  n_writes = 0;
  check(radio_hw_relays_tx(1, 7074000) == 0, "the relays go to transmit");
  check(n_writes == 2 && writes[0][0] == 12 && writes[0][1] == 1 && writes[1][0] == 23 &&
            writes[1][1] == 1,
        "... EXT_PTT high, then TX_LINE high");
  n_writes = 0;
  check(radio_hw_relays_tx(0, 7074000) == 0, "the relays go to receive");
  check(n_writes == 2 && writes[0][0] == 12 && writes[0][1] == 0 && writes[1][0] == 23 &&
            writes[1][1] == 0,
        "... EXT_PTT low, then TX_LINE low");
  check(high_pins() == BIT(8), "the band's LPF stays selected through T/R");
  tx_ext_ptt_delay_ms = 20;
  check(radio_hw_tx_settle_ms() == 20, "relay wait is ext_ptt_delay_ms");

  struct radio_hw_knob tune = radio_hw_knob(RADIO_KNOB_TUNING);
  struct radio_hw_knob vol = radio_hw_knob(RADIO_KNOB_VOLUME);
  check(tune.a_pin == 9 && tune.b_pin == 10 && tune.sw_pin == 11 && tune.edges_per_detent == 4,
        "tuning knob: A 9, B 10, switch 11 (wiringPi 13, 12, 14)");
  check(vol.a_pin == 17 && vol.b_pin == 27 && vol.sw_pin == 22 && vol.edges_per_detent == 4,
        "volume knob: A 17, B 27, switch 22 (wiringPi 0, 2, 3)");
  unsigned knob_pins = BIT(tune.a_pin) | BIT(tune.b_pin) | BIT(tune.sw_pin) | BIT(vol.a_pin) |
                       BIT(vol.b_pin) | BIT(vol.sw_pin);
  unsigned other = claimed_pins() | BIT(KEY_TIP_GPIO) | BIT(KEY_RING_GPIO) | BIT(13) | BIT(6);
  check((knob_pins & other) == 0 && __builtin_popcount(knob_pins) == 6,
        "six distinct knob pins, none driven, the key's, or the si5351 bus's (13, 6)");
  check(radio_hw_knob(RADIO_KNOBS).a_pin < 0, "an out-of-range knob has no pins");

  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
