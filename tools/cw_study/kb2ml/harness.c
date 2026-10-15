/*
 * harness.c - runs the receive half of sbitx's modem_cw.c (the KB2ML
 * decoder) off the radio, for tools/cw_study/cw_bench.py.
 *
 * Reads mono 96 kHz audio as raw little-endian int32 on stdin, scaled as
 * sbitx's rx_linear() hands it to modem_rx() (AGC peak about 1e8), in
 * 1024-sample blocks, and writes the decoded text to stdout.
 *
 *   harness [wpm] [pitch]
 *
 * wpm is what the decoder reads from the WPM field, the operator's own
 * keyer speed (default 20); pitch is the PITCH field (default 700).
 *
 * Everything modem_cw.c needs from the rest of sbitx is stubbed here:
 * the key is up, nothing is queued to send, and time advances with the
 * audio, 1024 samples at a time.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <complex.h>
#include <fftw3.h>

#include "sdr.h"
#include "sdr_ui.h"

void cw_init(void);
void cw_poll(int bytes_available, int tx_is_on);
void cw_rx(int32_t *samples, int count);

static unsigned long samples_done;
static int field_wpm = 20, field_pitch = 700;

int cw_decode_enabled = 1;
int text_ready = 0;

unsigned int millis(void) { return (unsigned int)(samples_done / 96); }
int field_int(char *label) {
  if (!strcmp(label, "WPM")) return field_wpm;
  if (!strcmp(label, "PITCH")) return field_pitch;
  return 0;
}
int get_pitch(void) { return field_pitch; }
int get_cw_delay(void) { return 300; }
int get_cw_input_method(void) { return 0; }
int key_poll(void) { return 0; }
int get_tx_data_byte(char *c) { (void)c; return 0; }
void tx_on(int trigger) { (void)trigger; }
void tx_off(void) {}
void vfo_start(struct vfo *v, int frequency_hz, int start_phase) {
  (void)v; (void)frequency_hz; (void)start_phase;
}
int vfo_read(struct vfo *v) { (void)v; return 0; }
void write_console(sbitx_style style, const char *text) {
  (void)style;
  fputs(text, stdout);
}

int main(int argc, char **argv) {
  if (argc > 1) field_wpm = atoi(argv[1]);
  if (argc > 2) field_pitch = atoi(argv[2]);
  int32_t block[1024];
  cw_init();
  while (fread(block, sizeof(block[0]), 1024, stdin) == 1024) {
    cw_rx(block, 1024);
    samples_done += 1024;
    cw_poll(0, 0);
  }
  putchar('\n');
  return 0;
}
