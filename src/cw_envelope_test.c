// cw_envelope_test.c
//
// Bench harness for cw.c's keying envelope and its 1:1 weighting correction.
// Drives the key a block at a time through cw_poll_key(), exactly as sound.c
// does, runs cw_get_sample() for every sample, and measures the envelope
// (cw_envelope_level()) at its 50% points. Checks:
//
//   - the hold cw_init() derives from the table is 150 samples;
//   - each mark, measured between its rising and falling 50% points, equals
//     the key-down time, and each space equals the key-up time, to within
//     half a sample (without the hold every mark would be ~150 samples short
//     and every space ~150 long);
//   - the envelope is back at the table's floor after the last element;
//   - TX is requested once at the first key-down, held through the spaces by
//     the hang timer, and released once, only after the envelope has fallen.
//
//   make test-cw && ./test-cw
//
// Links cw.c and vfo.c, with radio_hw_key_down(), radio_get_mode() and
// radio_set_tx() stubbed below, so no GPIO, I2C or ALSA is needed.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cw.h"
#include "radio.h"
#include "radio_hw.h"
#include "vfo.h"

#define BLOCK 1024
#define EXPECTED_HOLD 150

// --- stubs for the three calls cw.c makes into the radio ---
static int stub_key = 0;
static int tx_on_calls = 0, tx_off_calls = 0;
static long tx_off_at = -1; // sample index of the release
static long now = 0;        // current sample index

int radio_hw_key_down(void) { return stub_key; }
enum radio_mode radio_get_mode(void) { return RADIO_MODE_CW; }
int radio_set_tx(int on) {
    if (on) {
        tx_on_calls++;
    } else {
        tx_off_calls++;
        tx_off_at = now;
    }
    return 0;
}

static int failures = 0;
static void check(int ok, const char *what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

// Sample time where env crosses 0.5 between samples i-1 and i, linearly
// interpolated.
static double crossing(const double *env, long i) {
    return (i - 1) + (0.5 - env[i - 1]) / (env[i] - env[i - 1]);
}

// One run: a sequence of (key, blocks) pairs starting and ending key-up.
static void run(const char *name, const int *pattern, int n_steps) {
    printf("%s\n", name);
    long total_blocks = 0;
    for (int s = 0; s < n_steps; s++) total_blocks += pattern[2 * s + 1];
    total_blocks += 40; // hang (28 polls) plus the fall, with room to spare
    long n = total_blocks * BLOCK;

    double *env = malloc(sizeof(double) * n);
    int *key = malloc(sizeof(int) * n);
    if (!env || !key) exit(2);

    cw_init();
    tx_on_calls = tx_off_calls = 0;
    tx_off_at = -1;
    now = 0;

    int step = 0, left = pattern[1];
    for (long b = 0; b < total_blocks; b++) {
        if (step < n_steps) {
            stub_key = pattern[2 * step];
        } else {
            stub_key = 0;
        }
        cw_poll_key();
        for (int i = 0; i < BLOCK; i++, now++) {
            (void)cw_get_sample();
            env[now] = cw_envelope_level();
            key[now] = stub_key;
        }
        if (step < n_steps && --left == 0 && ++step < n_steps) left = pattern[2 * step + 1];
    }

    // Pair each key edge with the envelope's 50% crossing that follows it.
    double worst_mark = 0, worst_space = 0;
    int marks = 0, spaces = 0;
    long key_edge = -1;
    double env_edge = -1;
    long next_search = 1;
    for (long i = 1; i < n; i++) {
        if (key[i] == key[i - 1]) continue;
        // Find the matching envelope crossing, from this key edge onwards.
        long j = next_search > i ? next_search : i;
        for (; j < n; j++)
            if (key[i] ? (env[j - 1] < 0.5 && env[j] >= 0.5) : (env[j - 1] >= 0.5 && env[j] < 0.5))
                break;
        if (j >= n) {
            check(0, "every key edge has an envelope crossing");
            break;
        }
        double t = crossing(env, j);
        next_search = j + 1;
        if (key_edge >= 0) {
            double key_len = (double)(i - key_edge);
            double env_len = t - env_edge;
            double err = env_len - key_len;
            if (key[i - 1]) {
                marks++;
                if (fabs(err) > fabs(worst_mark)) worst_mark = err;
            } else {
                spaces++;
                if (fabs(err) > fabs(worst_space)) worst_space = err;
            }
        }
        key_edge = i;
        env_edge = t;
    }

    char msg[160];
    snprintf(msg, sizeof msg, "%d marks at the 50%% points: worst error %+.2f samples", marks,
             worst_mark);
    check(marks > 0 && fabs(worst_mark) < 0.5, msg);
    if (spaces > 0) {
        snprintf(msg, sizeof msg, "%d spaces at the 50%% points: worst error %+.2f samples",
                 spaces, worst_space);
        check(fabs(worst_space) < 0.5, msg);
    }
    snprintf(msg, sizeof msg, "envelope back at the floor at the end (%.6f)", env[n - 1]);
    check(env[n - 1] < 1e-3, msg);
    snprintf(msg, sizeof msg, "TX requested once (%d) and released once (%d)", tx_on_calls,
             tx_off_calls);
    check(tx_on_calls == 1 && tx_off_calls == 1, msg);
    if (tx_off_at >= 0) {
        snprintf(msg, sizeof msg, "TX released after the fall finished (envelope %.6f there)",
                 env[tx_off_at > 0 ? tx_off_at - 1 : 0]);
        check(env[tx_off_at > 0 ? tx_off_at - 1 : 0] < 1e-3, msg);
    }
    free(env);
    free(key);
}

int main(void) {
    vfo_init_phase_table();

    printf("hold\n");
    cw_init();
    char msg[80];
    snprintf(msg, sizeof msg, "cw_weighting_hold_samples() = %d, expected %d",
             cw_weighting_hold_samples(), EXPECTED_HOLD);
    check(cw_weighting_hold_samples() == EXPECTED_HOLD, msg);

    // Keying changes once per 1024-sample block, as cw_poll_key() sees it.
    // {key, blocks} pairs.
    static const int one_mark[] = {0, 3, 1, 2, 0, 1};
    static const int dits[] = {0, 2, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1};
    static const int mixed[] = {0, 2, 1, 3, 0, 1, 1, 1, 0, 3, 1, 3, 0, 1, 1, 1, 0, 7, 1, 2, 0, 1};
    run("one mark, 2 blocks", one_mark, 3);
    run("four one-block dits, one-block spaces", dits, 9);
    run("mixed marks and spaces, 1 to 7 blocks", mixed, 11);

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
