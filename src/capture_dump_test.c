// capture_dump_test.c
//
// Bench harness for capture_dump.c (make test-capture-dump): SIGUSR1 sent
// to the process while this harness plays the audio thread, feeding
// blocks of a counting pattern (L = frame number, R = its negative); the
// WAV file is read back - its header, and one second of frames, both
// channels, contiguous and in order - and nothing is recorded unasked.

#define _GNU_SOURCE
#include "capture_dump.h"
#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RATE 8000 // small, so a second is quick to check
#define BLOCK 100

static int failures;
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    printf((cond) ? "  ok   " : "  FAIL ");                                                        \
    if (!(cond))                                                                                   \
      failures++;                                                                                  \
    printf(__VA_ARGS__);                                                                           \
    printf("\n");                                                                                  \
  } while (0)

static char dir[] = "/tmp/capture_dump_testXXXXXX";
static int32_t frame_no;

static void feed(int blocks) {
  int32_t b[BLOCK * 2];
  for (int k = 0; k < blocks; k++) {
    for (int j = 0; j < BLOCK; j++, frame_no++) {
      b[2 * j] = frame_no;
      b[2 * j + 1] = -frame_no;
    }
    capture_dump_block(b, BLOCK);
    usleep(500);
  }
}

// How many maxibitx_capture_*.wav files dir holds; path gets one of them,
// or "".
static int find_file(char *path, size_t max) {
  DIR *d = opendir(dir);
  struct dirent *e;
  int n = 0;
  path[0] = '\0';
  while (d && (e = readdir(d))) {
    if (!strncmp(e->d_name, "maxibitx_capture_", 17)) {
      snprintf(path, max, "%s/%s", dir, e->d_name);
      n++;
    }
  }
  if (d)
    closedir(d);
  return n;
}

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void describe(char *out, int max) { snprintf(out, (size_t)max, "on the bench"); }

int main(void) {
  capture_dump_block_signal();
  if (!mkdtemp(dir)) {
    perror("mkdtemp");
    return 1;
  }
  CHECK(capture_dump_init(dir, RATE, 2) == 0, "initialised");
  capture_dump_set_describer(describe);

  char path[512];
  feed(200);
  CHECK(find_file(path, sizeof(path)) == 0, "nothing recorded unasked");

  kill(getpid(), SIGUSR1);
  usleep(100000);
  int32_t first_after_signal = frame_no;
  for (int k = 0; k < 400 && !find_file(path, sizeof(path)); k++)
    feed(1);
  usleep(200000);
  CHECK(find_file(path, sizeof(path)) == 1, "SIGUSR1: one file (%s)", path);

  FILE *f = fopen(path, "rb");
  static uint8_t wav[44 + RATE * 8];
  size_t got = f ? fread(wav, 1, sizeof(wav), f) : 0;
  int more = f && fgetc(f) != EOF; // nothing past the second
  if (f)
    fclose(f);
  CHECK(got == sizeof(wav) && !more,"its size: a 44-byte header and a second of stereo S32 (%zu)", got);
  CHECK(!memcmp(wav, "RIFF", 4) && !memcmp(wav + 8, "WAVEfmt ", 8) && !memcmp(wav + 36, "data", 4),
        "RIFF, WAVE, fmt and data");
  CHECK(u32(wav + 20) == (1 | 2u << 16) && u32(wav + 24) == RATE && u32(wav + 28) == RATE * 8 &&
            u32(wav + 32) == (8 | 32u << 16) && u32(wav + 40) == RATE * 8,
        "PCM, 2 channels, %d Hz, 32 bits, %d data bytes", RATE, RATE * 8);
  int32_t start = (int32_t)u32(wav + 44);
  int contiguous = 1;
  for (int k = 0; k < RATE; k++) {
    int32_t l = (int32_t)u32(wav + 44 + 8 * k), r = (int32_t)u32(wav + 48 + 8 * k);
    if (l != start + k || r != -(start + k))
      contiguous = 0;
  }
  CHECK(contiguous, "a second of frames, L and R as fed, contiguous from frame %d", start);
  CHECK(start >= first_after_signal - BLOCK, "starting after the signal");

  sleep(1); // a new file name (it has the time in it)
  kill(getpid(), SIGUSR1);
  usleep(100000);
  for (int k = 0; k < 400 && find_file(path, sizeof(path)) < 2; k++)
    feed(1);
  usleep(200000);
  CHECK(find_file(path, sizeof(path)) == 2, "and again: a second file");

  while (find_file(path, sizeof(path)))
    remove(path);
  rmdir(dir);
  printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
  return failures ? 1 : 0;
}
