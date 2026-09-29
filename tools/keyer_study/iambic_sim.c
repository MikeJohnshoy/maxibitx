// iambic_sim.c - paddle-sampling study for
// docs/dsp_design_notes/cw_keyer_design_study.md §5. Not built by the
// Makefile: gcc -O2 -std=gnu11 iambic_sim.c -o iambic_sim && ./iambic_sim
//
// Ports the KB2ML keyer state machine from sbitx dev-54bugfixes
// (modem_cw.c: handle_mode_iambic_common(), handle_mode_ultimatic(),
// handle_mode_bug(), and the keydown_count/keyup_count sample loop in
// cw_tx_get_sample()) and runs it at 96 kHz against scripted paddle
// input, reading that input three ways:
//
//   ideal  - paddle state read on every sample
//   ref    - paddle state refreshed every 96 samples (1 ms), which is what
//            the reference actually does: key_poll() runs from the GTK
//            ui_tick timer (g_timeout_add(1, ...)), and the 96 kHz loop only
//            reads the variable it leaves behind
//   block  - paddle state refreshed once per 1024-sample audio block, as
//            maxibitx's cw_poll_key() does today
//
// The state machine is identical in all three; only when it learns about
// the paddles differs. A fourth scheme - kernel-timestamped edges replayed
// one block late - is ideal shifted by exactly one block by construction,
// so it is not simulated separately.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 96000
enum { IDLE = 0, DASH = 1, DOT = 2, SQUEEZE = 64 };
enum { M_IAMBIC_A, M_IAMBIC_B, M_ULTIMATIC, M_BUG };

struct keyer {
  int period, keydown, keyup;
  int cur, last, next, next_flag;
};

static void send_dot(struct keyer *k) { k->keydown = k->period; k->keyup = k->period; }
static void send_dash(struct keyer *k) { k->keydown = 3 * k->period; k->keyup = k->period; }
static void key_on_short(struct keyer *k) { k->keydown = 1; k->keyup = 0; }
static void key_off_short(struct keyer *k) { k->keydown = 0; k->keyup = 1; }
static void schedule(struct keyer *k, int s) { k->next = s; k->next_flag = 1; }
static void send_now(struct keyer *k, int s) {
  if (s == DOT) { send_dot(k); k->last = DOT; } else { send_dash(k); k->last = DASH; }
}
static void sched_opp(struct keyer *k) { schedule(k, k->last == DOT ? DASH : DOT); }

static void iambic(struct keyer *k, int s, int modeB) {
  if (k->next_flag && k->keyup == 0) { send_now(k, k->next); k->next_flag = 0; return; }
  switch (k->cur) {
  case IDLE:
    if (s == DOT) { if (k->keyup == 0) send_now(k, DOT); k->cur = DOT; }
    else if (s == DASH) { if (k->keyup == 0) send_now(k, DASH); k->cur = DASH; }
    else if (s == SQUEEZE) { if (k->keyup == 0) { send_now(k, DOT); schedule(k, DASH); } k->cur = SQUEEZE; }
    break;
  case DOT:
    if (s == DOT) { if (k->keyup == 0) send_now(k, DOT); }
    else if (s == DASH) { if (k->keyup == 0) send_now(k, DASH); else schedule(k, DASH); k->cur = IDLE; }
    else if (s == SQUEEZE) { if (k->keydown > 0) sched_opp(k); k->cur = SQUEEZE; }
    break;
  case DASH:
    if (s == DASH) { if (k->keyup == 0) send_now(k, DASH); }
    else if (s == DOT) { if (k->keyup == 0) send_now(k, DOT); else schedule(k, DOT); k->cur = IDLE; }
    else if (s == SQUEEZE) { if (k->keydown > 0) sched_opp(k); k->cur = SQUEEZE; }
    break;
  case SQUEEZE:
    if (s == IDLE) k->cur = IDLE;
    else if (s == DOT) { if (k->keyup == 0) send_dot(k); k->last = DOT; k->cur = DOT; }
    else if (s == DASH) { if (k->keyup == 0) send_dash(k); k->last = DASH; k->cur = DASH; }
    else if (s == SQUEEZE) {
      if (k->keyup == 0) {
        if (k->last == DOT) { send_dash(k); k->last = DASH; } else { send_dot(k); k->last = DOT; }
      }
      if (modeB) sched_opp(k);
      k->cur = SQUEEZE;
    }
    break;
  }
}

static void ultimatic(struct keyer *k, int s) {
  switch (k->cur) {
  case IDLE:
    if (s == DOT) { send_dot(k); k->cur = DOT; }
    else if (s == DASH) { send_dash(k); k->cur = DASH; }
    else if (s == SQUEEZE) { send_dot(k); k->last = DASH; k->cur = SQUEEZE; }
    break;
  case DOT:
    if (s == IDLE) k->cur = IDLE;
    else if (s == SQUEEZE) { send_dash(k); k->last = DASH; k->cur = SQUEEZE; }
    else if (s == DOT) { send_dot(k); k->cur = DOT; }
    else if (s == DASH) { send_dash(k); k->cur = DASH; }
    break;
  case DASH:
    if (s == IDLE) k->cur = IDLE;
    else if (s == SQUEEZE) { send_dot(k); k->last = DOT; k->cur = SQUEEZE; }
    else if (s == DOT) { send_dot(k); k->cur = DOT; }
    else if (s == DASH) { send_dash(k); k->cur = DASH; }
    break;
  case SQUEEZE:
    if (s == IDLE) k->cur = IDLE;
    else if (s == SQUEEZE) {
      if (k->last == DOT) { send_dot(k); k->last = DOT; } else { send_dash(k); k->last = DASH; }
      k->cur = SQUEEZE;
    } else if (s == DOT) { send_dot(k); k->cur = DOT; }
    else if (s == DASH) { send_dash(k); k->cur = DASH; }
    break;
  }
}

static void bug(struct keyer *k, int s) {
  switch (k->cur) {
  case IDLE:
    if (s == IDLE) { key_off_short(k); k->cur = IDLE; }
    else if (s == DOT) { send_dot(k); k->cur = DOT; }
    else if (s == DASH) { key_on_short(k); k->cur = DASH; }
    else if (s == SQUEEZE) k->cur = IDLE;
    break;
  case DOT:
  case DASH:
    if (s == IDLE) k->cur = IDLE;
    else if (s == DOT) { send_dot(k); k->cur = DOT; }
    else if (s == DASH) { key_on_short(k); k->cur = DASH; }
    break;
  default: k->cur = IDLE;
  }
}

// Paddle script: press/release sample times per paddle (-1 = never).
struct script { long dot_on, dot_off, dash_on, dash_off, dot2_on, dot2_off; };

static int paddle_at(const struct script *sc, long t) {
  int dot = (t >= sc->dot_on && t < sc->dot_off && sc->dot_on >= 0) ||
            (t >= sc->dot2_on && t < sc->dot2_off && sc->dot2_on >= 0);
  int dash = (t >= sc->dash_on && t < sc->dash_off && sc->dash_on >= 0);
  if (dot && dash) return SQUEEZE;
  return dot ? DOT : dash ? DASH : IDLE;
}

// Runs the keyer. refresh = how often the paddle variable is updated
// (1 = every sample), phase = sample offset of the first refresh.
// Writes element (start, length) pairs; returns the element count.
#define MAXEL 64
static int run(int mode, int wpm, const struct script *sc, int refresh, int phase,
               long *start, long *len, long horizon) {
  struct keyer k = {0};
  k.period = 115200 / wpm; // modem_cw.c: cw_period = (12 * 9600) / wpm
  int held = IDLE, n = 0, was_down = 0;
  long down_at = 0;
  for (long t = 0; t < horizon; t++) {
    if (refresh == 1 || ((t - phase) % refresh + refresh) % refresh == 0) held = paddle_at(sc, t);
    int iamb = (mode == M_IAMBIC_A || mode == M_IAMBIC_B);
    // cw_tx_get_sample(): iambic modes evaluate every sample, the others
    // only when both counters are idle.
    if (iamb || (k.keydown == 0 && k.keyup == 0)) {
      if (mode == M_IAMBIC_A) iambic(&k, held, 0);
      else if (mode == M_IAMBIC_B) iambic(&k, held, 1);
      else if (mode == M_ULTIMATIC) ultimatic(&k, held);
      else bug(&k, held);
    }
    int down = 0;
    if (k.keydown > 0) { down = 1; k.keydown--; }
    else if (k.keyup > 0) { k.keyup--; }
    if (down && !was_down) down_at = t;
    if (!down && was_down && n < MAXEL) { start[n] = down_at; len[n] = t - down_at; n++; }
    was_down = down;
  }
  return n;
}

// Element classes by length: dot ~1 period, dash ~3 periods, anything
// else (bug dashes) is "manual".
// cut_after_dash: keep only the prefix through the first dash, for
// scenarios whose outcome is "how many dots before the switch" and whose
// tail is an open-ended repeat. manual: bug mode, where a non-dot element
// is the operator's own and its length is a separate question (straight-key
// edge quantization, covered analytically in the design note).
static void signature_ex(int n, const long *len, int period, char *out, int cut_after_dash, int manual) {
  int i, o = 0;
  for (i = 0; i < n && i < MAXEL; i++) {
    double r = (double)len[i] / period;
    char c = r < 1.5 ? '.' : manual ? 'm' : (r < 3.5 && r > 2.5) ? '-' : 'm';
    out[o++] = c;
    if (cut_after_dash && c == '-') break;
  }
  out[o] = 0;
}
static int g_cut, g_manual;
static void signature(int n, const long *len, int period, char *out) {
  signature_ex(n, len, period, out, g_cut, g_manual);
}

static const char *mode_name[] = { "iambic A", "iambic B", "ultimatic", "bug" };

// Sweeps the time of one input event over [from, to) dits and reports how
// often each sampling scheme sends a different element sequence than
// ideal sampling would.
static void sweep(const char *what, int mode, int wpm, int which,
                  double from, double to, double hold_dits) {
  int period = 115200 / wpm;
  long horizon = 30L * period + 4096;
  g_cut = (which == 2); g_manual = (mode == M_BUG);
  long s0[MAXEL], l0[MAXEL], s1[MAXEL], l1[MAXEL];
  char sig0[MAXEL + 1], sig1[MAXEL + 1];
  const int schemes[2] = { 96, 1024 };
  double mismatch[2] = {0, 0}, lat_sum[2] = {0, 0};
  double band_lo[2] = {1e9, 1e9}, band_hi[2] = {-1e9, -1e9};
  int points = 0, lat_n[2] = {0, 0}, boundaries = 0;
  char prev_sig[MAXEL + 1] = "";
  double amb_ms[2] = {0, 0};
  const double step = 0.02;
  for (double x = from; x < to; x += step) {
    long ev = (long)(x * period);
    struct script sc = { -1, -1, -1, -1, -1, -1 };
    if (which == 0) {           // squeeze at 0, release both at ev
      sc.dot_on = 0; sc.dot_off = ev; sc.dash_on = 0; sc.dash_off = ev;
    } else if (which == 1) {    // dash held for one dash, dot tapped at ev
      sc.dash_on = 0; sc.dash_off = (long)(0.5 * period);
      sc.dot_on = ev; sc.dot_off = ev + (long)(hold_dits * period);
    } else if (which == 2) {    // ultimatic: dot held, dash added at ev - dots give way to dashes
      sc.dot_on = 0; sc.dot_off = 12L * period; sc.dash_on = ev; sc.dash_off = 12L * period;
    } else {                    // bug: dot held for ev (auto dits), then a manual dash
      sc.dot_on = 0; sc.dot_off = ev; sc.dash_on = ev + 2L * period; sc.dash_off = ev + 5L * period;
    }
    int n0 = run(mode, wpm, &sc, 1, 0, s0, l0, horizon);
    signature(n0, l0, period, sig0);
    if (points > 0 && strcmp(sig0, prev_sig) != 0) boundaries++;
    strcpy(prev_sig, sig0);
    points++;
    for (int s = 0; s < 2; s++) {
      int R = schemes[s], trials = 0, bad = 0;
      for (int ph = 0; ph < R; ph += (R >= 1024 ? 32 : 4)) {
        int n1 = run(mode, wpm, &sc, R, ph, s1, l1, horizon);
        signature(n1, l1, period, sig1);
        trials++;
        if (strcmp(sig0, sig1) != 0) bad++;
        if (n0 > 0 && n1 > 0) { lat_sum[s] += (double)(s1[0] - s0[0]); lat_n[s]++; }
      }
      double frac = (double)bad / trials;
      mismatch[s] += frac;
      amb_ms[s] += frac * step * 1000.0 * period / FS;
      if (frac > 0) {
        if (x < band_lo[s]) band_lo[s] = x;
        if (x > band_hi[s]) band_hi[s] = x;
      }
    }
  }
  printf("%-10s %-34s %2d WPM (dit %5.1f ms): %2d decision points;", mode_name[mode], what, wpm,
         1000.0 * period / FS, boundaries);
  for (int s = 0; s < 2; s++) {
    double per = boundaries ? amb_ms[s] / boundaries : 0;
    printf("  %s %5.2f ms/point (%4.1f%% of a dit)", s == 0 ? "1 ms" : "block", per,
           100.0 * per / (1000.0 * period / FS));
  }
  printf("\n");
}

static void reaction(int wpm) {
  // A single dot press from idle, held for a full element: how late does
  // the first element start under each scheme?
  int period = 115200 / wpm;
  long s[MAXEL], l[MAXEL];
  struct script sc = { 5000, 5000 + period, -1, -1, -1, -1 };
  const int schemes[3] = { 1, 96, 1024 };
  printf("reaction, dot pressed from idle, %2d WPM:", wpm);
  for (int k = 0; k < 3; k++) {
    int R = schemes[k];
    double sum = 0, mx = 0;
    int trials = 0;
    for (int ph = 0; ph < R; ph++) {
      run(M_IAMBIC_B, wpm, &sc, R, ph, s, l, 5000L + 4L * period);
      double d = (s[0] - 5000) * 1000.0 / FS;
      sum += d; if (d > mx) mx = d; trials++;
    }
    printf("  %s mean %.2f ms max %.2f ms", R == 1 ? "ideal" : R == 96 ? "1 ms" : "block",
           sum / trials, mx);
  }
  printf("\n");
}

int main(void) {
  int speeds[] = { 20, 40, 60 };
  for (int i = 0; i < 3; i++) reaction(speeds[i]);
  printf("\n");
  for (int i = 0; i < 3; i++) {
    int w = speeds[i];
    sweep("squeeze, release both at t", M_IAMBIC_A, w, 0, 0.0, 8.0, 0);
    sweep("squeeze, release both at t", M_IAMBIC_B, w, 0, 0.0, 8.0, 0);
    sweep("dash, dot tapped at t for 0.5 dit", M_IAMBIC_B, w, 1, 0.0, 4.0, 0.5);
    sweep("dash, dot tapped at t for 1 dit", M_IAMBIC_B, w, 1, 0.0, 4.0, 1.0);
    sweep("dot held, dash added at t", M_ULTIMATIC, w, 2, 0.5, 8.0, 0);
    sweep("auto dits until t, then manual dash", M_BUG, w, 3, 1.0, 8.0, 0);
    printf("\n");
  }
  return 0;
}
