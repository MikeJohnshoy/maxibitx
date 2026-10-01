// tci_cw.c
//
// CW over TCI, sent by the keyer (keyer.h) as rigctld's b and CAT's KY text
// is, in CW and CWR only:
//
//   cw_macros:0,<text>;   text queued in order; |SK| prosigns, < and > speed
//                         changes (5 WPM a step, back to the keyer's speed
//                         after each macro), ^ ~ * for : , ; (morse_from_tci())
//   cw_msg:0,<prefix>,<callsign>,<suffix>;   a message whose callsign can be
//                         corrected (cw_msg:<callsign>;) until it is sent;
//                         <callsign>$N sends it N times, and _ is an empty
//                         prefix or suffix. callsign_send:<callsign>; tells
//                         every client when the last of it has started.
//   cw_macros_stop;       stops the text: the element in progress completes
//   cw_terminal:true|false;   while true, TX stays on after text ends, and
//                         cw_macros_empty; is sent as the last character
//                         queued starts
//
// The callsign is fed to the keyer one character at a time, each once the
// one before has started, so a correction applies to everything not yet
// started but one. Text the keyer's queue has no room for waits here, in
// order, until it has. A new cw_msg stops whatever text is being sent; a
// cw_macros during a cw_msg waits for it to finish. A paddle or key closing
// stops all of it, as it stops any text, and ends terminal mode's TX hold. cw_macros_delay reads 0 (TX starts
// as soon as the T/R sequence allows), and keyer - a client keying single
// elements, with the network's timing - is ignored.
// docs/dsp_design_notes/tci_design_study.md §8.

#include "tci_cw.h"
#include "cw.h"
#include "keyer.h"
#include "morse.h"
#include "radio.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CW_TEXT_MAX KEYER_TEXT_QUEUE_LEN
#define CALLSIGN_MAX 32
#define CALLSIGN_REPEAT_MAX 5
#define DEFERRED_MAX (4 * KEYER_TEXT_QUEUE_LEN)

static int terminal;            // cw_terminal
static int terminal_slot = -1;  // the client that set it
static int empty_due;           // text queued since cw_macros_empty was last due
static unsigned interrupts_seen; // keyer_text_interrupts() when text was last queued

static const char speed_base[] = {(char)MORSE_SPEED_BASE, '\0'};

static struct {
  int active;
  int slot;                // the client that sent it
  char base[CALLSIGN_MAX]; // the callsign, as last corrected
  int repeat;
  int fed;                 // characters of the callsign text given to the keyer
  int prefixed;            // the prefix sent something
  int speed_codes;         // the prefix or suffix changes speed
  char suffix[CW_TEXT_MAX]; // internal form, queued once the callsign is out
} msg;

static char waiting[CW_TEXT_MAX]; // cw_macros that arrived during a cw_msg
static int waiting_len;
static char deferred[DEFERRED_MAX]; // queued text the keyer has no room for yet
static int deferred_len;

static int in_cw(void) {
  enum radio_mode m = radio_get_mode();
  return m == RADIO_MODE_CW || m == RADIO_MODE_CWR;
}

static int has_speed_codes(const char *internal) {
  for (const unsigned char *p = (const unsigned char *)internal; *p; p++)
    if (*p >= MORSE_SPEED_DOWN && *p <= MORSE_SPEED_BASE)
      return 1;
  return 0;
}

// TCI text to internal form, reporting what the table lacks.
static int translate(int slot, const char *text, char *out, int max) {
  char skipped[32];
  int n = morse_from_tci(text, out, max, skipped, sizeof(skipped));
  if (skipped[0])
    printf("tci: client %d CW text: skipped, not in the Morse table: %s\n", slot, skipped);
  return n;
}

// Queues internal-form text for the keyer - or here, after anything already
// waiting, if the keyer's queue lacks room - and asks for TX. Returns 0, or
// -1 if it was refused.
static int queue_text(int slot, const char *internal) {
  unsigned seen = keyer_text_interrupts(); // before: a closure from here on counts
  int n = (int)strlen(internal);
  int r = deferred_len ? -1 : keyer_send_text(internal);
  if (r == -2) {
    printf("tci: client %d CW text refused - this keyer doesn't send text\n", slot);
    return -1;
  }
  if (r == -1) {
    if (deferred_len + n >= DEFERRED_MAX) {
      printf("tci: client %d CW text refused - too much text waiting to be sent\n", slot);
      return -1;
    }
    memcpy(deferred + deferred_len, internal, (size_t)n + 1);
    deferred_len += n;
  }
  cw_text_queued();
  empty_due = 1;
  interrupts_seen = seen;
  if (terminal)
    cw_hold_tx(1);
  return 0;
}

// Moves what has waited for room into the keyer's queue, as much as fits.
static void flush_deferred(void) {
  int n = keyer_text_room();
  if (!deferred_len || n <= 0)
    return;
  if (n > deferred_len)
    n = deferred_len;
  char chunk[KEYER_TEXT_QUEUE_LEN + 1];
  memcpy(chunk, deferred, (size_t)n);
  chunk[n] = '\0';
  if (keyer_send_text(chunk) == 0) {
    memmove(deferred, deferred + n, (size_t)(deferred_len - n) + 1);
    deferred_len -= n;
  }
}

static void drop_all(void) {
  msg.active = 0;
  waiting_len = 0;
  deferred_len = 0;
}

// The callsign text: the callsign, repeated with a space between.
static void callsign_text(char *out, int max) {
  char plain[(CALLSIGN_MAX + 1) * CALLSIGN_REPEAT_MAX + 1] = "";
  for (int k = 0; k < msg.repeat; k++) {
    if (k)
      strcat(plain, " ");
    strcat(plain, msg.base);
  }
  morse_from_tci(plain, out, max, NULL, 0);
}

// "RA6LH$2" -> base "RA6LH", repeat 2 (1-5). Spaces around it are dropped.
static void set_callsign(const char *s) {
  while (isspace((unsigned char)*s))
    s++;
  size_t n = strcspn(s, "$");
  while (n && isspace((unsigned char)s[n - 1]))
    n--;
  if (n >= CALLSIGN_MAX)
    n = CALLSIGN_MAX - 1;
  memcpy(msg.base, s, n);
  msg.base[n] = '\0';
  const char *dollar = strchr(s, '$');
  if (dollar) {
    long r = strtol(dollar + 1, NULL, 10);
    msg.repeat = r < 1 ? 1 : r > CALLSIGN_REPEAT_MAX ? CALLSIGN_REPEAT_MAX : (int)r;
  }
}

// Copies one comma-separated field of s into out, without the spaces around
// it, and returns where the next starts (NULL after the last). "_" alone is
// an empty field.
static const char *field(const char *s, char *out, size_t max, int rest) {
  const char *end = rest ? s + strlen(s) : s + strcspn(s, ",");
  while (s < end && isspace((unsigned char)*s))
    s++;
  const char *e = end;
  while (e > s && isspace((unsigned char)e[-1]))
    e--;
  size_t n = (size_t)(e - s) < max - 1 ? (size_t)(e - s) : max - 1;
  memcpy(out, s, n);
  out[n] = '\0';
  if (!strcmp(out, "_"))
    out[0] = '\0';
  return *end == ',' ? end + 1 : NULL;
}

static int is_trx0(const char *s, size_t n) {
  while (n && isspace((unsigned char)*s)) {
    s++;
    n--;
  }
  return n >= 1 && s[0] == '0' && (n == 1 || isspace((unsigned char)s[1]));
}

static void cmd_macros(int slot, const char *args) {
  const char *comma = args ? strchr(args, ',') : NULL;
  if (!comma || !is_trx0(args, (size_t)(comma - args)))
    return;
  if (!in_cw()) {
    printf("tci: client %d cw_macros ignored - text is sent in CW or CWR only\n", slot);
    return;
  }
  char internal[CW_TEXT_MAX];
  int n = translate(slot, comma + 1, internal, sizeof(internal) - 1);
  if (n == 0)
    return; // nothing to send: no TX for it
  if (has_speed_codes(internal)) { // the next macro starts at the keyer's speed
    internal[n++] = (char)MORSE_SPEED_BASE;
    internal[n] = '\0';
  }
  if (msg.active) {
    if (waiting_len + n < CW_TEXT_MAX) {
      memcpy(waiting + waiting_len, internal, (size_t)n + 1);
      waiting_len += n;
    } else {
      printf("tci: client %d cw_macros refused - too much waiting behind cw_msg\n", slot);
    }
    return;
  }
  if (queue_text(slot, internal) == 0)
    printf("tci: client %d cw_macros: %s\n", slot, comma + 1);
}

static void cmd_msg(int slot, const char *args) {
  if (!args)
    return;
  if (!strchr(args, ',')) { // cw_msg:<callsign>; - a correction
    if (msg.active) {
      set_callsign(args);
      printf("tci: client %d cw_msg callsign corrected to %s\n", slot, msg.base);
    }
    return;
  }
  char trx[8], prefix[CW_TEXT_MAX], call[CALLSIGN_MAX * 2], suffix[CW_TEXT_MAX];
  const char *p = field(args, trx, sizeof(trx), 0);
  if (!p || strcmp(trx, "0"))
    return;
  p = field(p, prefix, sizeof(prefix), 0);
  if (!p)
    return;
  p = field(p, call, sizeof(call), 0);
  suffix[0] = '\0';
  if (p) // a missing suffix is an empty one
    field(p, suffix, sizeof(suffix), 1);
  if (!in_cw()) {
    printf("tci: client %d cw_msg ignored - text is sent in CW or CWR only\n", slot);
    return;
  }

  // A message replaces whatever text is being sent.
  if (keyer_text_busy())
    keyer_stop_text();
  drop_all();

  char internal[CW_TEXT_MAX];
  int n = translate(slot, prefix, internal, sizeof(internal) - 1);
  if (n && call[0]) { // the word space before the callsign
    internal[n++] = ' ';
    internal[n] = '\0';
  }
  msg.speed_codes = has_speed_codes(internal);
  msg.prefixed = n > 0;
  if (queue_text(slot, internal) != 0)
    return;

  msg.repeat = 1;
  set_callsign(call);
  msg.fed = 0;
  translate(slot, suffix, msg.suffix, sizeof(msg.suffix) - 2); // room for a space and a code
  msg.speed_codes |= has_speed_codes(msg.suffix);
  msg.slot = slot;
  msg.active = 1;
  printf("tci: client %d cw_msg: %s / %s x%d / %s\n", slot, prefix, msg.base, msg.repeat, suffix);
}

static void cmd_terminal(int slot, const char *args) {
  if (!args)
    return;
  const char *v = strrchr(args, ',');
  v = v ? v + 1 : args;
  while (isspace((unsigned char)*v))
    v++;
  int on;
  if (!strncasecmp(v, "true", 4))
    on = 1;
  else if (!strncasecmp(v, "false", 5))
    on = 0;
  else
    return;
  terminal = on;
  terminal_slot = slot;
  if (on && keyer_text_busy())
    cw_hold_tx(1);
  else if (!on)
    cw_hold_tx(0);
  tci_broadcast("cw_terminal:%s;", on ? "true" : "false");
}

int tci_cw_command(int slot, const char *name, const char *args) {
  if (!strcmp(name, "cw_macros")) {
    cmd_macros(slot, args);
  } else if (!strcmp(name, "cw_msg")) {
    cmd_msg(slot, args);
  } else if (!strcmp(name, "cw_macros_stop")) {
    keyer_stop_text();
    drop_all();
    empty_due = 0;
    cw_hold_tx(0);
    printf("tci: client %d cw_macros_stop - text stopped\n", slot);
  } else if (!strcmp(name, "cw_terminal")) {
    cmd_terminal(slot, args);
  } else if (!strcmp(name, "cw_macros_delay")) {
    tci_send_to(slot, "cw_macros_delay:0;");
  } else if (!strcmp(name, "keyer")) {
    // single elements keyed over the network: not offered
  } else {
    return 0;
  }
  return 1;
}

void tci_cw_service(void) {
  if (!in_cw()) { // cw.c stops the text itself outside CW
    drop_all();
    empty_due = 0;
    return;
  }
  if ((msg.active || empty_due || deferred_len) && keyer_text_interrupts() != interrupts_seen) {
    drop_all(); // the operator took over with the key
    empty_due = 0;
    if (terminal)
      cw_hold_tx(0);
    printf("tci: CW text stopped by the key\n");
    return;
  }
  flush_deferred();
  if (msg.active && !deferred_len && keyer_text_queued() == 0) {
    char call[(CALLSIGN_MAX + 1) * CALLSIGN_REPEAT_MAX + 1];
    callsign_text(call, sizeof(call));
    if (msg.fed < (int)strlen(call)) {
      char one[2] = {call[msg.fed++], '\0'};
      keyer_send_text(one);
    } else {
      msg.active = 0;
      if (msg.base[0])
        tci_broadcast("callsign_send:%s;", msg.base);
      char tail[CW_TEXT_MAX + 2];
      snprintf(tail, sizeof(tail), "%s%s%s",
               msg.suffix[0] && (msg.prefixed || msg.base[0]) ? " " : "", msg.suffix,
               msg.speed_codes ? speed_base : "");
      queue_text(msg.slot, tail);
      if (waiting_len) {
        queue_text(msg.slot, waiting);
        waiting_len = 0;
      }
    }
  }
  if (empty_due && !msg.active && !deferred_len && keyer_text_letters_queued() == 0) {
    empty_due = 0;
    if (terminal)
      tci_broadcast("cw_macros_empty;");
  }
}

void tci_cw_client_closed(int slot) {
  if (terminal && terminal_slot == slot) {
    terminal = 0;
    terminal_slot = -1;
    cw_hold_tx(0);
  }
}
