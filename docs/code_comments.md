# Code comments vs. docs

Status: adopted 2026-09-21. First applied to `src/rx_audio.c`: 477
comment lines cut to 148, with byte-identical machine code.

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
