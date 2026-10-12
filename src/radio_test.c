// radio_test.c
//
// Bench harness for radio.c's RIT and XIT: what each puts on the si5351's
// clk2 in receive and in transmit, with the clock generator, the board, the
// codec and the band table stubbed. The TX worker thread runs for real; the
// harness waits for it to apply each change.
//
//   make test-radio && ./test-radio
//
// RIT moves only the receive clock, XIT only the transmit one; both
// together; a change while transmitting waits for the next transmission;
// a retune clears both; the [tx_band] check and the board's LPF use the
// transmit frequency, so XIT can carry a transmission out of a band.

#include "radio.h"
#include "radio_hw.h"
#include "rx_audio.h"
#include "si5351.h"
#include "sound.h"
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static int failures = 0;
static void check(int ok, const char *what) {
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    failures++;
}

/* ---- Stubs ------------------------------------------------------------------ */

static volatile uint32_t clk[3];
static volatile int relays_freq = 0, applied = 0;
void si5351bx_setfreq(uint8_t n, uint32_t f) {
  if (n < 3)
    clk[n] = f;
}
void radio_hw_tune(int f) { (void)f; }
int radio_hw_relays_tx(int on, int f) {
  if (on)
    relays_freq = f;
  return 0;
}
int radio_hw_tx_permitted(void) { return 1; }
const char *radio_hw_board_name(void) { return "test board"; }
void sound_set_rx_capture(int on) { (void)on; }
void sound_set_tx_drive(int v) {
  (void)v;
  applied++;
}
int sound_tr_timing_enabled(void) { return 0; }
int sound_tx_first_block(int64_t *a, int64_t *b, int64_t *c) {
  (void)a; (void)b; (void)c;
  return 0;
}
int sound_tx_if_placed(void) { return 1; }
int sound_update_cw_if_placement(void) { return 0; }
void cw_set_pitch(int hz) { (void)hz; }
int rx_audio_set_narrow_pitch(int hz) { return hz; }
int rx_audio_get_narrow_pitch(void) { return 700; }
void rx_audio_set_demod(enum rx_demod d) { (void)d; }
void rx_audio_inhibit_narrow_filter(int on) { (void)on; }
// One band, 14.000-14.350 MHz.
int hw_settings_tx_allowed(int f) { return f >= 14000000 && f <= 14350000; }

/* ---- Helpers ---------------------------------------------------------------- */

#define C 40012400 // xtal_filter_center's default

// Waits for the TX worker to apply a change (sound_set_tx_drive() is the
// last thing it does either way).
static void settle(int before) {
  for (int i = 0; i < 200 && applied == before; i++)
    usleep(1000);
}
static int tx(int on) {
  int before = applied;
  int r = radio_set_tx(on);
  if (r == 0)
    settle(before);
  return r;
}

int main(void) {
  printf("RIT and XIT in receive\n");
  radio_tune_to(14060000);
  check(clk[2] == 14060000 + C, "tuned: clk2 at dial + filter centre");
  radio_set_rit(200);
  check(clk[2] == 14060200 + C, "RIT +200 moves the receive clock");
  radio_set_xit(-500);
  check(clk[2] == 14060200 + C, "XIT -500 leaves the receive clock alone");
  check(radio_xit_enabled() && radio_get_xit() == -500, "XIT on at -500");
  check(radio_tx_freq() == 14059500, "the transmit frequency is the dial - 500");

  printf("Transmitting\n");
  check(tx(1) == 0, "TX accepted");
  check(clk[2] == 14059500 + C, "TX clk2 at dial + XIT, without RIT");
  check(relays_freq == 14059500, "the board's relays get the transmit frequency");
  radio_set_xit(300);
  check(clk[2] == 14059500 + C && radio_tx_freq() == 14059500,
        "XIT changed mid-transmission: this one stays put");
  check(tx(0) == 0 && clk[2] == 14060200 + C, "back to RX: RIT's receive clock again");
  check(tx(1) == 0 && clk[2] == 14060300 + C, "the next transmission takes XIT +300");
  tx(0);
  radio_set_xit_enabled(0);
  check(radio_get_xit() == 300 && radio_tx_freq() == 14060000, "XIT off keeps its offset");
  check(tx(1) == 0 && clk[2] == 14060000 + C, "... and transmits on the dial");
  tx(0);
  radio_set_xit_enabled(1);
  check(radio_tx_freq() == 14060300, "XIT back on: +300 again");

  printf("Band edges\n");
  radio_tune_to(14349000);
  check(!radio_xit_enabled() && radio_get_xit() == 0 && !radio_rit_enabled(),
        "a retune clears RIT and XIT");
  radio_set_xit(2000);
  int refused_hz;
  radio_tx_refused(&refused_hz); // drain
  check(tx(1) < 0 && !in_tx, "XIT +2 kHz past the band's top: TX refused");
  check(radio_tx_refused(&refused_hz) == RADIO_TX_REFUSED_BAND && refused_hz == 14351000,
        "... reported at the transmit frequency, 14.351 MHz");
  radio_set_xit(-2000);
  check(tx(1) == 0 && clk[2] == 14347000 + C, "XIT -2 kHz, inside it: TX on 14.347 MHz");
  tx(0);
  radio_tune_to(13999500);
  check(tx(1) < 0, "a dial just below the band ...");
  radio_set_xit(600);
  check(tx(1) == 0 && clk[2] == 14000100 + C, "... with XIT taking it inside: TX accepted");
  tx(0);

  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
