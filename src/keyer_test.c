// keyer_test.c
//
// Bench harness for keyer.h. Built twice: test-keyer against keyer.c, and
// test-keyer-straight against keyer_straight.c, where every case needing a
// mode that keyer offers is reported as not offered rather than failed - so
// the stub is held to the same interface.
//
//   make test-keyer && ./test-keyer
//
// 1. The golden cases of cw_keyer_design_study.md §15: element sequence,
//    and every mark the keyer times exactly T or 3T and every space between
//    elements exactly T, in samples.
// 2. Against the specification model (tools/keyer_study/keyer_spec_model.c,
//    compiled into this file): random paddle input in every mode at 5, 20
//    and 60 WPM, run through keyer_run_block() in blocks of random length,
//    must give the model's key stream sample for sample.
// 3. The rules the model doesn't cover: a speed change waits for the next
//    element start, a mode change for idle, the WPM range, the TX-wanted
//    return.
// 4. Text: spacing (T, 3T, 7T, exactly), prosigns, the surfaces'
//    translations, text arriving during a word gap, a closure or a stop
//    ending it after the element in progress, a speed change, a full queue.

#define main keyer_spec_model_main
#include "keyer_spec_model.c"
#undef main

#include "keyer.h"
#include "morse.h"

static int failures = 0;
static void check(int ok, const char *what) {
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    failures++;
}

static const enum keyer_mode to_keyer[] = {KEYER_STRAIGHT, KEYER_BUG, KEYER_ULTIMATIC,
                                           KEYER_IAMBIC_A, KEYER_IAMBIC_B};

// The model's DOT/DASH as the key jack delivers them, unreversed.
static struct key_event as_event(const struct edge *e, int offset) {
  struct key_event k;
  k.offset = offset;
  k.paddle = e->line == DOT ? KEY_DOT : KEY_DASH;
  k.contact = e->line == DOT ? KEY_TIP : KEY_RING;
  k.closed = (uint8_t)e->down;
  return k;
}

// Runs keyer.h over [0, horizon) with the model's edges, in blocks whose
// lengths come from block_len(i). Returns 0 if this keyer doesn't offer the
// mode.
static unsigned rng = 12345;
static unsigned rnd(void) {
  rng = rng * 1103515245u + 12345u;
  return (rng >> 8) & 0xffffff;
}
static int run_keyer(enum mode m, int wpm_, const struct edge *ev, int n_ev, long horizon,
                     unsigned char *key, int random_blocks) {
  if (keyer_set_mode(to_keyer[m]) != 0)
    return 0;
  keyer_set_wpm(wpm_);
  keyer_reset();
  static struct key_event block_ev[4096];
  static uint8_t out[4096];
  int e = 0;
  for (long t = 0; t < horizon;) {
    int n = random_blocks ? 1 + (int)(rnd() % 1500) : 1024;
    if (t + n > horizon)
      n = (int)(horizon - t);
    int k = 0;
    while (e < n_ev && ev[e].t < t + n) {
      block_ev[k] = as_event(&ev[e], (int)(ev[e].t - t));
      k++;
      e++;
    }
    keyer_run_block(block_ev, k, out, n);
    memcpy(key + t, out, (size_t)n);
    t += n;
  }
  return 1;
}

#define GE(x, line, down) {(long)((x) * 1000), line, down}

struct golden {
  const char *what;
  enum mode mode;
  int wpm;
  struct edge ev[8];
  int n_ev;
  double horizon_dits;
  const char *expect;
};

static void golden_cases(void) {
  // The table in keyer_spec_model.c, edge times in thousandths of a dit.
  static const struct golden cases[] = {
      {"single dot tap from idle", IAMBIC_A, 20, {GE(0, DOT, 1), GE(0.5, DOT, 0)}, 2, 6, "."},
      {"dot held for 5 dits", IAMBIC_A, 20, {GE(0, DOT, 1), GE(5.0, DOT, 0)}, 2, 12, "..."},
      {"C, squeeze dah-first, release in the last dit (A)", IAMBIC_A, 20,
       {GE(0, DASH, 1), GE(0.5, DOT, 1), GE(10.5, DOT, 0), GE(10.5, DASH, 0)}, 4, 20, "-.-."},
      {"same squeeze released in the second dah (A)", IAMBIC_A, 20,
       {GE(0, DASH, 1), GE(0.5, DOT, 1), GE(6.5, DOT, 0), GE(6.5, DASH, 0)}, 4, 20, "-.-"},
      {"same squeeze released in the second dah (B)", IAMBIC_B, 20,
       {GE(0, DASH, 1), GE(0.5, DOT, 1), GE(6.5, DOT, 0), GE(6.5, DASH, 0)}, 4, 20, "-.-."},
      {"dah with a dot tapped inside it (A)", IAMBIC_A, 20,
       {GE(0, DASH, 1), GE(0.3, DASH, 0), GE(1.0, DOT, 1), GE(1.2, DOT, 0)}, 4, 12, "-."},
      {"dah with a dot tapped inside it (B)", IAMBIC_B, 20,
       {GE(0, DASH, 1), GE(0.3, DASH, 0), GE(1.0, DOT, 1), GE(1.2, DOT, 0)}, 4, 12, "-."},
      {"squeeze from idle, dot closed 1/10 dit first", IAMBIC_B, 20,
       {GE(0, DOT, 1), GE(0.1, DASH, 1), GE(0.5, DOT, 0), GE(0.5, DASH, 0)}, 4, 12, ".-"},
      {"squeeze from idle, dah closed 1/10 dit first", IAMBIC_B, 20,
       {GE(0, DASH, 1), GE(0.1, DOT, 1), GE(0.5, DOT, 0), GE(0.5, DASH, 0)}, 4, 12, "-."},
      {"ultimatic: dit held, dah added, dah released", ULTIMATIC, 20,
       {GE(0, DOT, 1), GE(3.0, DASH, 1), GE(10.5, DASH, 0), GE(14.5, DOT, 0)}, 4, 24, "..--.."},
      {"ultimatic: dah held, dit tapped (memory)", ULTIMATIC, 20,
       {GE(0, DASH, 1), GE(1.0, DOT, 1), GE(1.3, DOT, 0), GE(3.5, DASH, 0)}, 4, 16, "-."},
      {"bug: dits while dit held, then a manual dash", BUG, 20,
       {GE(0, DOT, 1), GE(5.0, DOT, 0), GE(8.0, DASH, 1), GE(10.0, DASH, 0)}, 4, 16, "...m"},
      {"straight key follows the contact", STRAIGHT, 20,
       {GE(0, DOT, 1), GE(2.2, DOT, 0), GE(4.0, DOT, 1), GE(4.4, DOT, 0)}, 4, 8, "mm"},
      {"C at 1 WPM (A)", IAMBIC_A, 1,
       {GE(0, DASH, 1), GE(0.5, DOT, 1), GE(10.5, DOT, 0), GE(10.5, DASH, 0)}, 4, 20, "-.-."},
      {"C at 60 WPM (B)", IAMBIC_B, 60,
       {GE(0, DASH, 1), GE(0.5, DOT, 1), GE(6.5, DOT, 0), GE(6.5, DASH, 0)}, 4, 20, "-.-."},
  };
  printf("1. golden cases\n");
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    const struct golden *g = &cases[c];
    long T = (FS * 12L / 10 + g->wpm / 2) / g->wpm;
    struct edge ev[8];
    for (int i = 0; i < g->n_ev; i++) {
      ev[i] = g->ev[i];
      ev[i].t = 1000 + g->ev[i].t * T / 1000;
    }
    long horizon = 1000 + (long)(g->horizon_dits * T);
    unsigned char *key = calloc((size_t)horizon, 1);
    char msg[160];
    if (!run_keyer(g->mode, g->wpm, ev, g->n_ev, horizon, key, 0)) {
      printf("  --   %-9s %-52s not offered by this keyer\n", mode_name[g->mode], g->what);
      free(key);
      continue;
    }
    char got[64];
    int timing_ok;
    decode(key, horizon, T, got, &timing_ok, g->mode == STRAIGHT);
    snprintf(msg, sizeof msg, "%-9s %2d WPM  %-50s %-7s got %s%s", mode_name[g->mode], g->wpm,
             g->what, g->expect, got, timing_ok ? "" : " (timing)");
    check(strcmp(got, g->expect) == 0 && timing_ok, msg);
    free(key);
  }
}

static void against_model(void) {
  printf("2. against the specification model, random paddle input\n");
  static const int speeds[] = {5, 20, 60};
  for (int m = STRAIGHT; m <= IAMBIC_B; m++) {
    if (keyer_set_mode(to_keyer[m]) != 0) {
      printf("  --   %-9s not offered by this keyer\n", mode_name[m]);
      continue;
    }
    int runs = 0, mismatches = 0;
    long first_bad = -1;
    for (int s = 0; s < 3; s++) {
      int w = speeds[s];
      long T = (FS * 12L / 10 + w / 2) / w;
      for (int r = 0; r < 40; r++) {
        // Up to 24 edges over ~30 dits, each paddle toggling at random
        // times, some well inside an element and some near its edges.
        struct edge ev[24];
        int n_ev = 0, st[2] = {0, 0};
        long t = 500;
        while (n_ev < 24) {
          t += 1 + (long)(rnd() % (unsigned)(T * 3 / 2));
          if (t > 30 * T)
            break;
          int line = (int)(rnd() & 1);
          st[line] = !st[line];
          ev[n_ev++] = (struct edge){t, line, st[line]};
        }
        for (int l = 0; l < 2; l++)
          if (st[l])
            ev[n_ev++] = (struct edge){t + 1, l, 0};
        long horizon = t + 12 * T;
        unsigned char *want = calloc((size_t)horizon, 1);
        unsigned char *got = calloc((size_t)horizon, 1);
        run((enum mode)m, w, ev, n_ev, horizon, want);
        run_keyer((enum mode)m, w, ev, n_ev, horizon, got, 1);
        runs++;
        for (long i = 0; i < horizon; i++)
          if (want[i] != got[i]) {
            mismatches++;
            if (first_bad < 0)
              first_bad = i;
            break;
          }
        free(want);
        free(got);
      }
    }
    char msg[120];
    snprintf(msg, sizeof msg, "%-9s %d random runs at 5/20/60 WPM identical to the model (%d differ)",
             mode_name[m], runs, mismatches);
    check(mismatches == 0, msg);
  }
}

// Key-down runs in key[0..n): start and length of each, in samples.
static int marks(const uint8_t *key, long n, long *start, long *len, int max) {
  int k = 0;
  for (long i = 0; i < n && k < max; i++)
    if (key[i] && (i == 0 || !key[i - 1])) {
      long j = i;
      while (j < n && key[j])
        j++;
      start[k] = i;
      len[k++] = j - i;
      i = j;
    }
  return k;
}

static void other_rules(void) {
  printf("3. speed and mode changes, limits, TX wanted\n");
  static uint8_t key[400000];
  long start[16], len[16];
  char msg[160];

  if (keyer_set_mode(KEYER_IAMBIC_A) != 0) {
    printf("  --   iambic not offered by this keyer: speed and mode rules skipped\n");
  } else {
    // Dash paddle held from sample 0. 20 WPM: T = 5760, a dash 17280. The
    // speed goes to 40 WPM (T = 2880) during the first dash.
    keyer_set_wpm(20);
    keyer_reset();
    struct key_event down = {.offset = 0, .contact = KEY_RING, .paddle = KEY_DASH, .closed = 1};
    keyer_run_block(&down, 1, key, 1024);
    long t = 1024;
    while (t < 10000) {
      keyer_run_block(NULL, 0, key + t, 1024);
      t += 1024;
    }
    keyer_set_wpm(40);
    while (t < 60000) {
      keyer_run_block(NULL, 0, key + t, 1024);
      t += 1024;
    }
    int k = marks(key, t, start, len, 16);
    snprintf(msg, sizeof msg, "speed change mid-dash: that dash stays %ld, the next is %ld (want "
                              "17280, 8640), space %ld (want 5760)",
             len[0], k > 1 ? len[1] : -1, k > 1 ? start[1] - (start[0] + len[0]) : -1);
    check(k > 1 && len[0] == 17280 && len[1] == 8640 && start[1] - 17280 == 5760, msg);

    // Mode change to straight while the dash is still held: nothing changes
    // until the paddle is released and the keyer goes idle.
    keyer_set_mode(KEYER_STRAIGHT);
    long t_mode = t;
    while (t < t_mode + 40000) {
      keyer_run_block(NULL, 0, key + t, 1024);
      t += 1024;
    }
    k = marks(key + t_mode, 40000, start, len, 16);
    snprintf(msg, sizeof msg, "mode change while busy: still keyer dashes (%d marks, first %ld)",
             k, k ? len[0] : -1);
    check(k >= 2 && (len[0] == 8640 || len[1] == 8640), msg);
    struct key_event up = {.offset = 0, .contact = KEY_RING, .paddle = KEY_DASH, .closed = 0};
    keyer_run_block(&up, 1, key + t, 1024);
    t += 1024;
    long t_up = t;
    while (t < t_up + 20000) {
      keyer_run_block(NULL, 0, key + t, 1024);
      t += 1024;
    }
    struct key_event pr[2] = {{.offset = 100, .contact = KEY_TIP, .paddle = KEY_DOT, .closed = 1},
                              {.offset = 600, .contact = KEY_TIP, .paddle = KEY_DOT, .closed = 0}};
    keyer_run_block(pr, 2, key + t, 1024);
    k = marks(key + t, 1024, start, len, 16);
    snprintf(msg, sizeof msg, "once idle it is a straight key: a 500-sample tap keys %ld samples",
             k ? len[0] : -1);
    check(k == 1 && start[0] == 100 && len[0] == 500, msg);
  }

  snprintf(msg, sizeof msg, "WPM clamps: 0 -> %d, 99 -> %d, 25 -> %d", keyer_set_wpm(0),
           keyer_set_wpm(99), keyer_set_wpm(25));
  check(keyer_set_wpm(0) == 1 && keyer_set_wpm(99) == 60 && keyer_set_wpm(25) == 25, msg);
  check(keyer_set_mode((enum keyer_mode)7) == -1, "an unknown mode is refused");

  if (keyer_set_mode(KEYER_IAMBIC_B) == 0) {
    // A dot tapped and released within one block: TX is still wanted in the
    // blocks after, through the dot and its trailing space.
    keyer_set_wpm(20);
    keyer_reset();
    struct key_event tap[2] = {{.offset = 10, .contact = KEY_TIP, .paddle = KEY_DOT, .closed = 1},
                               {.offset = 50, .contact = KEY_TIP, .paddle = KEY_DOT, .closed = 0}};
    int w0 = keyer_run_block(tap, 2, key, 1024);
    int wanted = 1;
    long t = 1024;
    for (; t < 5760 * 2; t += 1024)
      wanted &= keyer_run_block(NULL, 0, key + t, 1024);
    int w_after = keyer_run_block(NULL, 0, key + t, 1024);
    snprintf(msg, sizeof msg, "TX wanted through the dot and its space (%d, %d), not after (%d)",
             w0, wanted, w_after);
    check(w0 && wanted && !w_after, msg);
  }
  keyer_set_mode(KEYER_STRAIGHT);
  keyer_set_wpm(KEYER_WPM_DEFAULT);
}

// Runs blocks of 1024 until the keyer is idle with no text left (or `limit`
// samples), writing key[] from `at`. Returns the new end.
static long run_until_idle(uint8_t *key, long at, long limit) {
  long t = at;
  int quiet = 0;
  while (t < limit && quiet < 2) {
    int w = keyer_run_block(NULL, 0, key + t, 1024);
    quiet = (!w && !keyer_text_busy()) ? quiet + 1 : 0;
    t += 1024;
  }
  return t;
}

// Reads key[0..n) back as text: marks T or 3T are . or -, a space of T
// joins elements, 3T ends a character and 7T a word. Any other length sets
// *timing_ok to 0. Prosigns decode to their codes.
static void read_text(const uint8_t *key, long n, long T, char *out, int max, int *timing_ok) {
  char pat[16] = "";
  int np = 0, no = 0;
  *timing_ok = 1;
  long i = 0;
  while (i < n && !key[i])
    i++;
  while (i < n) {
    long s = i;
    while (i < n && key[i])
      i++;
    long len = i - s;
    if (len != T && len != 3 * T)
      *timing_ok = 0;
    if (np < 15)
      pat[np++] = len == T ? '.' : '-';
    pat[np] = 0;
    long g = i;
    while (i < n && !key[i])
      i++;
    long gap = i - g;
    int end_char = (i >= n) || gap == 3 * T || gap == 7 * T;
    if (i < n && gap != T && gap != 3 * T && gap != 7 * T)
      *timing_ok = 0;
    if (end_char) {
      char c = '?';
      for (int k = 32; k < MORSE_PROSIGN_END; k++)
        if (morse_pattern((unsigned char)k) && strcmp(morse_pattern((unsigned char)k), pat) == 0) {
          c = (char)k;
          break;
        }
      if (no < max - 1)
        out[no++] = c;
      if (i < n && gap == 7 * T && no < max - 1)
        out[no++] = ' ';
      np = 0;
    }
  }
  out[no] = 0;
}

static void text_rules(void) {
  printf("4. text\n");
  static uint8_t key[3000000];
  char msg[200], got[128], internal[128], skipped[32];
  int ok;

  morse_from_plain("cq <sk> 73 <xx>~", internal, sizeof internal, skipped, sizeof skipped);
  snprintf(msg, sizeof msg,
           "plain: lower case, <SK> one code; <xx> sends XX, its brackets and ~ skipped ('%s')",
           skipped);
  check(strcmp(internal, "CQ \x86 73 XX") == 0 && strcmp(skipped, "<>~") == 0, msg);
  morse_from_kenwood("tu_[<]", internal, sizeof internal, skipped, sizeof skipped);
  check(strcmp(internal, "TU\x80\x83\x81\x85") == 0 && skipped[0] == 0,
        "Kenwood: _ AR, [ BT, < AS, ] KN");

  if (keyer_send_text("E") == -2) {
    printf("  --   text not offered by this keyer\n");
    return;
  }
  keyer_stop_text();

  long T20 = (115200 + 10) / 20, T40 = (115200 + 20) / 40;
  keyer_set_mode(KEYER_IAMBIC_B);
  keyer_set_wpm(20);
  keyer_reset();
  keyer_send_text("PARIS PARIS");
  long n = run_until_idle(key, 0, sizeof key);
  read_text(key, n, T20, got, sizeof got, &ok);
  snprintf(msg, sizeof msg, "PARIS PARIS at 20 WPM reads back '%s', every mark and space exact", got);
  check(strcmp(got, "PARIS PARIS") == 0 && ok, msg);

  keyer_reset();
  morse_from_plain("cq <sk> 73", internal, sizeof internal, NULL, 0);
  keyer_send_text(internal);
  n = run_until_idle(key, 0, sizeof key);
  read_text(key, n, T20, got, sizeof got, &ok);
  check(strcmp(got, "CQ \x86 73") == 0 && ok, "a prosign is one character, no letter space inside");

  // Text arriving during a word gap continues after exactly 7T.
  keyer_reset();
  keyer_send_text("CQ ");
  long t = 0;
  for (int b = 0; b < 30; b++, t += 1024) // CQ is ~ 27T = 155520: stop inside the gap
    keyer_run_block(NULL, 0, key + t, 1024);
  check(keyer_text_busy(), "text still busy during the trailing word gap");
  keyer_send_text("DE");
  n = run_until_idle(key, t, sizeof key);
  read_text(key, n, T20, got, sizeof got, &ok);
  snprintf(msg, sizeof msg, "\"CQ \" then \"DE\" during the gap reads '%s' with a 7T word space", got);
  check(strcmp(got, "CQ DE") == 0 && ok, msg);

  // A dot paddle tapped during the second T of "TTTT": that dash completes,
  // the tap's memory sends a dot, and the rest of the text is gone.
  keyer_reset();
  keyer_send_text("TTTT");
  t = 0;
  long tap_at = 3 * T20 + 3 * T20 + T20; // inside the second dash
  struct key_event ev[2];
  while (t < 200000) {
    int k = 0;
    if (tap_at >= t && tap_at < t + 1024) {
      ev[k++] = (struct key_event){.offset = (int)(tap_at - t), .contact = KEY_TIP,
                                   .paddle = KEY_DOT, .closed = 1};
      ev[k++] = (struct key_event){.offset = (int)(tap_at - t) + 100, .contact = KEY_TIP,
                                   .paddle = KEY_DOT, .closed = 0};
    }
    keyer_run_block(ev, k, key + t, 1024);
    t += 1024;
  }
  long start[16], len[16];
  int m = marks(key, t, start, len, 16);
  snprintf(msg, sizeof msg, "paddle during text: %d marks (%ld, %ld, %ld), text discarded", m,
           m > 0 ? len[0] : -1, m > 1 ? len[1] : -1, m > 2 ? len[2] : -1);
  check(m == 3 && len[0] == 3 * T20 && len[1] == 3 * T20 && len[2] == T20 &&
            start[2] == start[1] + 4 * T20 && !keyer_text_busy(),
        msg);

  // A stop mid-text: the element in progress completes, nothing after.
  keyer_reset();
  keyer_send_text("EEEEEEEEEE");
  t = 0;
  for (int b = 0; b < 50; b++, t += 1024) // into the third E's mark
    keyer_run_block(NULL, 0, key + t, 1024);
  keyer_stop_text();
  n = run_until_idle(key, t, sizeof key);
  m = marks(key, n, start, len, 16);
  snprintf(msg, sizeof msg, "stop mid-text: %d of 10 marks sent, the last whole (%ld)", m,
           m ? len[m - 1] : -1);
  check(m == 3 && len[m - 1] == T20 && !keyer_text_busy(), msg);

  // A speed change during a character's first dash: the next dash is at the
  // new speed.
  keyer_reset();
  keyer_send_text("MM");
  t = 0;
  for (; t < 2 * 1024; t += 1024)
    keyer_run_block(NULL, 0, key + t, 1024);
  keyer_set_wpm(40);
  n = run_until_idle(key, t, sizeof key);
  m = marks(key, n, start, len, 16);
  snprintf(msg, sizeof msg, "speed change mid-dash: %ld then %ld (want %ld, %ld)",
           m > 0 ? len[0] : -1, m > 1 ? len[1] : -1, 3 * T20, 3 * T40);
  check(m == 4 && len[0] == 3 * T20 && len[1] == 3 * T40, msg);
  keyer_set_wpm(20);

  // Straight-key mode sends text too, with the same timing.
  keyer_set_mode(KEYER_STRAIGHT);
  keyer_reset();
  keyer_send_text("K");
  n = run_until_idle(key, 0, sizeof key);
  read_text(key, n, T20, got, sizeof got, &ok);
  check(strcmp(got, "K") == 0 && ok, "text in straight-key mode");

  // A full queue refuses the whole message.
  keyer_reset();
  static char big[KEYER_TEXT_QUEUE_LEN + 2];
  memset(big, 'E', sizeof big - 1);
  int room = keyer_text_room();
  int r = keyer_send_text(big);
  snprintf(msg, sizeof msg, "%zu characters refused (%d), room still %d", strlen(big), r,
           keyer_text_room());
  check(r == -1 && keyer_text_room() == room && room == KEYER_TEXT_QUEUE_LEN, msg);
  keyer_reset();
}

int main(void) {
  golden_cases();
  against_model();
  other_rules();
  text_rules();
  printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
