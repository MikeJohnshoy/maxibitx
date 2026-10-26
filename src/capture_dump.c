// capture_dump.c - see capture_dump.h.
//
// SIGUSR1 is blocked in every thread (capture_dump_block_signal(), before
// any starts), so it never cuts into a system call; one thread at ordinary
// priority takes it with sigwait(), asks the audio thread for a second,
// waits for it and writes the file. The audio thread only copies into a
// buffer allocated at start-up and posts a semaphore when the second is
// full, so it never waits on the disk. The file is 32-bit PCM,
// little-endian, the channels interleaved as captured (L = the RX IF from
// the crystal filter, R = the mic).

#include "capture_dump.h"
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { IDLE, ASKED, RECORDING, WRITING }; // the writer asks, the audio thread records

static atomic_int state = IDLE;
static int32_t *buf;
static int buf_frames, filled, n_channels, rate;
static char out_dir[256];
static sem_t full;
static void (*describer)(char *out, int max);

void capture_dump_set_describer(void (*describe)(char *out, int max)) { describer = describe; }

void capture_dump_block(const int32_t *interleaved, int frames) {
  int s = atomic_load_explicit(&state, memory_order_acquire);
  if (s == ASKED) {
    filled = 0;
    atomic_store_explicit(&state, RECORDING, memory_order_relaxed);
  } else if (s != RECORDING) {
    return;
  }
  int n = frames < buf_frames - filled ? frames : buf_frames - filled;
  memcpy(buf + (size_t)filled * n_channels, interleaved, (size_t)n * n_channels * sizeof(int32_t));
  filled += n;
  if (filled == buf_frames) {
    atomic_store_explicit(&state, WRITING, memory_order_release);
    sem_post(&full);
  }
}

static void put_u32(FILE *f, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  fwrite(b, 1, 4, f);
}

static void put_u16(FILE *f, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  fwrite(b, 1, 2, f);
}

static int write_wav(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return -1;
  uint32_t data_bytes = (uint32_t)buf_frames * (uint32_t)n_channels * 4;
  fwrite("RIFF", 1, 4, f);
  put_u32(f, 36 + data_bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put_u32(f, 16);
  put_u16(f, 1); // PCM
  put_u16(f, (uint16_t)n_channels);
  put_u32(f, (uint32_t)rate);
  put_u32(f, (uint32_t)rate * (uint32_t)n_channels * 4);
  put_u16(f, (uint16_t)(n_channels * 4));
  put_u16(f, 32);
  fwrite("data", 1, 4, f);
  put_u32(f, data_bytes);
  for (size_t k = 0; k < (size_t)buf_frames * n_channels; k++)
    put_u32(f, (uint32_t)buf[k]);
  int bad = ferror(f);
  return fclose(f) != 0 || bad ? -1 : 0;
}

void capture_dump_block_signal(void) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &set, NULL);
}

static void *writer(void *arg) {
  (void)arg;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  for (;;) {
    int sig;
    if (sigwait(&set, &sig) != 0)
      continue;
    printf("capture_dump: SIGUSR1 - recording %d s of the raw capture\n", CAPTURE_DUMP_SECONDS);
    fflush(stdout);
    atomic_store(&state, ASKED);
    while (sem_wait(&full) != 0)
      ;
    char stamp[32], path[320], about[160] = "";
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", localtime(&now));
    snprintf(path, sizeof(path), "%s/maxibitx_capture_%s.wav", out_dir, stamp);
    if (describer)
      describer(about, sizeof(about));
    if (write_wav(path) == 0)
      printf("capture_dump: wrote %s - %d s, %d Hz, %d channels, S32%s%s\n", path,
             CAPTURE_DUMP_SECONDS, rate, n_channels, about[0] ? ", " : "", about);
    else
      printf("capture_dump: couldn't write %s\n", path);
    fflush(stdout);
    atomic_store(&state, IDLE);
  }
  return NULL;
}

int capture_dump_init(const char *dir, int sample_rate, int channels) {
  snprintf(out_dir, sizeof(out_dir), "%s", dir);
  rate = sample_rate;
  n_channels = channels;
  buf_frames = sample_rate * CAPTURE_DUMP_SECONDS;
  buf = calloc((size_t)buf_frames * channels, sizeof(int32_t));
  if (!buf || sem_init(&full, 0, 0) != 0)
    return -1;
  pthread_t t;
  if (pthread_create(&t, NULL, writer, NULL) != 0)
    return -1;
  pthread_detach(t);
  return 0;
}
