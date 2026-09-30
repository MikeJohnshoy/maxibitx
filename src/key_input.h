// key_input.h
//
// The key jack's two contacts - tip on BCM 5, ring on BCM 4 - read as
// kernel-timestamped edge events by a thread of their own that sleeps until
// an edge arrives. It debounces them, spots a mono straight-key plug
// grounding the ring, names the paddles (tip = dot unless reversed), asks
// for TX the moment a key closes, and hands the audio thread every edge
// with its time, which the audio thread turns into a sample position.
// Knows nothing of Morse or of radio modes.
// docs/dsp_design_notes/cw_keyer_design_study.md §6 and §16.

#ifndef KEY_INPUT_H
#define KEY_INPUT_H

#include <stdint.h>

enum key_contact { KEY_TIP = 0, KEY_RING = 1, KEY_CONTACTS = 2 };
enum key_paddle { KEY_DOT = 0, KEY_DASH = 1 };

// One edge as the audio thread receives it: a contact of the jack opening
// or closing, placed at a sample within the block being generated.
struct key_event {
  int offset;         // sample index in the block, 0..n-1
  uint8_t contact;    // enum key_contact - the physical contact
  uint8_t paddle;     // enum key_paddle - the contact's role for a paddle keyer
  uint8_t closed;     // 1 = closed (key down), 0 = open
};

// Called from the input thread each time a contact closes (after debounce,
// and not for a contact excluded as a grounded ring). cw.c's
// cw_key_closed() requests TX from it in CW and CWR.
typedef void (*key_closed_fn)(void);

// Claims both contacts, reads them, starts mono-plug detection and starts
// the input thread. debounce_ms is hw_settings.ini's key_debounce_ms.
// Call once, before the audio thread starts. Returns 0, or -1 if the lines
// can't be claimed (already logged).
int key_input_start(int debounce_ms, key_closed_fn on_closed);

// Audio thread, once per block: takes every edge stamped up to block_end_ns
// and places it in an n-sample block that stands for the interval
// block_start_ns..block_end_ns, i.e. the edge's position within that
// interval scaled to the block. So a block replays the interval before it,
// one block late, sample for sample. An edge stamped before block_start_ns
// (one the audio thread was too late for) goes at offset 0; one stamped
// after block_end_ns stays queued for the next block. Returns the number
// written to ev[], at most max, in order and with non-decreasing offsets.
// Never blocks.
int key_input_take(int64_t block_start_ns, int64_t block_end_ns, int n,
                   struct key_event *ev, int max);

// Nonzero while edges are queued that key_input_take() hasn't taken - an
// edge is on its way, so the key isn't idle even if every contact read
// open in the last block. Never blocks.
int key_input_pending(void);

// Paddle reversal: 0 (default) tip = dot, ring = dash; 1 swaps them. Only
// a paddle keyer uses the roles - a straight key keys from either contact.
void key_input_set_reverse(int on);
int key_input_get_reverse(void);

// ---- The input thread's steps, exposed for the bench harness --------------
//
// The thread calls these; key_input_test.c calls them directly with
// synthetic edges and times, no GPIO. All times are CLOCK_MONOTONIC ns.
// They are not thread-safe against each other - one caller at a time.

// Resets both contacts to the given levels (bit KEY_TIP / KEY_RING set =
// closed) and starts mono-plug detection from them. debounce_ms as above.
void key_input_arm(unsigned closed_mask, int64_t now_ns, int debounce_ms,
                   key_closed_fn on_closed);

// One kernel edge event: the contact reads closed (1) or open (0) after it.
void key_input_edge(enum key_contact c, int closed, int64_t ts_ns);

// Time has reached now_ns with no edge: ends any debounce lockout and
// detection window due by then.
void key_input_tick(int64_t now_ns);

// The earliest time key_input_tick() has work to do, or 0 if nothing is due.
int64_t key_input_next_due(void);

// Which contacts are excluded as a grounded ring (bitmask of KEY_TIP /
// KEY_RING), provisionally or confirmed.
unsigned key_input_excluded(void);

#endif /* KEY_INPUT_H */
