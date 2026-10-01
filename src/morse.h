// morse.h
//
// The Morse table, and the translation of each control surface's text into
// the one internal form the keyer sends. Internal form: the table's
// characters (A-Z, 0-9 and . , ? ' ! / ( ) : ; - _ " @ $ + = &), ' ' between
// words, the prosign codes below, and the speed codes - so no protocol's
// punctuation ever reaches the table, and '<' can mean AS on one surface and
// a speed change on another. Knows nothing of timing. docs/dsp_design_notes/cw_keyer_design_study.md §9.

#ifndef MORSE_H
#define MORSE_H

// Prosigns, sent as one character with no letter space inside.
enum morse_prosign {
  MORSE_AR = 0x80, // .-.-.   end of message
  MORSE_AS,        // .-...   wait
  MORSE_BK,        // -...-.- break
  MORSE_BT,        // -...-   separator
  MORSE_HH,        // ........ error
  MORSE_KN,        // -.--.   go ahead, named station only
  MORSE_SK,        // ...-.-  end of contact
  MORSE_SN,        // ...-.   understood
  MORSE_PROSIGN_END,
};

// Speed changes inside text (TCI's < and >): the text after one is sent
// MORSE_SPEED_STEP WPM slower or faster than before it, until MORSE_SPEED_BASE
// or the end of the text returns it to the keyer's speed. Each takes no time.
enum morse_speed {
  MORSE_SPEED_DOWN = 0x90,
  MORSE_SPEED_UP,
  MORSE_SPEED_BASE,
};
#define MORSE_SPEED_STEP 5

// The dots and dashes for an internal-form character ("-.-." for C), or
// NULL for a space or anything the table lacks.
const char *morse_pattern(unsigned char c);

// "AR", "SK", ... for a prosign code; NULL otherwise.
const char *morse_prosign_name(unsigned char c);

// Translate a surface's text into internal form in out (NUL-terminated, at
// most max - 1 characters). Letters are taken in either case; tabs and line
// ends become spaces. A character the table lacks is left out and appended
// to skipped (NUL-terminated, at most skip_max - 1), for the caller to
// report. Returns the length written to out.
//
// Plain (rigctld, the control panel): prosigns as <AR>, <SK>, <BT>, <KN>,
// <AS>, <BK>, <HH>, <SN>, in either case.
int morse_from_plain(const char *in, char *out, int max, char *skipped, int skip_max);

// Kenwood (CAT KY): [ BT, _ AR, < AS, # HH, > SK, ] KN, \ BK, % SN, as on a
// TS-480; everything else as plain.
int morse_from_kenwood(const char *in, char *out, int max, char *skipped, int skip_max);

// TCI (cw_macros, cw_msg): prosigns between bars, |AR| |SK| ... in either
// case; < and > lower and raise the speed by MORSE_SPEED_STEP; ^ ~ * stand for
// the protocol's reserved : , ;. Letters between bars that aren't a prosign
// are sent as ordinary letters. Everything else as plain, without its <XX>.
int morse_from_tci(const char *in, char *out, int max, char *skipped, int skip_max);

#endif /* MORSE_H */
