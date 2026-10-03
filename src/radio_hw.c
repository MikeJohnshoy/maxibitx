// radio_hw.c
//
// The radio board, and every difference between the boards maxibitx runs
// on: one profile per board (boards[] below), selected by hw_settings.ini's
// sbitx_version line, and the operations radio_hw.h offers on top of it.
// Pins are BCM numbers, as gpio.c takes them.
// docs/dsp_design_notes/zbitx_port_study.md §3 and §5.

#include "gpio.h"
#include "hw_settings.h" // tx_ext_ptt_delay_ms, hw_i2c_bus
#include "i2c.h"
#include "radio_hw.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- INA260 power monitor (I2C address 0x40) -------------------------- */
#define INA260_ADDRESS 0x40
#define CONFIG_REGISTER 0x00
#define VOLTAGE_REGISTER 0x02
#define CURRENT_REGISTER 0x01
#define CONFIG_DEFAULT 0x6127 // Continuous mode, default averaging

/* ---- Board profiles ----------------------------------------------------- */

#define MAX_LPF 5

struct lpf_band {
  int below_hz; // this filter serves frequencies below this; 0 ends the plan
  int pin;
};

struct board {
  const char *version; // hw_settings.ini's sbitx_version value
  const char *name;
  int tx_permitted;    // 0: radio_set_tx() refuses every transmit request
  int i2c_bus;         // the si5351's bus, unless hw_settings.ini says otherwise
  int tx_line_pin;     // T/R relay; on the zBitx it also powers the PA
  int tx_power_pin;    // held low; purpose unconfirmed in sbitx
  int ext_ptt_pin;     // PTT out to an external amplifier, -1 if none
  int rx_line_pin;     // connects the receiver when high, -1 if none
  int lpf_pins[MAX_LPF + 1]; // every LPF relay line, -1 ends the list
  struct lpf_band lpf[MAX_LPF + 1];
  int lpf_in_rx_path;  // 1: the band's LPF is selected on every retune
  int (*relays_tx)(int on, int freq_hz);
};

static int sbitx_relays_tx(int on, int freq_hz);
static int zbitx_relays_tx(int on, int freq_hz);

// LPF_A..E are sbitx's names: A 24, B 25, C 8 and D 7 on both boards (C
// and D share pins with SPI0's chip selects, unused as SPI here), E 12 on
// the zBitx, where the sBitx has EXT_PTT.
static const struct board boards[] = {
    {
        .version = "SBITX_V3",
        .name = "sBitx (DE, v2 or v3)",
        .tx_permitted = 1,
        // The si5351 shares the RTC's bus (GPIO13/GPIO6), which the
        // i2c-rtc-gpio overlay makes bus 22 (`i2cdetect -l`, then
        // `i2cdetect -y 22` shows it at 0x60). The kernel can renumber it.
        .i2c_bus = 22,
        .tx_line_pin = 23,
        .tx_power_pin = 16,
        .ext_ptt_pin = 12,
        .rx_line_pin = -1,
        .lpf_pins = {24, 25, 8, 7, -1},
        .lpf = {{5500000, 7}, {10500000, 8}, {18500000, 25}, {30000000, 24}, {0, 0}},
        .lpf_in_rx_path = 1,
        .relays_tx = sbitx_relays_tx,
    },
    {
        // zbitx (drexjj/zbitx, branches dev and devcwmod): LPF_B reaches
        // 21.5 MHz, LPF_E is held off and never selected, and the LPFs are
        // switched for transmit only. Transmit is refused until its
        // sequence is written and the board calibrated (study §7, step 2).
        .version = "SBITX_V4",
        .name = "zBitx",
        .tx_permitted = 0,
        .i2c_bus = 3, // zbitx opens /dev/i2c-3; the RP2040 panel is at 0x0a there
        .tx_line_pin = 23,
        .tx_power_pin = 16,
        .ext_ptt_pin = -1,
        .rx_line_pin = 15, // the UART's RXD pin: the UART must be off
        .lpf_pins = {24, 25, 8, 7, 12, -1},
        .lpf = {{5500000, 7}, {10500000, 8}, {21500000, 25}, {30000000, 24}, {0, 0}},
        .lpf_in_rx_path = 0,
        .relays_tx = zbitx_relays_tx,
    },
};

#define BOARD_COUNT ((int)(sizeof(boards) / sizeof(boards[0])))

static const struct board *board = NULL;

// The LPF relay's settling time before TX_LINE on a board whose LPFs are
// switched at transmit, as zbitx waits.
#define LPF_SETTLE_MS 10
// Relay settling between the PTT/LPF lines and the T/R line going back to
// receive.
#define RX_RELAY_SETTLE_MS 5

int radio_hw_select_board(const char *sbitx_version) {
  for (int i = 0; sbitx_version && i < BOARD_COUNT; i++) {
    if (!strcmp(sbitx_version, boards[i].version)) {
      board = &boards[i];
      return 0;
    }
  }
  fprintf(stderr, "init: hw_settings.ini must name the radio board with one of:\n");
  for (int i = 0; i < BOARD_COUNT; i++)
    fprintf(stderr, "init:     sbitx_version = %s    (%s)\n", boards[i].version, boards[i].name);
  if (sbitx_version && sbitx_version[0])
    fprintf(stderr, "init: it names \"%s\", which is none of these\n", sbitx_version);
  return -1;
}

const char *radio_hw_board_name(void) { return board ? board->name : "none"; }

int radio_hw_i2c_bus(void) { return hw_i2c_bus >= 0 ? hw_i2c_bus : board->i2c_bus; }

int radio_hw_tx_permitted(void) { return board && board->tx_permitted; }

int radio_hw_tx_settle_ms(void) {
  if (board->ext_ptt_pin >= 0)
    return tx_ext_ptt_delay_ms;
  return board->rx_line_pin >= 0 ? LPF_SETTLE_MS : 0;
}

/* ---- GPIO lines --------------------------------------------------------- */
//
// One line-request handle per claimed pin, held for the life of the
// process, indexed by BCM pin number. -1: not claimed on this board, and
// drive() on it does nothing.

#define MAX_BCM_PIN 28
static int line[MAX_BCM_PIN];

static int claim(int pin, int value, const char *label) {
  if (pin < 0)
    return 0;
  line[pin] = gpio_request_output((unsigned)pin, value, label);
  return line[pin] < 0 ? -1 : 0;
}

static void drive(int pin, int value) {
  if (pin >= 0 && line[pin] >= 0)
    gpio_write(line[pin], value);
}

static int prev_lpf = -1; // pin last selected; 0: none; -1: none since start-up

int radio_hw_gpio_init(void) {
  for (int i = 0; i < MAX_BCM_PIN; i++)
    line[i] = -1;
  prev_lpf = -1;

  // Each line is driven to its receive state as part of its request, so it
  // never holds whatever the pin's power-on default was.
  int err = 0;
  err |= claim(board->tx_line_pin, 0, "maxibitx-tx_line");
  err |= claim(board->tx_power_pin, 0, "maxibitx-tx_power");
  err |= claim(board->ext_ptt_pin, 0, "maxibitx-ext_ptt");
  err |= claim(board->rx_line_pin, 1, "maxibitx-rx_line");
  for (int i = 0; board->lpf_pins[i] >= 0; i++)
    err |= claim(board->lpf_pins[i], 0, "maxibitx-lpf");
  if (err)
    return -1; // gpio_request_output() already logged which pin and why

  printf("init: GPIO configured for the %s: T/R relay%s held low, LPFs off%s\n", board->name,
         board->ext_ptt_pin >= 0 ? " and EXT_PTT" : "",
         board->rx_line_pin >= 0 ? ", receiver connected (RX_LINE high)" : "");
  return 0;
}

/* ---- Low-pass filter band switching -------------------------------------- */

static void lpfs_off(void) {
  for (int i = 0; board->lpf_pins[i] >= 0; i++)
    drive(board->lpf_pins[i], 0);
}

// Selects the LPF for freq_hz, all others off. 30 MHz and above: none.
static void select_lpf(int freq_hz) {
  int pin = 0;
  for (int i = 0; board->lpf[i].below_hz; i++) {
    if (freq_hz < board->lpf[i].below_hz) {
      pin = board->lpf[i].pin;
      break;
    }
  }
  if (pin == prev_lpf)
    return;
  lpfs_off();
  if (pin)
    drive(pin, 1);
  prev_lpf = pin;
  printf("LPF: selected pin %d for %d Hz\n", pin, freq_hz);
}

void radio_hw_tune(int freq_hz) {
  if (board->lpf_in_rx_path)
    select_lpf(freq_hz);
}

/* ---- Relay half of the T/R sequence -------------------------------------- */

int radio_hw_relays_tx(int on, int freq_hz) {
  if (on && !board->tx_permitted)
    return -1;
  return board->relays_tx(on, freq_hz);
}

// sBitx: the LPF is already selected for the band (it is in the receive
// path too). EXT_PTT first and TX_LINE after hw_settings.ini's
// ext_ptt_delay_ms, so an external amplifier's relay has closed before RF
// arrives (hw_settings.h); 0 skips the wait.
static int sbitx_relays_tx(int on, int freq_hz) {
  (void)freq_hz;
  if (on) {
    drive(board->ext_ptt_pin, 1);
    if (tx_ext_ptt_delay_ms > 0)
      usleep((useconds_t)tx_ext_ptt_delay_ms * 1000);
    drive(board->tx_line_pin, 1);
  } else {
    drive(board->ext_ptt_pin, 0);
    usleep(RX_RELAY_SETTLE_MS * 1000);
    drive(board->tx_line_pin, 0);
  }
  return 0;
}

// zBitx: TX_LINE powers the PA, RX_LINE connects the receiver, and the LPF
// relays carry only the transmitter. The transmit half is not written
// until the zBitx transmit step (study §3.3, §7), so it refuses whatever
// the profile says; the receive half leaves the radio receiving.
static int zbitx_relays_tx(int on, int freq_hz) {
  (void)freq_hz;
  if (on)
    return -1;
  drive(board->tx_line_pin, 0);
  lpfs_off();
  prev_lpf = 0;
  usleep(RX_RELAY_SETTLE_MS * 1000);
  drive(board->rx_line_pin, 1);
  return 0;
}

/* ---- INA260 power monitor ------------------------------------------------ */

void read_voltage_current(float *voltage, float *current) {
  uint8_t data_buffer[2]; // Buffer to hold raw register data

  // Explicitly set the register pointer to the voltage register
  if (i2c_write_i2c_block_data(INA260_ADDRESS, VOLTAGE_REGISTER, 0, NULL) < 0) {
    printf("Error setting voltage register pointer\n");
    *voltage = 0.0f;
    *current = 0.0f;
    return;
  }

  // Read the voltage register (2 bytes)
  int e = i2c_read_i2c_block_data(INA260_ADDRESS, VOLTAGE_REGISTER, 2, data_buffer);
  if (e != 2) {
    printf("Error reading voltage register\n");
    *voltage = 0.0f;
    *current = 0.0f;
    return;
  }
  uint16_t raw_voltage = (data_buffer[0] << 8) | data_buffer[1];
  *voltage = raw_voltage * 1.25e-3f; // Convert to volts (1.25 mV per LSB)

  // Explicitly set the register pointer to the current register
  if (i2c_write_i2c_block_data(INA260_ADDRESS, CURRENT_REGISTER, 0, NULL) < 0) {
    printf("Error setting current register pointer\n");
    *voltage = 0.0f;
    *current = 0.0f;
    return;
  }

  // Read the current register (2 bytes)
  e = i2c_read_i2c_block_data(INA260_ADDRESS, CURRENT_REGISTER, 2, data_buffer);
  if (e != 2) {
    printf("Error reading current register\n");
    *voltage = 0.0f;
    *current = 0.0f;
    return;
  }
  uint16_t raw_current = (data_buffer[0] << 8) | data_buffer[1];

  // Handle saturation or invalid value
  if (raw_current == 0xFFFF) {
    printf("Current measurement out of range or invalid\n");
    *current = 0.0f;
  } else {
    *current = raw_current * 1.25e-3f; // Convert to amps (1.25 mA per LSB)
  }
}

int radio_hw_ina260_configure(void) {
  uint8_t config_data[2] = {
      (uint8_t)(CONFIG_DEFAULT >> 8),  // MSB
      (uint8_t)(CONFIG_DEFAULT & 0xFF) // LSB
  };
  if (i2c_write_i2c_block_data(INA260_ADDRESS, CONFIG_REGISTER, 2, config_data) < 0)
    return -1;
  return 0;
}
