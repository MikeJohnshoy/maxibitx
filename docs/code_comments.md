# Code comments vs. docs

Status: adopted 2026-09-21. Applied so far, each with byte-identical
machine code across the whole build:

| file | comment lines before -> after |
|---|---|
| `src/rx_audio.c` | 477 -> 148 |
| `src/sound.c` | 504 -> 230 |
| `src/sound.h` | 45 -> 32 |
| `src/radio.c` | 125 -> 57 |
| `src/radio.h` | 88 -> 42 |
| `src/interfaces/usb_gadget.c` | 572 -> 373 |
| `src/interfaces/usb_gadget.h` | 203 -> 73 |
| `src/interfaces/hamlib.c` | 142 -> 61 |
| `src/interfaces/hamlib.h` | 20 -> 21 |
| `src/cw.c` | 81 -> 33 |
| `src/cw.h` | 50 -> 21 |
| `src/tx_pipeline.c` | 161 -> 48 |
| `src/tx_pipeline.h` | 138 -> 59 |

The second pass, 31 October 2026, checked the comments against the code
as well as against the policy. Every object's code and data, and every
file's comment-free token stream, compared identical before and after.
Counts here are lines carrying a comment, so they don't compare with the
table above:

| file | comment lines before -> after |
|---|---|
| `src/rx_audio.h` | 263 -> 221 |
| `src/rx_filter.h` | 142 -> 90 |
| `src/rx_filter.c` | 43 -> 26 |
| `src/fft_filter.h` | 137 -> 113 |
| `src/fft_filter.c` | 159 -> 133 |
| `src/interfaces/hpsdr_p1.c` | 176 -> 170 |
| `src/interfaces/hpsdr_p1.h` | 1 -> 2 |
| `src/interfaces/iq_stream.c` | 95 -> 91 |
| `src/interfaces/iq_stream.h` | 34 -> 30 |
| `src/upsample48k.h` | 51 -> 35 |
| `src/upsample48k.c` | 25 -> 25 |
| `src/decim48k.h` | 21 -> 22 |
| `src/decim48k.c` | 22 -> 17 |
| `src/antialias.h` | 18 -> 19 |
| `src/gpio.h` | 58 -> 48 |
| `src/gpio.c` | 12 -> 11 |
| `src/i2c.h` | 11 -> 15 |
| `src/i2c.c` | 42 -> 34 |
| `src/si5351v2.c` | 62 -> 56 |
| `src/vfo.h` | 1 -> 2 |
| `src/vfo.c` | 23 -> 22 |
| `src/hw_settings.h` | 107 -> 100 |
| `src/hw_settings.c` | 46 -> 45 |
| `src/maxibitx.c` | 117 -> 116 |

Read line by line and found current: `antialias.c`, `si5351.h`,
`radio_hw.c`, `radio_hw.h`. The files written after the policy was
adopted were checked more lightly - their headers, every function a
comment names, and `make check-comments-all` - and needed one-line fixes
only (`tci.c`, `tone_gen.h`, and `rx_audio.c` from the first table):
`tci.c/.h`, `tci_stream.c/.h`, `tci_ws.c/.h`, `tci_cw.c/.h`,
`keyer.c/.h`, `keyer_straight.c`, `key_input.c/.h`, `morse.c/.h`,
`cw_decoder.c/.h`, `cw_spot.c/.h`, `encoder.c/.h`, `knobs.c/.h`,
`codec_check.c/.h`, `capture_dump.c/.h`, `tone_gen.c/.h`, and the
generated `narrow_filter_bank.h`. Not yet reviewed: the bench tests
(`src/*_test.c`) and the Makefile, whose comments still carry build
history.

This project records its engineering history carefully, and that record
is worth keeping. But source comments are read by someone trying to
change the code *as it is now*, and a paragraph about a design that no
longer exists is in their way. So the rule is: **the code says what is
true now, and the docs say how it got that way.**

## What stays in a comment

- **What the code does, when that isn't obvious from reading it.** The
  signal chain in a file header. The math a loop implements.
- **Units, ranges and invariants.** "dBFS, relative only." "Stored as
  integer percent so readback is exact." "Must be exactly
  `RX_FILTER_BLOCK_LEN` samples."
- **Where a constant came from.** The exact `scipy` call that produced
  a coefficient table, or the measurement that set a value.
- **Coupling.** "`usb_gadget.c`'s `UAC_RX_AUDIO_SCALE` is defined
  relative to this - change them together."
- **Guardrails against a known-wrong change, in a sentence or two plus
  a pointer.** "The AGC measures the raw input, because measuring after
  stage 3 makes the gain undo its selectivity (design note §8.7-8.8)."
  This is the one kind of history that earns space in the code: it
  stops someone repeating a mistake, and it still describes the current
  design.
- **Known open caveats about the current code.** "Placeholder, not a
  calibration."

## What moves to the docs

- Earlier versions and attempts ("v1 did X, attempt 2 moved it to Y").
- Dates, on-air reports and who noticed what.
- Old values ("was 0.03", "used to be unity gain").
- Removed functions ("`rx_audio_set_filter_bw()` existed for one
  revision").
- Long derivations and measurement tables. Keep a one-line summary and
  a pointer to the section.

Point comments at a **doc section**, like
`rx_audio_demod_design.md §8.2`. Never have one comment point at
another comment's title in a different file: titles get cut, and doc
section numbers are stable.

## Process for trimming a file

1. **Read the whole file and list every piece of history in it.**
2. **Check each one already exists in the docs.** Grep the design notes
   for its numbers and key terms. Move anything missing *before*
   cutting it from the code.
3. **Rewrite the comments only.** Correct anything the comment no
   longer gets right while you're there. `rx_audio.c`'s stage 1 comment
   still claimed "passes content above dial center", which the I/Q
   inversion finding made false.
4. **Prove nothing but comments changed.** Build before and after with
   the real compiler flags and compare `objdump -d` plus `.rodata`/
   `.data`. As a second check, compare the comment-stripped token
   stream (`gcc -fpreprocessed -dD -E -P`, whitespace removed). All
   must be identical.
5. **Grep the tree for references to comment titles that no longer
   exist** (`see rx_audio.c's "Why ..."`), and repoint them at doc
   sections.
6. Rebuild and run the tests.
7. Commit on its own, separate from any functional change, so the diff
   is reviewable as "comments only".

## Keeping it that way

New code brings new comments, and most of them are written while a fix
is fresh - exactly when history creeps in. Two things keep the policy
from eroding:

- **`CLAUDE.md`** at the repo root states the rule and where each kind
  of change is documented. AI assistants working in the repo read it
  automatically, and it's a one-page summary for anyone else.
- **`make check-comments`** (`tools/check_comments.py`) lists
  history-style comment lines - "used to", "previously", "now uses",
  dates, "bug", bench/on-air reports, build-order steps without a doc
  pointer - and comment blocks of 30+ lines, in files changed vs
  `origin/main`. It's advisory: every hit is a suspect to read, and
  lines that point at a doc section aren't flagged. `make
  check-comments-all` runs it over all of `src/`, for the periodic
  sweep. It also checks, over every file, that a source file's header
  names that file, and fails if one names a different file - the sign
  of a file uploaded under the wrong name, which has happened more
  than once.
