/*
 * harness.c - runs maxibitx's CW decoder (src/cw_decoder.c) on recorded
 * audio, for tools/cw_study/cw_bench.py.
 *
 * Reads mono 96 kHz audio as raw little-endian int32 on stdin, any level,
 * in 1024-sample blocks, as rx_audio.c hands it over, and writes the
 * decoded text to stdout.
 *
 *   harness [pitch]      (default 700 Hz)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cw_decoder.h"

int main(int argc, char **argv) {
  int pitch = argc > 1 ? atoi(argv[1]) : 700;
  int32_t raw[1024];
  float block[1024];
  char text[CW_DECODER_TEXT_KEEP + 1];
  uint32_t seq = 0;
  cw_decoder_init();
  while (fread(raw, sizeof(raw[0]), 1024, stdin) == 1024) {
    for (int i = 0; i < 1024; i++)
      block[i] = (float)raw[i] * (1.0f / 2147483648.0f);
    cw_decoder_process(block, 1024, pitch);
    seq = cw_decoder_read(seq, text, sizeof(text));
    fputs(text, stdout);
  }
  // a second of silence, so the last character and word are written
  for (int i = 0; i < 1024; i++)
    block[i] = 0.0f;
  for (int b = 0; b < 300; b++) {
    cw_decoder_process(block, 1024, pitch);
    seq = cw_decoder_read(seq, text, sizeof(text));
    fputs(text, stdout);
  }
  putchar('\n');
  return 0;
}
