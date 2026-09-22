# Working on maxibitx

maxibitx is a headless radio daemon for the Raspberry Pi inside an
sBitx: hardware control, the RX/TX signal chains, and external
interfaces, with no UI in the process. Start with `README.md`;
`docs/00_intro.md` maps the rest of the docs.

## Comments: the code says what's true now, the docs say how it got there

Read `docs/code_comments.md` before editing comments. The short form:

- **A comment describes the current code**: what it does when that isn't
  obvious, units and ranges, invariants, where a constant came from,
  coupling to other code, known open caveats, and one- or two-line
  guardrails against a known-wrong change.
- **History goes in the docs**, not the code: earlier versions, bugs
  and how they were found, on-air and bench reports, dates, old values,
  removed functions, long derivations and measurement tables.
- **Point at doc sections** (`ARCHITECTURE.md §10 step 8`,
  `rx_audio_demod_design.md §8.2`), never at another comment's title.
- **Don't narrate the change you're making.** "Now uses X", "was changed
  to", "fixed the bug where" belong in the commit message and the docs.

Every fix or feature that teaches something gets two parts: a short,
current-tense comment in the code, and the story in the doc that covers
that behavior. `docs/04_remote_control_and_iq_output.md`'s "Outbound
status bytes" paragraph and the matching comment in `hpsdr_p1.c` are
the model.

## Where things are documented

| Change touches | Update |
|---|---|
| RX signal chain, `rx_audio.c` | `docs/02_rx_processing_pipeline.md`, `docs/dsp_design_notes/` |
| TX chain, `tx_pipeline.c`, `cw.c`, power | `docs/03_tx_processing_pipeline.md` |
| rigctld, CAT, HPSDR, iq_stream, USB audio | `docs/06_api.md` (client view) and `docs/04_remote_control_and_iq_output.md` (implementation) |
| Startup, threads, failure handling | `docs/05_process_and_threading_model.md` |
| Build, kernel, deployment | `docs/07_build_and_deployment.md` |
| A design decision or build-order step | `docs/ARCHITECTURE.md` §10 |

If a doc says something the code no longer does, fix the doc in the same
change.

## Before calling a change done

- `make` builds with no warnings (`-Wall -Wextra`).
- Run the bench tests that cover what changed: `make test-fft-filter`,
  `test-tx-pipeline`, `test-rx-filter`, `test-rx-audio`,
  `test-upsample48k`, then run the binary. They need no radio hardware.
- `make check-comments` - flags history-style comments in changed files,
  and errors if any source file's header names a different file (the
  sign of a file uploaded under the wrong name).
- A comments-only change must leave the machine code identical: compare
  `objdump -d` and `.rodata`/`.data` before and after, as described in
  `docs/code_comments.md`.
- Say what was verified and how. "Bench-checked" and "confirmed on air"
  are different claims; only the operator can make the second.

## Conventions

- C, `gnu11`. Functions ordered bottom-up, so no forward declarations.
- Diagnostics are opt-in (a compile-time flag or a `MAXIBITX_*`
  environment variable), off by default.
- Nothing may block the real-time audio thread (`sound.c`'s
  `audio_loop()`): no locks shared with ordinary threads, no sleeps, no
  I/O. Hand work to another thread through the existing lock-free rings.
- **Keep each file's line endings.** `Makefile`, `sound.c`/`.h`,
  `hpsdr_p1.c`/`.h` and `vfo.h` are CRLF; everything else is LF. Don't
  convert a file as a side effect of an edit.
- The raw baseband I/Q is spectrally inverted (a station above the dial
  arrives at negative frequency). Consumers correct for it; `sound.c`
  deliberately doesn't. See `docs/02_rx_processing_pipeline.md`.
