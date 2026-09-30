// morse.c
//
// The Morse table and the surfaces' text translators (morse.h). The table
// is the reference keyer's (sbitx dev-54bugfixes, modem_cw.c), with its
// punctuation-as-prosign entries replaced by the internal prosign codes.

#include "morse.h"
#include <ctype.h>
#include <stddef.h>
#include <string.h>

static const char *const letters[26] = {
    ".-",   "-...", "-.-.", "-..",  ".",   "..-.", "--.",  "....", "..",
    ".---", "-.-",  ".-..", "--",   "-.",  "---",  ".--.", "--.-", ".-.",
    "...",  "-",    "..-",  "...-", ".--", "-..-", "-.--", "--..",
};

static const char *const digits[10] = {
    "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----.",
};

static const struct {
  char c;
  const char *pattern;
} punctuation[] = {
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'\'', ".----."}, {'!', "-.-.--"},
    {'/', "-..-."},  {'(', "-.--."},  {')', "-.--.-"}, {':', "---..."},  {';', "-.-.-."},
    {'-', "-....-"}, {'_', "..--.-"}, {'"', ".-..-."}, {'@', ".--.-."},  {'$', "...-..-"},
    {'+', ".-.-."},  {'=', "-...-"},  {'&', ".-..."},
};

static const struct {
  const char *name;
  const char *pattern;
} prosigns[MORSE_PROSIGN_END - MORSE_AR] = {
    {"AR", ".-.-."}, {"AS", ".-..."},    {"BK", "-...-.-"}, {"BT", "-...-"},
    {"HH", "........"}, {"KN", "-.--."}, {"SK", "...-.-"},  {"SN", "...-."},
};

const char *morse_pattern(unsigned char c) {
  if (c >= 'A' && c <= 'Z')
    return letters[c - 'A'];
  if (c >= '0' && c <= '9')
    return digits[c - '0'];
  if (c >= MORSE_AR && c < MORSE_PROSIGN_END)
    return prosigns[c - MORSE_AR].pattern;
  for (size_t i = 0; i < sizeof(punctuation) / sizeof(punctuation[0]); i++)
    if (punctuation[i].c == (char)c)
      return punctuation[i].pattern;
  return NULL;
}

const char *morse_prosign_name(unsigned char c) {
  if (c >= MORSE_AR && c < MORSE_PROSIGN_END)
    return prosigns[c - MORSE_AR].name;
  return NULL;
}

static void note_skipped(char *skipped, int skip_max, char c) {
  if (!skipped || skip_max <= 0)
    return;
  size_t n = strlen(skipped);
  if ((int)n < skip_max - 1) {
    skipped[n] = c;
    skipped[n + 1] = '\0';
  }
}

// One character of a surface's text, already known not to be a prosign:
// a space, a table character, or skipped.
static void put_plain(char c, char *out, int max, int *n, char *skipped, int skip_max) {
  if (c == '\t' || c == '\r' || c == '\n')
    c = ' ';
  if (c == ' ') {
    if (*n < max - 1)
      out[(*n)++] = ' ';
    return;
  }
  char u = (char)toupper((unsigned char)c);
  if (morse_pattern((unsigned char)u)) {
    if (*n < max - 1)
      out[(*n)++] = u;
  } else {
    note_skipped(skipped, skip_max, c);
  }
}

int morse_from_plain(const char *in, char *out, int max, char *skipped, int skip_max) {
  int n = 0;
  if (skipped && skip_max > 0)
    skipped[0] = '\0';
  for (const char *p = in; *p; p++) {
    if (*p == '<' && p[1] && p[2] && p[3] == '>') {
      char name[3] = {(char)toupper((unsigned char)p[1]), (char)toupper((unsigned char)p[2]), 0};
      int code = -1;
      for (int i = 0; i < MORSE_PROSIGN_END - MORSE_AR; i++)
        if (strcmp(prosigns[i].name, name) == 0)
          code = MORSE_AR + i;
      if (code >= 0) {
        if (n < max - 1)
          out[n++] = (char)code;
        p += 3;
        continue;
      }
    }
    put_plain(*p, out, max, &n, skipped, skip_max);
  }
  if (max > 0)
    out[n < max ? n : max - 1] = '\0';
  return n;
}

int morse_from_kenwood(const char *in, char *out, int max, char *skipped, int skip_max) {
  static const struct {
    char c;
    unsigned char code;
  } kenwood[] = {
      {'[', MORSE_BT}, {'_', MORSE_AR}, {'<', MORSE_AS}, {'#', MORSE_HH},
      {'>', MORSE_SK}, {']', MORSE_KN}, {'\\', MORSE_BK}, {'%', MORSE_SN},
  };
  int n = 0;
  if (skipped && skip_max > 0)
    skipped[0] = '\0';
  for (const char *p = in; *p; p++) {
    int done = 0;
    for (size_t i = 0; i < sizeof(kenwood) / sizeof(kenwood[0]); i++)
      if (*p == kenwood[i].c) {
        if (n < max - 1)
          out[n++] = (char)kenwood[i].code;
        done = 1;
        break;
      }
    if (!done)
      put_plain(*p, out, max, &n, skipped, skip_max);
  }
  if (max > 0)
    out[n < max ? n : max - 1] = '\0';
  return n;
}
