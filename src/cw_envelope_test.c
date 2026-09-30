// cw_envelope_test.c
//
// Bench harness for cw.c's keying: the envelope and its 1:1 weighting
// correction, with the key's edges arriving the way they do on the radio.
// Each edge goes through key_input.c's steps at its own timestamp (as its
// input thread would deliver it), cw_poll_key() takes them once per block
// against a simulated capture time, and cw_get_sample() runs for every
// sample of every block that transmits, as sound.c does. The envelope is
// measured at its 50% points (cw_envelope_level()). Checks:
//
//   - the hold cw_init() derives from the table is 150 samples;
//   - each mark and space, between the envelope's 50% points, equals the
//     time between the key's edges - to within 0.23 samples when the edges
//     fall on block boundaries, and within a sample more (placing an edge
//     rounds down to a sample) anywhere else, including a mark shorter than
//     a block, and when capture reads return late - by scheduling latency,
//     or by 8 ms once, as when the audio thread stalls and catches up;
//   - the envelope is back at the table's floor after the last element;
//   - in CW, TX is requested by the input thread's hook before the audio
//     thread has taken the edge, once per burst, and released once, after
//     the fall and the hang time; in USB the ring is PTT and the tip does
//     nothing; a change to DIGITAL releases a TX the key started; once the
//     audio thread has stopped (cw_audio_stopped()) a closure keys nothing.
//
//   make test-cw && ./test-cw
//
// Links cw.c, key_input.c, gpio.c and vfo.c, with radio_get_mode() and
// radio_set_tx() stubbed below. gpio.c is linked but never called.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cw.h"
#include "key_input.h"
#include "radio.h"
#include "vfo.h"

#define BLOCK 1024
#define FS 96000.0
#define EXPECTED_HOLD 150
#define T0 1000000000LL // simulated capture clock, ns; 0 is kept clear
#define MS 1000000LL

// --- stubs for the calls cw.c makes into the radio ---
static enum radio_mode mode = RADIO_MODE_CW;
static int tx_on_calls = 0, tx_off_calls = 0;
static long tx_on_block = -1, tx_off_block = -1; // block being prepared at the call
static int in_audio_thread = 0, tx_on_from_input_thread = 0;
static long cur_block = 0;

enum radio_mode radio_get_mode(void) { return mode; }
int radio_set_tx(int on) {
  if (on) {
    tx_on_calls++;
    tx_on_block = cur_block;
    if (!in_audio_thread)
      tx_on_from_input_thread = 1;
  } else {
    tx_off_calls++;
    tx_off_block = cur_block;
  }
  return 0;
}

static int failures = 0;
static void check(int ok, const char *what) {
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    failures++;
}

struct edge {
  int64_t ts; // ns after T0
  int contact;
  int closed;
};

// The codec's sample clock against CLOCK_MONOTONIC: positive runs slow.
static double codec_ppm = 0;

// When block k's capture read returns: when its period ends (k blocks of
// the codec's clock after T0), plus up to jitter_ns of scheduling latency -
// a read never returns early. With jitter, block 12's read is also 8 ms
// late, as when the audio thread stalls and then catches up.
static int64_t capture_ns(long k, int64_t jitter_ns) {
  int64_t t = T0 + (int64_t)llround(k * BLOCK * 1e9 / FS * (1 + codec_ppm * 1e-6));
  if (jitter_ns) {
    t += (int64_t)(((k * 2654435761u) >> 8) % 1001) * jitter_ns / 1000;
    if (k == 12)
      t += 8 * MS;
  }
  return t;
}

struct result {
  int marks, spaces;
  double worst_mark, worst_space; // samples, measured minus key time
  double end_level;
  int end_idle;
};

// Runs a whole burst: delivers edges[] as their times come, polls and
// generates each block, and measures the envelope's marks and spaces
// against the key's edges. Runs first_block blocks before the `blocks` it
// records, for the clock estimate to settle.
static struct result run_from(const struct edge *edges, int n_edges, long first_block,
                              long blocks, int64_t jitter_ns) {
  long n = blocks * BLOCK;
  double *env = calloc(n, sizeof(double));
  if (!env)
    exit(2);

  cw_init();
  key_input_arm(0, T0, 3, cw_key_closed);
  tx_on_calls = tx_off_calls = 0;
  tx_on_block = tx_off_block = -1;
  tx_on_from_input_thread = 0;

  int next_edge = 0;
  for (long k = 0; k < first_block + blocks; k++) {
    cur_block = k;
    int64_t t_end = capture_ns(k, jitter_ns);
    // The input thread: every edge that happened before this capture.
    in_audio_thread = 0;
    while (next_edge < n_edges && T0 + edges[next_edge].ts < t_end) {
      key_input_tick(T0 + edges[next_edge].ts);
      key_input_edge(edges[next_edge].contact, edges[next_edge].closed,
                     T0 + edges[next_edge].ts);
      next_edge++;
    }
    key_input_tick(t_end);
    // The audio thread.
    in_audio_thread = 1;
    cw_poll_key(t_end, BLOCK);
    if (cw_tx_active() && (mode == RADIO_MODE_CW || mode == RADIO_MODE_CWR)) {
      for (int i = 0; i < BLOCK; i++) {
        (void)cw_get_sample();
        if (k >= first_block)
          env[(k - first_block) * BLOCK + i] = cw_envelope_level();
      }
    }
  }

  // Where each edge belongs in the output: one block after its own
  // interval, at its position within that interval - by the true sample
  // clock, i.e. the capture times without jitter.
  struct result r = {0};
  double prev_edge = -1, prev_cross = -1;
  int prev_closed = 0;
  long search = 1;
  for (int e = 0; e < n_edges && mode != RADIO_MODE_USB; e++) {
    long k = 0;
    while (capture_ns(k + 1, 0) <= T0 + edges[e].ts)
      k++;
    int64_t a = capture_ns(k, 0), b = capture_ns(k + 1, 0);
    double at = (k + 1) * BLOCK + (double)(T0 + edges[e].ts - a) * BLOCK / (double)(b - a);
    at -= (double)first_block * BLOCK;
    // The envelope's matching 50% crossing.
    long j = search;
    for (; j < n; j++)
      if (edges[e].closed ? (env[j - 1] < 0.5 && env[j] >= 0.5)
                          : (env[j - 1] >= 0.5 && env[j] < 0.5))
        break;
    if (j >= n) {
      check(0, "every edge has an envelope crossing");
      break;
    }
    double cross = (j - 1) + (0.5 - env[j - 1]) / (env[j] - env[j - 1]);
    search = j + 1;
    if (prev_edge >= 0) {
      double err = (cross - prev_cross) - (at - prev_edge);
      if (prev_closed) {
        r.marks++;
        if (fabs(err) > fabs(r.worst_mark))
          r.worst_mark = err;
      } else {
        r.spaces++;
        if (fabs(err) > fabs(r.worst_space))
          r.worst_space = err;
      }
    }
    prev_edge = at;
    prev_cross = cross;
    prev_closed = edges[e].closed;
  }
  r.end_level = cw_envelope_level();
  r.end_idle = !cw_tx_active();
  free(env);
  return r;
}

static struct result run(const struct edge *edges, int n_edges, long blocks, int64_t jitter_ns) {
  return run_from(edges, n_edges, 0, blocks, jitter_ns);
}

static void report(const struct result *r, double bound) {
  char msg[160];
  snprintf(msg, sizeof msg, "%d marks at the 50%% points: worst error %+.2f samples (bound %.2f)",
           r->marks, r->worst_mark, bound);
  check(r->marks > 0 && fabs(r->worst_mark) < bound, msg);
  if (r->spaces > 0) {
    snprintf(msg, sizeof msg, "%d spaces at the 50%% points: worst error %+.2f samples",
             r->spaces, r->worst_space);
    check(fabs(r->worst_space) < bound, msg);
  }
  snprintf(msg, sizeof msg, "envelope back at the floor (%.6f)", r->end_level);
  check(r->end_level < 1e-3, msg);
}

// A dit train: n_dits marks of dit_ms, spaces of dit_ms, from start_ms, on
// one contact.
static int dits(struct edge *e, double start_ms, double dit_ms, int n_dits, int contact) {
  int n = 0;
  for (int i = 0; i < n_dits; i++) {
    e[n++] = (struct edge){(int64_t)llround((start_ms + 2 * i * dit_ms) * MS), contact, 1};
    e[n++] = (struct edge){(int64_t)llround((start_ms + (2 * i + 1) * dit_ms) * MS), contact, 0};
  }
  return n;
}

int main(void) {
  vfo_init_phase_table();
  char msg[160];
  struct edge e[64];
  struct result r;

  printf("hold\n");
  cw_init();
  snprintf(msg, sizeof msg, "cw_weighting_hold_samples() = %d, expected %d",
           cw_weighting_hold_samples(), EXPECTED_HOLD);
  check(cw_weighting_hold_samples() == EXPECTED_HOLD, msg);

  printf("edges on block boundaries (2-block marks and spaces)\n");
  int n = 0;
  for (int i = 0; i < 4; i++) {
    e[n++] = (struct edge){capture_ns(3 + 4 * i, 0) - T0, KEY_RING, 1};
    e[n++] = (struct edge){capture_ns(5 + 4 * i, 0) - T0, KEY_RING, 0};
  }
  r = run(e, n, 60, 0);
  report(&r, 0.5);

  printf("60 WPM dits, edges anywhere in a block\n");
  n = dits(e, 23.456, 20.0, 10, KEY_RING);
  r = run(e, n, 80, 0);
  report(&r, 1.3);
  check(tx_on_calls == 1 && tx_off_calls == 1, "TX requested once and released once");
  check(tx_on_from_input_thread, "TX requested by the input thread's hook");
  snprintf(msg, sizeof msg, "requested while preparing block %ld, before the audio thread took "
                            "the first edge (block 3)", tx_on_block);
  check(tx_on_block == 3, msg);
  long last = 0;
  while (capture_ns(last, 0) <= T0 + e[n - 1].ts)
    last++;
  snprintf(msg, sizeof msg, "released at block %ld, after the last element (block %ld) and the "
                            "28-block hang", tx_off_block, last);
  check(tx_off_block > last + 28 && r.end_idle, msg);

  printf("20 WPM dits on the tip\n");
  n = dits(e, 7.7, 60.0, 5, KEY_TIP);
  r = run(e, n, 100, 0);
  report(&r, 1.3);

  printf("a 5 ms tap inside one block, 480.5 samples long\n");
  e[0] = (struct edge){(int64_t)(31.2 * MS), KEY_RING, 1};
  e[1] = (struct edge){(int64_t)(31.2 * MS) + 5005208, KEY_RING, 0};
  r = run(e, 2, 50, 0);
  report(&r, 1.3);

  printf("60 WPM dits, capture reads up to 300 us late, one 8 ms late\n");
  n = dits(e, 23.456, 20.0, 10, KEY_RING);
  r = run(e, n, 80, 300000);
  report(&r, 1.5);

  printf("the same, with the codec's clock 300 ppm slow, after 30 s to settle\n");
  codec_ppm = 300;
  long settle = 2813; // blocks in 30 s
  n = dits(e, settle * 10.6667 + 23.456, 20.0, 10, KEY_RING);
  r = run_from(e, n, settle, 80, 300000);
  report(&r, 1.5);
  codec_ppm = 0;

  printf("USB: the ring is mic PTT, the tip does nothing\n");
  mode = RADIO_MODE_USB;
  e[0] = (struct edge){(int64_t)(50.0 * MS), KEY_TIP, 1};
  e[1] = (struct edge){(int64_t)(80.0 * MS), KEY_TIP, 0};
  run(e, 2, 20, 0);
  check(tx_on_calls == 0, "tip: no TX");
  e[0] = (struct edge){(int64_t)(50.0 * MS), KEY_RING, 1};
  e[1] = (struct edge){(int64_t)(200.0 * MS), KEY_RING, 0};
  run(e, 2, 40, 0);
  snprintf(msg, sizeof msg, "ring: TX on at block %ld and off at block %ld, no hang",
           tx_on_block, tx_off_block);
  check(tx_on_calls == 1 && tx_off_calls == 1 && tx_on_block == 5 && tx_off_block == 19, msg);
  mode = RADIO_MODE_CW;

  printf("CW to DIGITAL inside the hang time\n");
  e[0] = (struct edge){(int64_t)(50.0 * MS), KEY_RING, 1};
  e[1] = (struct edge){(int64_t)(70.0 * MS), KEY_RING, 0};
  cw_init();
  key_input_arm(0, T0, 3, cw_key_closed);
  tx_on_calls = tx_off_calls = 0;
  int next = 0;
  for (long k = 0; k < 20; k++) {
    cur_block = k;
    int64_t t_end = capture_ns(k, 0);
    while (next < 2 && T0 + e[next].ts < t_end) {
      key_input_edge(e[next].contact, e[next].closed, T0 + e[next].ts);
      next++;
    }
    key_input_tick(t_end);
    if (k == 10)
      mode = RADIO_MODE_DIGITAL; // key up since block 7; hang runs to block 35
    cw_poll_key(t_end, BLOCK);
  }
  snprintf(msg, sizeof msg, "released on the first DIGITAL block, 10 (%d on, %d off, at %ld)",
           tx_on_calls, tx_off_calls, tx_off_block);
  check(tx_on_calls == 1 && tx_off_calls == 1 && tx_off_block == 10 && !cw_tx_active(), msg);
  mode = RADIO_MODE_CW;

  printf("after the audio thread stops\n");
  cw_init();
  key_input_arm(0, T0, 3, cw_key_closed);
  cw_poll_key(capture_ns(1, 0), BLOCK);
  key_input_edge(KEY_RING, 1, capture_ns(1, 0) + 1 * MS);
  check(cw_tx_active(), "a closure keys TX while it runs");
  tx_on_calls = tx_off_calls = 0;
  cw_audio_stopped();
  check(!cw_tx_active() && tx_off_calls == 1, "stopping releases it");
  key_input_edge(KEY_RING, 0, capture_ns(1, 0) + 20 * MS);
  key_input_edge(KEY_RING, 1, capture_ns(1, 0) + 40 * MS);
  check(!cw_tx_active() && tx_on_calls == 0, "a closure afterwards keys nothing");

  printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
