// keyer_straight.c
//
// keyer.h with the straight key alone: the key follows either contact of the
// jack, and every other mode, and text, is refused. Built in place of
// keyer.c by `make KEYER=keyer_straight`, which is how the paddle keyer is
// removed, and what a replacement keyer has to match. Speed is stored and reported but
// times nothing.

#include "keyer.h"
#include <stdatomic.h>

static _Atomic int wpm = KEYER_WPM_DEFAULT;
static unsigned contacts; // bit per enum key_contact, closed - audio thread only

int keyer_run_block(const struct key_event *ev, int n_ev, uint8_t *key, int n) {
  int any = 0, e = 0;
  for (int pos = 0; pos < n; pos++) {
    while (e < n_ev && ev[e].offset <= pos) {
      unsigned bit = 1u << ev[e].contact;
      contacts = ev[e].closed ? (contacts | bit) : (contacts & ~bit);
      e++;
    }
    key[pos] = contacts != 0;
    any |= key[pos];
  }
  for (; e < n_ev; e++) {
    unsigned bit = 1u << ev[e].contact;
    contacts = ev[e].closed ? (contacts | bit) : (contacts & ~bit);
  }
  return any;
}

void keyer_reset(void) { contacts = 0; }

int keyer_set_mode(enum keyer_mode m) { return m == KEYER_STRAIGHT ? 0 : -1; }

enum keyer_mode keyer_get_mode(void) { return KEYER_STRAIGHT; }

int keyer_set_wpm(int w) {
  if (w < KEYER_WPM_MIN)
    w = KEYER_WPM_MIN;
  if (w > KEYER_WPM_MAX)
    w = KEYER_WPM_MAX;
  atomic_store(&wpm, w);
  return w;
}

int keyer_get_wpm(void) { return atomic_load(&wpm); }

int keyer_send_text(const char *text) {
  (void)text;
  return -2;
}

void keyer_stop_text(void) {}

int keyer_text_busy(void) { return 0; }

int keyer_text_room(void) { return 0; }

const char *keyer_mode_name(enum keyer_mode m) {
  static const char *const names[KEYER_MODES] = {"straight", "bug", "ultimatic", "iambic A",
                                                 "iambic B"};
  return ((int)m >= 0 && m < KEYER_MODES) ? names[m] : "?";
}
