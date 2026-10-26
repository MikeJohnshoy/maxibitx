// codec_check.c - see codec_check.h.
//
// Two threads. The audio thread (codec_check_block()) only adds into a
// 128-entry table while a measurement is asked for, and posts a semaphore
// when it has CODEC_CHECK_FRAMES of them; it never waits. The check's own
// thread, at ordinary priority, waits for the audio to settle, asks for a
// measurement, restarts the codec's interface while it's bad, and says on
// the console what it found and did.

#include "codec_check.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define WM8731_ADDR 0x1a
#define WM8731_ACTIVE 9
#define RESTART_SETTLE_MS 300 // after an interface restart, before measuring
#define MEASURE_WAIT_S 60     // for one measurement (receive, no xruns)

enum { IDLE, ASKED, MEASURING, DONE }; // the check asks, the audio thread measures

static atomic_int state = IDLE;
static atomic_int outcome = CODEC_CHECK_RUNNING;
static double power[CODEC_CHECK_PERIOD];
static int counted, phase, have_prev;
static int32_t prev;
static double score;
static sem_t done;
static int (*restart_fn)(void);

double codec_check_score(const double p[CODEC_CHECK_PERIOD]) {
  double mean = 0, re = 0, im = 0;
  for (int k = 0; k < CODEC_CHECK_PERIOD; k++) {
    double a = 2 * M_PI * CODEC_CHECK_HARMONIC * k / CODEC_CHECK_PERIOD;
    mean += p[k];
    re += p[k] * cos(a);
    im += p[k] * sin(a);
  }
  mean /= CODEC_CHECK_PERIOD;
  return mean > 0 ? 2 * hypot(re, im) / CODEC_CHECK_PERIOD / mean : 0;
}

static void start_over(void) {
  memset(power, 0, sizeof(power));
  counted = phase = have_prev = 0;
}

void codec_check_gap(void) {
  if (atomic_load_explicit(&state, memory_order_acquire) == MEASURING)
    start_over();
}

void codec_check_block(const int32_t *interleaved, int frames, int channels, int usable) {
  int s = atomic_load_explicit(&state, memory_order_acquire);
  if (s == ASKED) {
    start_over();
    atomic_store_explicit(&state, MEASURING, memory_order_relaxed);
  } else if (s != MEASURING) {
    return;
  }
  if (!usable) {
    start_over();
    return;
  }
  for (int i = 0; i < frames; i++) {
    int32_t r = interleaved[i * channels + 1];
    if (have_prev) {
      double d = (double)r - (double)prev;
      power[phase] += d * d;
      if (++counted == CODEC_CHECK_FRAMES) {
        score = codec_check_score(power);
        atomic_store_explicit(&state, DONE, memory_order_release);
        sem_post(&done);
        return;
      }
    }
    prev = r;
    have_prev = 1;
    phase = (phase + 1) % CODEC_CHECK_PERIOD;
  }
}

static void sleep_ms(int ms) {
  struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
  while (nanosleep(&t, &t) != 0 && errno == EINTR)
    ;
}

// One measurement's score, or -1 if none came (transmitting, or no audio).
static double measure(void) {
  atomic_store_explicit(&state, ASKED, memory_order_release);
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += MEASURE_WAIT_S;
  while (sem_timedwait(&done, &until) != 0) {
    if (errno != EINTR) {
      atomic_store(&state, IDLE);
      return -1;
    }
  }
  double s = score;
  atomic_store(&state, IDLE);
  return s;
}

static void *check(void *arg) {
  (void)arg;
  sleep_ms(CODEC_CHECK_SETTLE_MS);
  double first = measure(), s = first;
  for (int restarts = 0;; restarts++) {
    if (s < 0) {
      printf("codec: couldn't check the sampling state (no receive audio for %d s)\n",
             MEASURE_WAIT_S);
      atomic_store(&outcome, CODEC_CHECK_UNMEASURED);
      break;
    }
    if (s < CODEC_CHECK_BAD) {
      if (restarts == 0)
        printf("codec: sampling clean (%.3f)\n", s);
      else
        printf("codec: sampling clean after %d interface restart%s (%.3f)\n", restarts,
               restarts == 1 ? "" : "s", s);
      atomic_store(&outcome, CODEC_CHECK_CLEAN);
      break;
    }
    if (restarts == 0)
      printf("codec: started with a 128-sample timing pattern (%.3f) - every signal "
             "would have ghosts every 750 Hz%s\n",
             s, restart_fn ? "; restarting its interface" : " - restart maxibitx");
    if (!restart_fn) {
      atomic_store(&outcome, CODEC_CHECK_BAD_STATE);
      break;
    }
    if (restarts == CODEC_CHECK_TRIES) {
      printf("codec: still patterned after %d restarts (%.3f) - restart maxibitx\n", restarts, s);
      atomic_store(&outcome, CODEC_CHECK_BAD_STATE);
      break;
    }
    if (restart_fn() < 0) {
      printf("codec: couldn't restart its interface (no WM8731 found, or the I2C write "
             "failed) - restart maxibitx\n");
      atomic_store(&outcome, CODEC_CHECK_BAD_STATE);
      break;
    }
    sleep_ms(RESTART_SETTLE_MS);
    s = measure();
  }
  fflush(stdout);
  return NULL;
}

int codec_check_start(int (*restart)(void)) {
  restart_fn = restart;
  atomic_store(&outcome, CODEC_CHECK_RUNNING);
  pthread_t t;
  if (sem_init(&done, 0, 0) != 0 || pthread_create(&t, NULL, check, NULL) != 0) {
    atomic_store(&outcome, CODEC_CHECK_UNMEASURED); // nothing to wait for
    return -1;
  }
  pthread_detach(t);
  return 0;
}

int codec_check_outcome(void) { return atomic_load(&outcome); }

// The bus the kernel found the WM8731 on: /sys/bus/i2c/devices/<bus>-001a
// whose name is wm8731. -1 if there's none.
static int wm8731_bus(void) {
  DIR *d = opendir("/sys/bus/i2c/devices");
  if (!d)
    return -1;
  struct dirent *e;
  int bus = -1;
  while (bus < 0 && (e = readdir(d))) {
    int b;
    unsigned addr;
    char rest;
    if (sscanf(e->d_name, "%d-%x%c", &b, &addr, &rest) != 2 || addr != WM8731_ADDR)
      continue;
    char path[300], name[32] = "";
    snprintf(path, sizeof(path), "/sys/bus/i2c/devices/%s/name", e->d_name);
    FILE *f = fopen(path, "r");
    if (f) {
      if (fgets(name, sizeof(name), f) && !strncmp(name, "wm8731", 6))
        bus = b;
      fclose(f);
    }
  }
  closedir(d);
  return bus;
}

// One register write. The WM8731 takes 7 bits of address and 9 of data.
static int wm8731_write(int fd, int reg, int value) {
  uint8_t b[2] = {(uint8_t)(reg << 1 | (value >> 8 & 1)), (uint8_t)value};
  struct i2c_msg msg = {.addr = WM8731_ADDR, .flags = 0, .len = 2, .buf = b};
  struct i2c_rdwr_ioctl_data x = {.msgs = &msg, .nmsgs = 1};
  return ioctl(fd, I2C_RDWR, &x) == 1 ? 0 : -1;
}

int codec_check_wm8731_restart(void) {
  int bus = wm8731_bus();
  if (bus < 0)
    return -1;
  char dev[32];
  snprintf(dev, sizeof(dev), "/dev/i2c-%d", bus);
  int fd = open(dev, O_RDWR);
  if (fd < 0)
    return -1;
  // ACTIVE back to 1 even if the 0 didn't take, so the codec is never
  // left with its interface off
  int r = wm8731_write(fd, WM8731_ACTIVE, 0);
  sleep_ms(5);
  r |= wm8731_write(fd, WM8731_ACTIVE, 1);
  close(fd);
  return r ? -1 : 0;
}
