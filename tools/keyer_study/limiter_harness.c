// limiter_harness.c - feeds a CW dit train, shaped exactly the way src/cw.c
// shapes it, through the real src/tx_pipeline.c at a chosen limiter
// ceiling, and writes the pipeline's output as raw float32 for
// envelope_study.py --limiter to analyse. Built and run by that script,
// which also generates the cw_table.h this includes from src/cw.c's table.
//
// Usage: limiter_harness WPM CEILING OUT.f32
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cw_table.h"
#include "tx_pipeline.h"

#define FS 96000.0
#define N_DITS 40
#define LEAD_DITS 4

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s WPM CEILING OUT.f32\n", argv[0]);
    return 2;
  }
  double wpm = atof(argv[1]);
  float ceiling = (float)atof(argv[2]);
  int dit = (int)lround(FS * 1.2 / wpm);
  int blocks = dit * (2 * N_DITS + 2 * LEAD_DITS) / TX_PIPELINE_BLOCK_LEN + 4;

  struct tx_pipeline *p = tx_pipeline_new();
  tx_pipeline_set_ceiling(p, ceiling);
  FILE *f = fopen(argv[3], "wb");
  if (!f) {
    perror(argv[3]);
    return 1;
  }

  // Same envelope walk as cw_get_sample(): one table step per sample toward
  // the key state, read after the step.
  int pos = 0;
  double ph = 0, dph = 2 * M_PI * 700.0 / FS;
  float in[TX_PIPELINE_BLOCK_LEN], out[TX_PIPELINE_BLOCK_LEN];
  for (int b = 0; b < blocks; b++) {
    for (int i = 0; i < TX_PIPELINE_BLOCK_LEN; i++) {
      long n = (long)b * TX_PIPELINE_BLOCK_LEN + i;
      int key = n >= (long)dit * LEAD_DITS && n < (long)dit * (LEAD_DITS + 2 * N_DITS) &&
                ((n - (long)dit * LEAD_DITS) / dit) % 2 == 0;
      if (key) {
        if (pos < ENV_LEN - 1) pos++;
      } else if (pos > 0) {
        pos--;
      }
      in[i] = (float)(sin(ph) * env_table[pos]);
      ph += dph;
    }
    tx_pipeline_process_block(p, TX_PIPELINE_CW, in, out);
    fwrite(out, sizeof(float), TX_PIPELINE_BLOCK_LEN, f);
  }
  fclose(f);
  tx_pipeline_free(p);
  return 0;
}
