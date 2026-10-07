// radio_hw.h
//
// The radio board: which one this is, its GPIO lines, LPF band switching,
// the relay half of the T/R sequence, and the INA260 power monitor.
//
// Every difference between the boards maxibitx runs on lives in
// radio_hw.c, as one profile per board. Nothing outside it asks which
// board it is on; it calls the operations below, which mean the same on
// every board. docs/dsp_design_notes/zbitx_port_study.md §5 and §9.

#ifndef RADIO_HW_H
#define RADIO_HW_H

// The key jack, the same two pins on every board (BCM numbering): read by
// key_input.c, not by this file. Inputs, pulled up, closed to ground =
// down.
#define KEY_RING_GPIO 4 // ring: dash, and the mic PTT line (sbitx's PTT)
#define KEY_TIP_GPIO  5 // tip: dot, or a straight key's contact (sbitx's
                        // DASH line - its name, not its role here)

// The front-panel knobs: rotary encoders with a push switch, read by
// encoder.c. What each does is knobs.c's.
enum radio_knob { RADIO_KNOB_TUNING = 0, RADIO_KNOB_VOLUME = 1, RADIO_KNOBS = 2 };

// One knob's lines (BCM numbering), each pulled up and closed to ground.
// Turning it so A changes before B counts up. a_pin -1: the board has no
// such knob that the Pi reads.
struct radio_hw_knob {
  int a_pin, b_pin, sw_pin;
  int edges_per_detent; // quadrature edges from one click of the knob to the next: 4 or 2
};

// Selects the board named by hw_settings.ini's sbitx_version line
// (hw_settings.h): "SBITX_V3" for an sBitx DE, v2 or v3, "SBITX_V4" for a
// zBitx. Returns 0, or -1 - after saying what the valid names are - for a
// missing (empty) or unknown name. Touches no hardware. Call once, before
// anything else here.
int radio_hw_select_board(const char *sbitx_version);

// The selected board, for the log: e.g. "zBitx".
const char *radio_hw_board_name(void);

// The I2C bus the si5351 is on: hw_settings.ini's i2c_bus if set,
// otherwise the board's usual bus.
int radio_hw_i2c_bus(void);

// Claims the board's output lines through gpio.c and puts each in its
// receive state as part of the request: T/R relay, PA and PTT lines low,
// every LPF relay off, and the receiver connected where the board has a
// line for it. Logs what it claimed. Returns 0, or -1 if a request
// failed (gpio.c says which and why).
int radio_hw_gpio_init(void);

// Called on every retune with the new dial frequency. On a board whose LPF
// relays are in the receive path (the sBitx), selects the band's filter;
// on one where they are switched only for transmit (the zBitx), does
// nothing.
void radio_hw_tune(int freq_hz);

// The selected board's knob k; a_pin is -1 if it has none.
struct radio_hw_knob radio_hw_knob(enum radio_knob k);

// 1 if this board may transmit at all, 0 if every transmit request is to
// be refused (the zBitx, until its transmit path is calibrated).
int radio_hw_tx_permitted(void);

// The relay half of the T/R sequence, for radio.c's TX worker, which does
// the clocks and the codec around it. on = 1: everything between the
// clocks being set for TX and the exciter feed being raised, including any
// settling wait. on = 0: everything between the exciter feed being dropped
// and the clocks going back to RX, ending with the receiver connected.
// freq_hz is the dial, for a board that picks its LPF at transmit. Returns
// 0, or -1 if this board cannot transmit, in which case nothing is
// switched; on = 0 always succeeds.
int radio_hw_relays_tx(int on, int freq_hz);

// Milliseconds the relay half of the TX-up sequence deliberately waits
// before RF can appear (EXT_PTT's delay on the sBitx, the LPF relay's
// settling time on the zBitx), for sound.c's start-up T/R timing check.
int radio_hw_tx_settle_ms(void);

// Reads the INA260 power monitor's voltage (V) and current (A) registers
// over I2C. On any I2C error, both outputs are set to 0.0.
void read_voltage_current(float *voltage, float *current);

// Writes the INA260's configuration register (continuous mode, default
// averaging). Returns 0 on success, -1 on I2C failure.
int radio_hw_ina260_configure(void);

#endif /* RADIO_HW_H */
