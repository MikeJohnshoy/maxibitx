// cw_decoder.c
//
// Received CW to text (cw_decoder.h). Three stages, all on the audio
// thread, all cheap enough not to matter there:
//
//   1. Envelope, every 1 ms. The audio (the CW tone at the pitch) is mixed
//      down to 0 Hz, summed 16 samples at a time to 6 kHz, lowpassed at
//      150 Hz (4th-order Butterworth, on I and Q) and its magnitude taken
//      every 6th sample.
//   2. Mark or gap, every 1 ms: half way between a tracked peak and a
//      tracked noise floor, with 10% hysteresis. The peak follows the
//      signal down over 0.5 s in marks and 10 s in gaps; the floor learns
//      in gaps only, quickly down, slowly up (and quickly both ways for
//      the first WARMUP_MS, which decide nothing). Nothing is a mark while the
//      peak is less than SQUELCH_RATIO times the floor. The time constants
//      are fixed, never tied to the speed being decoded, so the timing
//      stays true whatever the decoder thinks the speed is.
//   3. Marks and gaps to text. The last MARK_HIST mark lengths and
//      GAP_HIST gap lengths (in log ms) are each split in two by the
//      threshold that best separates them (Otsu: the largest
//      between-class variance) - dots from dashes, gaps inside characters
//      from longer ones - and the long gaps split again, between
//      characters from between words. A threshold sits between the two
//      classes' centres: half way for dot/dash and character/word,
//      CHAR_FRAC of the way up from the gaps inside characters. A run of
//      one kind of mark (E, T, S, O...) is told dot or dash by comparing it
//      with the gaps inside characters, which are one unit long. Nothing
//      is decoded until MIN_MARKS marks and MIN_GAPS gaps have been seen;
//      what came before is decoded then. A mark or gap shorter than
//      BLIP_DOTS of a dot (MIN_BLIP_MS at least) is noise and joins the
//      event before it. A gap of PAUSE_MS, and PAUSE_OVER_WORD word gaps,
//      ends the transmission: the timing is learned afresh, since the next
//      may be another station.
//
// A character is written as soon as the gap after it passes the character
// threshold, a space as soon as it passes the word threshold.
//
// tools/cw_study/cw_proto.py is the same classifier in Python, and
// tools/cw_study/cw_bench.py measures this file against it and against what
// the fists allow; docs/dsp_design_notes/rx_cw_decoder_study.md §8 has the
// numbers.

#include "cw_decoder.h"
#include "morse.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#ifdef CW_DECODER_TRACE
// tools/cw_study's trace build: every mark and gap as it is committed
#include <stdio.h>
#define TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

#define FS 96000
#define BOX 16                      // 96 kHz -> 6 kHz
#define FS_LO (FS / BOX)
#define TICK_LO (FS_LO / 1000)      // low-rate samples per 1 ms tick
#define LPF_HZ 150.0

// stage 2
#define PEAK_MARK_TAU_MS 500.0
#define PEAK_GAP_TAU_MS 10000.0
#define FLOOR_DOWN_TAU_MS 50.0
#define FLOOR_UP_TAU_MS 1000.0
#define HYSTERESIS 0.1f
#define SQUELCH_RATIO 4.0f
#define NOISE_TAU_MS 500.0
#define WARMUP_MS 300               // floor learned fast first, no marks yet
#define WARMUP_TAU_MS 30.0

// stage 3
#define MARK_HIST 32
#define GAP_HIST 64
#define MIN_MARKS 4
#define MIN_GAPS 4
#define PENDING_MAX 96
#define CHAR_FRAC 0.42f
#define WORD_FRAC 0.5f
#define BLIP_DOTS 0.15f
#define MIN_BLIP_MS 6
#define PAUSE_MS 2000
#define PAUSE_OVER_WORD 2.5f
#define MAX_CODE 9                  // HH is 8 elements
#define LOG_SEPARATED 0.5878f       // log(1.8): two classes are distinct
#define LOG_WORD_SEPARATED 0.4700f  // log(1.6)
#define LOG_WORD_OVER_CHAR 0.5306f  // log(1.7)
#define LOG_2 0.6931f
#define LOG_3 1.0986f
#define LOG_1_7 0.5306f

struct biquad {
  float b0, b1, b2, a1, a2, z1, z2;
};

struct event {
  char mark;
  int ms;
};

static struct {
  // stage 1
  int pitch_hz;
  double osc_re, osc_im, step_re, step_im;
  float acc_i, acc_q;
  int box_n, lo_n;
  struct biquad lp_i[2], lp_q[2];
  // stage 2
  int primed, warm_ms;
  float peak, floor_, noise;
  int mark;
  float a_peak_mark, a_peak_gap, a_floor_down, a_floor_up, a_noise;
  // events: the one being measured, and the last one closed, which a blip
  // can still join until the one after it is longer than a blip
  int seen_mark;
  int cur_mark, cur_ms;
  int last_valid, last_mark, last_ms;
  // stage 3
  float marks[MARK_HIST];
  int n_marks, marks_pos;
  float gaps[GAP_HIST];
  int n_gaps, gaps_pos;
  float t_mark, t_char, t_word, dot_ms;
  int t_char_ms, t_word_ms, pause_ms;
  struct event pending[PENDING_MAX];
  int n_pending;
  char code[MAX_CODE + 1];
  int code_len, code_overflow;
  int char_done, word_done, paused;
  int line_used, last_space;
} d;

static atomic_int enabled = 1;
static atomic_int reset_request = 0;
static atomic_int wpm_now = 0;

static char text_ring[CW_DECODER_TEXT_KEEP];
static atomic_uint text_head = 0;   // characters written since start

// ---------------------------------------------------------------- text

static void put_text(const char *s) {
  unsigned h = atomic_load_explicit(&text_head, memory_order_relaxed);
  for (; *s; s++) {
    text_ring[h % CW_DECODER_TEXT_KEEP] = *s;
    h++;
    atomic_store_explicit(&text_head, h, memory_order_release);
  }
}

static void put_char(int c) {
  if (c < 0) {
    put_text("*");
  } else if (morse_prosign_name((unsigned char)c)) {
    char buf[8] = "<";
    strcat(buf, morse_prosign_name((unsigned char)c));
    strcat(buf, ">");
    put_text(buf);
  } else {
    char buf[2] = {(char)c, 0};
    put_text(buf);
  }
  d.line_used = 1;
  d.last_space = 0;
}

static void put_space(void) {
  if (d.line_used && !d.last_space) {
    put_text(" ");
    d.last_space = 1;
  }
}

static void put_newline(void) {
  if (d.line_used) {
    put_text("\n");
    d.line_used = 0;
    d.last_space = 0;
  }
}

uint32_t cw_decoder_read(uint32_t since, char *buf, int len) {
  unsigned h = atomic_load_explicit(&text_head, memory_order_acquire);
  if (since > h)
    since = h;
  if (h - since > CW_DECODER_TEXT_KEEP)
    since = h - CW_DECODER_TEXT_KEEP;
  int n = (int)(h - since);
  if (len < 1)
    return since;
  if (n > len - 1)
    n = len - 1;
  for (int i = 0; i < n; i++)
    buf[i] = text_ring[(since + i) % CW_DECODER_TEXT_KEEP];
  buf[n] = '\0';
  // if the writer lapped the ring while we copied, drop what it overwrote
  unsigned h2 = atomic_load_explicit(&text_head, memory_order_acquire);
  if (h2 - since > CW_DECODER_TEXT_KEEP) {
    int over = (int)(h2 - since - CW_DECODER_TEXT_KEEP);
    if (over > n)
      over = n;
    memmove(buf, buf + over, (size_t)(n - over + 1));
  }
  return since + (uint32_t)n;
}

// ---------------------------------------------------------------- stage 3

// The best two-way split of n values (Otsu). Returns 0 for fewer than 2.
static int otsu(const float *v_in, int n, float *lo, float *hi) {
  if (n < 2)
    return 0;
  float v[GAP_HIST];
  memcpy(v, v_in, (size_t)n * sizeof(float));
  for (int i = 1; i < n; i++) {           // insertion sort; n is at most 64
    float x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = x;
  }
  float total = 0;
  for (int i = 0; i < n; i++)
    total += v[i];
  float best = -1, sum = 0;
  int best_k = 1;
  for (int k = 1; k < n; k++) {
    sum += v[k - 1];
    float m0 = sum / k, m1 = (total - sum) / (n - k);
    float var = (float)k * (float)(n - k) * (m1 - m0) * (m1 - m0);
    if (var > best) {
      best = var;
      best_k = k;
    }
  }
  float s0 = 0;
  for (int i = 0; i < best_k; i++)
    s0 += v[i];
  *lo = s0 / best_k;
  *hi = (total - s0) / (n - best_k);
  return 1;
}

static float mean(const float *v, int n) {
  float s = 0;
  for (int i = 0; i < n; i++)
    s += v[i];
  return n ? s / n : 0;
}

static int ready(void) { return d.n_marks >= MIN_MARKS && d.n_gaps >= MIN_GAPS; }

static void update_thresholds(void) {
  float lo, hi, intra;
  if (otsu(d.gaps, d.n_gaps, &lo, &hi)) {
    if (hi - lo < LOG_SEPARATED)          // no character gaps in the window yet
      hi = lo + LOG_3;
    d.t_char = lo + CHAR_FRAC * (hi - lo);
    float longer[GAP_HIST];
    int n_long = 0;
    for (int i = 0; i < d.n_gaps; i++)
      if (d.gaps[i] > d.t_char)
        longer[n_long++] = d.gaps[i];
    float l2, h2;
    if (n_long >= 2 && otsu(longer, n_long, &l2, &h2) && h2 - l2 >= LOG_WORD_SEPARATED)
      d.t_word = l2 + WORD_FRAC * (h2 - l2);
    else
      d.t_word = (n_long ? mean(longer, n_long) : hi) + LOG_WORD_OVER_CHAR;
    intra = lo;
  } else {
    d.t_char = logf(100.0f);
    d.t_word = logf(250.0f);
    intra = logf(60.0f);
  }
  float dot;
  if (otsu(d.marks, d.n_marks, &lo, &hi) && hi - lo >= LOG_SEPARATED) {
    d.t_mark = (lo + hi) / 2;
    dot = lo;
  } else {
    // one kind of mark: dots if about as long as the gaps inside characters
    float centre = d.n_marks ? mean(d.marks, d.n_marks) : intra;
    if (centre - intra < LOG_2) {
      d.t_mark = centre + LOG_1_7;
      dot = centre;
    } else {
      d.t_mark = centre - LOG_1_7;
      dot = centre - LOG_3;
    }
  }
  d.dot_ms = expf(dot);
  d.t_char_ms = (int)ceilf(expf(d.t_char));
  d.t_word_ms = (int)ceilf(expf(d.t_word));
  float p = PAUSE_OVER_WORD * expf(d.t_word);
  d.pause_ms = p > PAUSE_MS ? (int)p : PAUSE_MS;
  atomic_store(&wpm_now, ready() ? (int)lroundf(1200.0f / d.dot_ms) : 0);
}

static void finish_char(void) {
  if (d.code_len || d.code_overflow)
    put_char(d.code_overflow ? -1 : morse_decode(d.code));
  d.code_len = 0;
  d.code[0] = '\0';
  d.code_overflow = 0;
}

static void forget_timing(void) {
  d.n_marks = d.marks_pos = 0;
  d.n_gaps = d.gaps_pos = 0;
  d.n_pending = 0;
  update_thresholds();
}

// A mark or gap whose length is final, already in the history: its text.
static void classify(int mark, int ms) {
  float x = logf((float)ms);
  if (mark) {
    if (d.code_len < MAX_CODE) {
      d.code[d.code_len++] = x < d.t_mark ? '.' : '-';
      d.code[d.code_len] = '\0';
    } else {
      d.code_overflow = 1;
    }
    d.char_done = d.word_done = 0;
    return;
  }
  if (x >= d.t_char && !d.char_done) {
    finish_char();
    d.char_done = 1;
  }
  if (x >= d.t_word && !d.word_done) {
    put_space();
    d.word_done = 1;
  }
}

static void replay_pending(void) {
  for (int i = 0; i < d.n_pending; i++)
    classify(d.pending[i].mark, d.pending[i].ms);
  d.n_pending = 0;
}

static void commit(int mark, int ms) {
  TRACE("%c%d ", mark ? 'M' : 's', ms);
  if (!mark && d.paused) {          // the gap that ended a transmission
    d.paused = 0;
    return;
  }
  if (mark) {
    d.marks[d.marks_pos] = logf((float)ms);
    d.marks_pos = (d.marks_pos + 1) % MARK_HIST;
    if (d.n_marks < MARK_HIST)
      d.n_marks++;
  } else if (ms < PAUSE_MS) {
    d.gaps[d.gaps_pos] = logf((float)ms);
    d.gaps_pos = (d.gaps_pos + 1) % GAP_HIST;
    if (d.n_gaps < GAP_HIST)
      d.n_gaps++;
  }
  update_thresholds();
  if (!ready()) {
    if (d.n_pending < PENDING_MAX)
      d.pending[d.n_pending++] = (struct event){(char)mark, ms};
    if (d.n_pending == PENDING_MAX)  // not settling: decode what there is
      replay_pending();
    return;
  }
  replay_pending();
  classify(mark, ms);
}

static int blip_ms(void) {
  int b = (int)(BLIP_DOTS * d.dot_ms);
  return b > MIN_BLIP_MS ? b : MIN_BLIP_MS;
}

// One 1 ms tick of the mark/gap decision from stage 2.
static void event_tick(int m) {
  if (!d.seen_mark) {                // nothing before the first mark counts
    if (!m)
      return;
    d.seen_mark = 1;
    d.cur_mark = 1;
    d.cur_ms = 0;
  }
  if (m == d.cur_mark) {
    d.cur_ms++;
  } else if (d.last_valid) {
    // the event that just ended was a blip: it joins the one before, and
    // that one goes on
    d.cur_mark = d.last_mark;
    d.cur_ms = d.last_ms + d.cur_ms + 1;
    d.last_valid = 0;
  } else if (d.cur_mark && d.cur_ms < blip_ms() && d.n_marks == 0 && d.n_pending == 0) {
    // a blip before anything has been decoded: noise, not the first mark
    d.seen_mark = 0;
    d.cur_mark = 0;
    d.cur_ms = 0;
    return;
  } else {
    d.last_valid = 1;
    d.last_mark = d.cur_mark;
    d.last_ms = d.cur_ms;
    d.cur_mark = m;
    d.cur_ms = 1;
  }
  if (d.last_valid && d.cur_ms >= blip_ms()) {
    commit(d.last_mark, d.last_ms);
    d.last_valid = 0;
  }
  if (d.cur_mark || d.last_valid)
    return;
  // in a gap: the character, the word space and the pause, as they come
  if (ready() && d.n_pending == 0 && !d.paused) {
    if (d.cur_ms >= d.t_char_ms && !d.char_done) {
      finish_char();
      d.char_done = 1;
    }
    if (d.cur_ms >= d.t_word_ms && !d.word_done) {
      put_space();
      d.word_done = 1;
    }
  }
  if (!d.paused && (ready() ? d.cur_ms >= d.pause_ms : d.cur_ms >= 3 * PAUSE_MS && d.n_pending)) {
    replay_pending();
    finish_char();
    put_newline();
    forget_timing();
    d.char_done = d.word_done = 1;
    d.paused = 1;
  }
}

// ---------------------------------------------------------------- stage 2

static float tau_alpha(double tau_ms) { return (float)exp(-1.0 / tau_ms); }

static void detector_tick(float e) {
  if (!d.primed) {
    d.floor_ = d.noise = e;
    d.primed = 1;
  }
  if (d.warm_ms < WARMUP_MS) {
    // learn the noise floor quickly before deciding anything
    float a = tau_alpha(WARMUP_TAU_MS);
    d.floor_ = a * d.floor_ + (1 - a) * e;
    d.noise = d.floor_;
    d.peak = d.floor_;
    d.warm_ms++;
    event_tick(0);
    return;
  }
  if (e > d.peak)
    d.peak = e;
  else if (d.mark)
    d.peak = d.a_peak_mark * d.peak + (1 - d.a_peak_mark) * e;
  else
    d.peak = d.a_peak_gap * d.peak + (1 - d.a_peak_gap) * d.floor_;
  if (!d.mark) {
    float a = e < d.floor_ ? d.a_floor_down : d.a_floor_up;
    d.floor_ = a * d.floor_ + (1 - a) * e;
    d.noise = d.a_noise * d.noise + (1 - d.a_noise) * e;
  }
#ifdef CW_DECODER_TRACE
  static int tk;
  if (++tk % 1000 == 0)
    TRACE("[%.1f %.1f] ", d.peak / d.noise, d.peak / d.floor_);
#endif
  float span = d.peak - d.floor_;
  float mid = d.floor_ + 0.5f * span;
  float h = HYSTERESIS * span;
  int m = d.mark;
  if (d.peak < SQUELCH_RATIO * d.noise)
    m = 0;
  else if (d.mark && e < mid - h)
    m = 0;
  else if (!d.mark && e > mid + h)
    m = 1;
  d.mark = m;
  event_tick(m);
}

// ---------------------------------------------------------------- stage 1

static void biquad_lowpass(struct biquad *b, double fc, double fs, double q) {
  double k = tan(M_PI * fc / fs);
  double norm = 1.0 / (1.0 + k / q + k * k);
  b->b0 = (float)(k * k * norm);
  b->b1 = 2 * b->b0;
  b->b2 = b->b0;
  b->a1 = (float)(2.0 * (k * k - 1.0) * norm);
  b->a2 = (float)((1.0 - k / q + k * k) * norm);
  b->z1 = b->z2 = 0;
}

static inline float biquad_run(struct biquad *b, float x) {
  float y = b->b0 * x + b->z1;
  b->z1 = b->b1 * x - b->a1 * y + b->z2;
  b->z2 = b->b2 * x - b->a2 * y;
  return y;
}

static void set_pitch(int hz) {
  d.pitch_hz = hz;
  double w = 2.0 * M_PI * hz / FS;
  d.step_re = cos(w);
  d.step_im = -sin(w);           // e^{-jw}: mixes the tone down to 0 Hz
}

static void reset_all(void) {
  int pitch = d.pitch_hz;
  memset(&d, 0, sizeof(d));
  d.osc_re = 1.0;
  set_pitch(pitch ? pitch : 700);
  // Butterworth, 4th order: two sections with Q 0.5412 and 1.3066
  biquad_lowpass(&d.lp_i[0], LPF_HZ, FS_LO, 0.54119610);
  biquad_lowpass(&d.lp_i[1], LPF_HZ, FS_LO, 1.30656296);
  d.lp_q[0] = d.lp_i[0];
  d.lp_q[1] = d.lp_i[1];
  d.a_peak_mark = tau_alpha(PEAK_MARK_TAU_MS);
  d.a_peak_gap = tau_alpha(PEAK_GAP_TAU_MS);
  d.a_floor_down = tau_alpha(FLOOR_DOWN_TAU_MS);
  d.a_floor_up = tau_alpha(FLOOR_UP_TAU_MS);
  d.a_noise = tau_alpha(NOISE_TAU_MS);
  update_thresholds();
}

void cw_decoder_init(void) {
  reset_all();
  atomic_store(&wpm_now, 0);
}

void cw_decoder_process(const float *audio, int n, int pitch_hz) {
  if (atomic_exchange(&reset_request, 0)) {
    put_newline();
    reset_all();
  }
  if (!atomic_load_explicit(&enabled, memory_order_relaxed))
    return;
  if (pitch_hz != d.pitch_hz && pitch_hz > 0)
    set_pitch(pitch_hz);
  for (int k = 0; k < n; k++) {
    float a = audio[k];
    d.acc_i += a * (float)d.osc_re;
    d.acc_q += a * (float)d.osc_im;
    double re = d.osc_re * d.step_re - d.osc_im * d.step_im;
    d.osc_im = d.osc_re * d.step_im + d.osc_im * d.step_re;
    d.osc_re = re;
    if (++d.box_n < BOX)
      continue;
    float i = biquad_run(&d.lp_i[1], biquad_run(&d.lp_i[0], d.acc_i));
    float q = biquad_run(&d.lp_q[1], biquad_run(&d.lp_q[0], d.acc_q));
    d.acc_i = d.acc_q = 0;
    d.box_n = 0;
    if (++d.lo_n < TICK_LO)
      continue;
    d.lo_n = 0;
    detector_tick(sqrtf(i * i + q * q));
  }
  double r = 1.0 / sqrt(d.osc_re * d.osc_re + d.osc_im * d.osc_im);
  d.osc_re *= r;
  d.osc_im *= r;
}

void cw_decoder_set_enabled(int on) {
  int was = atomic_exchange(&enabled, on ? 1 : 0);
  if (on && !was)
    atomic_store(&reset_request, 1);
}

int cw_decoder_get_enabled(void) { return atomic_load(&enabled); }

void cw_decoder_reset(void) { atomic_store(&reset_request, 1); }

int cw_decoder_get_wpm(void) { return atomic_load(&wpm_now); }
