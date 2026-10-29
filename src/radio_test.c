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
// transmit frequency, so XIT can carry a transmission out of a band. And
// when the CW decoder is told to start afresh: a move of more than 300 Hz,
// or entering CW or CWR from another mode. And the si5351's drive
// strength and clk1's divider: the receive clocks rewritten, refused while
// transmitting.

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
static volatile int relays_freq = 0, applied = 0, clk_writes = 0;
void si5351bx_setfreq(uint8_t n, uint32_t f) {
  if (n < 3)
    clk[n] = f;
  clk_writes++;
}
static int drive_ma = 8;
int si5351_set_drive_ma(int ma) {
  if (ma != 2 && ma != 4 && ma != 6 && ma != 8)
    return -1;
  drive_ma = ma;
  return 0;
}
int si5351_get_drive_ma(void) { return drive_ma; }
static int clk1_div = 0, pll_resets_done = 0;
int si5351_set_clk1_divider(int div) {
  if (div != 0 && div != 16 && div != 18 && div != 20 && div != 22)
    return -1;
  clk1_div = div;
  return 0;
}
int si5351_get_clk1_divider(void) { return clk1_div; }
int si5351_clk1_divider_for(uint32_t f) { (void)f; return clk1_div ? clk1_div : 18; }
void si5351_reset(void) { pll_resets_done++; }
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
static int decoder_resets = 0;
void cw_decoder_reset(void) { decoder_resets++; }
// SPOT's receive half: what the search "found" is set by each check.
static enum rx_spot_result spot_result = RX_SPOT_OK;
static double spot_offset = 0;
static int spot_resets = 0;
enum rx_spot_result rx_audio_spot(double *offset_hz) {
  *offset_hz = spot_offset;
  return spot_result;
}
void rx_audio_spot_reset(void) { spot_resets++; }
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

  printf("The CW decoder\n");
  radio_set_mode(RADIO_MODE_CW);
  radio_tune_to(7030000);
  int r0 = decoder_resets;
  radio_tune_to(7030100);
  radio_set_rit(-150);
  check(decoder_resets == r0, "a 100 Hz retune and 150 Hz of RIT: the decoder keeps learning");
  radio_tune_to(7031000);
  check(decoder_resets == r0 + 1, "a 1 kHz retune: the decoder starts afresh");
  radio_set_mode(RADIO_MODE_CWR);
  check(decoder_resets == r0 + 1, "CW to CWR: no reset");
  radio_set_mode(RADIO_MODE_USB);
  radio_set_mode(RADIO_MODE_CW);
  check(decoder_resets == r0 + 2, "USB back to CW: the decoder starts afresh");

  printf("SPOT\n");
  radio_set_mode(RADIO_MODE_CW);
  radio_tune_to(14050000);
  radio_set_rit(100);
  radio_set_xit(200);
  spot_result = RX_SPOT_OK;
  spot_offset = 37.4;              // the station, above where RIT listens
  int moved = 0, s0 = spot_resets;
  check(radio_spot(&moved) == RADIO_SPOT_OK, "a tone found: SPOT moves");
  check(freq_hdr == 14050137 && moved == 137,
        "the dial moves to where RIT was listening, plus the tone's offset: +137 Hz");
  check(!radio_rit_enabled() && radio_get_rit() == 0, "RIT is cleared");
  check(radio_xit_enabled() && radio_get_xit() == 200 && radio_tx_freq() == 14050337,
        "XIT is kept");
  check(clk[2] == 14050137 + C, "the receive clock follows the dial");
  check(spot_resets > s0, "the audio it searched is forgotten, so a second press waits");
  spot_offset = -12.6;
  check(radio_spot(&moved) == RADIO_SPOT_OK && freq_hdr == 14050124 && moved == -13,
        "a tone below: the dial moves down, rounded to 1 Hz");
  spot_result = RX_SPOT_NO_SIGNAL;
  radio_set_rit(50);
  check(radio_spot(&moved) == RADIO_SPOT_NO_SIGNAL && freq_hdr == 14050124 &&
        radio_get_rit() == 50, "nothing found: nothing moves, RIT stays");
  spot_result = RX_SPOT_NOT_YET;
  check(radio_spot(&moved) == RADIO_SPOT_NOT_YET && freq_hdr == 14050124,
        "too soon after a retune: nothing moves");
  spot_result = RX_SPOT_OK;
  spot_offset = 20;
  check(tx(1) == 0, "transmitting ...");
  check(radio_spot(&moved) == RADIO_SPOT_TX && freq_hdr == 14050124, "... SPOT refused");
  tx(0);
  radio_set_mode(RADIO_MODE_USB);
  check(radio_spot(&moved) == RADIO_SPOT_NOT_CW && freq_hdr == 14050124, "USB: SPOT refused");
  radio_set_mode(RADIO_MODE_CWR);
  check(radio_spot(&moved) == RADIO_SPOT_OK && freq_hdr == 14050194,
        "CWR: moves by the offset rx_audio_spot() gives (its sign is rx_audio.c's job)");

  printf("The si5351's drive strength\n");
  radio_set_mode(RADIO_MODE_CW);
  radio_tune_to(14060000);
  int writes = clk_writes;
  check(radio_set_si5351_drive(4) == 0 && drive_ma == 4 && clk_writes == writes + 2 &&
            clk[1] == C + RX_IF_FREQ_HZ && clk[2] == 14060000 + C,
        "4 mA: set, and both receive clocks rewritten where they were");
  writes = clk_writes;
  check(radio_set_si5351_drive(4) == 0 && clk_writes == writes, "4 mA again: nothing rewritten");
  check(radio_set_si5351_drive(5) < 0 && drive_ma == 4 && clk_writes == writes,
        "5 mA refused, nothing changed");
  check(tx(1) == 0, "transmitting ...");
  writes = clk_writes;
  check(radio_set_si5351_drive(8) < 0 && drive_ma == 4 && clk_writes == writes,
        "... a drive change refused");
  tx(0);
  check(radio_set_si5351_drive(8) == 0 && drive_ma == 8, "back in receive: 8 mA again");

  printf("clk1's divider\n");
  check(radio_get_clk1_divider() == 18, "automatic: 18");
  writes = clk_writes;
  int pll_resets = pll_resets_done;
  check(radio_set_clk1_divider(22) == 0 && clk1_div == 22 && radio_get_clk1_divider() == 22 &&
            clk_writes == writes + 1 && clk[1] == C + RX_IF_FREQ_HZ && pll_resets_done == pll_resets + 1,
        "22: set, clk1 rewritten at the same frequency, the PLLs reset");
  writes = clk_writes;
  check(radio_set_clk1_divider(22) == 0 && clk_writes == writes && pll_resets_done == pll_resets + 1,
        "22 again: nothing rewritten");
  check(radio_set_clk1_divider(19) < 0 && clk1_div == 22 && clk_writes == writes,
        "19 refused, nothing changed");
  check(tx(1) == 0, "transmitting ...");
  writes = clk_writes;
  check(radio_set_clk1_divider(0) < 0 && clk1_div == 22 && clk_writes == writes,
        "... a divider change refused");
  tx(0);
  check(radio_set_clk1_divider(0) == 0 && radio_get_clk1_divider() == 18,
        "back in receive: automatic again");

  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
