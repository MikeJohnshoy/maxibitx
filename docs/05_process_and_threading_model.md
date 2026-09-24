# 05 — process and threading model

Status: current. Covers the startup sequence, the threads once running,
TX transitions, shutdown and startup failure handling, plus a map for
reading `sound.c`.

## Scope

- `main()`'s startup sequence in `maxibitx.c`: board calibration
  (`hw_settings_load()`, `data/hw_settings.ini`) → GPIO → si5351 (clk1
  at its RX value) → board revision/INA260 → software VFO and initial
  tune → CW key (`cw_init()`) → RX demodulator (`rx_audio_init()`) →
  Hamlib/rigctld → HPSDR → I/Q telemetry stream (`iq_stream.c`) → USB
  gadget (`uac_init()`) → Kenwood CAT on the gadget's serial port
  (`cat_init()`) → audio codec → audio thread. See
  [`01_hardware_init_and_control.md`](01_hardware_init_and_control.md)
  for the hardware part of that sequence in detail. Each step logs its
  own `init: ...` result; the sequence ends with
  `maxiBitx: radio hardware initialization complete, ready to serve!`.
  The network and USB interfaces have no dependency on each other, so
  their order is a convention; the one hard dependency among them is
  that `cat_init()` needs the gadget `uac_init()` creates.
- The thread structure once running:
  - the audio thread (`sound.c`'s `audio_loop()`, calling
    `sound_process()` once per ~10.7ms block), `SCHED_FIFO` when the
    binary has `cap_sys_nice`;
  - HPSDR's listener and pacer threads (`hpsdr_p1.c`);
  - the I/Q telemetry stream's listener and pacer threads
    (`iq_stream.c`);
  - the Hamlib accept thread plus one thread per connected client;
  - the USB gadget's UAC writer thread, and its reader thread when the
    capture (TX audio) side came up (see
    [`usb_gadget_OS_setup.md`](dsp_design_notes/usb_gadget_OS_setup.md) §7);
  - the Kenwood CAT thread on `/dev/ttyGS0`;
  - a dedicated TX worker thread (below), started on the first
    `radio_set_tx()` call;
  - and the main thread's idle loop, which also turns the TX test-tone
    generator off and drops PTT after 30 s in transmit.
- **TX transitions run on their own worker thread** (`radio.c`'s
  `radio_tx_worker()`), not on whichever thread calls `radio_set_tx()`.
  This replaced an earlier version where `radio_set_tx()` did its
  PTT/relay-settling `usleep()`s and ALSA mixer call inline, on the
  caller's own thread — harmless from Hamlib's or `hpsdr_p1.c`'s network
  threads, but `cw.c` calls `radio_set_tx()` from `cw_poll_key()`, which
  runs once per ~10.7ms audio block on the real-time audio thread
  (`sound.c`'s `audio_loop()`). A single call there blocking 20ms+ (PTT
  settle + relay settle + opening/closing a fresh ALSA mixer handle)
  guaranteed a missed capture period — the `sound: xrun, recovering`
  logged on every key transition — and made the physical key feel
  sluggish, since `cw_poll_key()` couldn't return to re-poll it until
  the blocking sequence finished. Fix: `radio_set_tx()` keeps its exact
  signature and still updates `in_tx` immediately/synchronously (cheap —
  every other guard in the codebase that reads `in_tx`, e.g.
  `cw_tx_active()` and the network MOX logic, needs to see the new state
  right away, even though the physical relay/mixer change is still
  pending); the actual slow hardware sequence (`radio_tx_apply()`) is
  handed to the worker thread via a mutex/condvar/pending-flag pair, so
  the calling thread — audio thread included — never blocks.
- Graceful shutdown: `main()` installs a `SIGINT`/`SIGTERM` handler
  (Ctrl+C, or a normal `kill`/`systemctl stop` - not `SIGKILL`, which
  can't be caught) that sets a flag; the idle loop notices it, parks
  PTT/the T/R relay low (`radio_set_tx(0)`, in case the key was down at
  the moment of the signal), waits 50ms for PTT and the relay to drop,
  then tears down roughly in the reverse of startup:
  `sound_thread_stop()` first (stops the real-time audio thread that
  feeds `hpsdr_send_iq()`, `iq_stream_send()` and
  `uac_push_audio_rx()`, so no consumer races a producer still calling
  into it), then `cat_stop()`, `uac_stop()`, `hpsdr_stop()`,
  `iq_stream_stop()`, `hamlib_stop()`. See
  [`usb_gadget_OS_setup.md`](dsp_design_notes/usb_gadget_OS_setup.md) §8 for the failure
  mode this fixed (a restart-without-rebooting used to leave the USB
  gadget's configfs tree bound to a dead process) and for the
  independent self-healing fix that still covers `SIGKILL`/a crash,
  neither of which this handler can catch.
- Console reporting: no periodic status line. `status.c`/`status.h` (a
  single-line, redraw-in-place frequency/TX-RX display, once called
  right after the init-complete line) were removed entirely - ongoing
  operational visibility comes from the `rigctl:`/`hpsdr:` command
  echoes described in
  [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md),
  which report a freq/PTT change at the moment it happens rather than a
  point-in-time snapshot.
- Failure handling at startup: GPIO, the HPSDR socket bind and audio
  capture are fatal - `main()` exits if any fails. Everything else is
  best-effort and logs that it's continuing without it: Hamlib/rigctld,
  the I/Q telemetry stream, the USB gadget (and its TX audio direction
  separately), Kenwood CAT, the INA260 power monitor, and audio
  playback (without it there's no local audio and no TX). A missing
  `hw_settings.ini` falls back to compiled-in defaults.

## Reading sound.c

`sound.c` is the second-largest file in the project, and it looks more
forbidding than it is: roughly a third of it is ALSA plumbing and
diagnostics that the audio thread never executes. It's divided by
banner comments, and those are the reliable way to navigate it — search
for the title rather than a line number.

| Banner | What's there | Audio thread runs it? |
|---|---|---|
| Constants | Sample rate, block size, channel layout | — |
| TX sample scaling | The amplitude constants, with the calibration story in [`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md) | — |
| Module state | The PCM handles, the TX pipeline instance, mic gain / TX power / ALC accessors | Reads only |
| ALSA mixer helper | Generic "find a mixer element and set it" helpers | No |
| Codec hardware setup | `setup_audio_codec()`, which calls those helpers about a dozen times to bring up the WM8731 | No |
| ALSA PCM helpers | `open_pcm()`'s hardware-parameter negotiation, and `xrun_recover()` | Recovery only |
| xrun flood tracking | Rate-limiting and the one-time hint when xruns are caused by missing `cap_sys_nice` | Yes, cheaply |
| IQ mixing | `sound_process()` - the receive path: VFO mix, antialias, hand-off to the demodulator and the I/Q consumers | Yes |
| Per-block compute timing | Opt-in instrumentation (`MAXIBITX_LOOP_TIMING`) | Yes, cheaply |
| Audio thread | `audio_loop()` - capture, process, the transmit branch, playback | Yes, all of it |
| Public API | `sound_thread_start()` / `sound_thread_stop()` | No |

**Where the complexity actually is.** The mixer helpers and the codec
setup together are the single largest block, but they run once during
bring-up and can't affect keying or audio. The timing counters are
diagnostics. What's genuinely intricate is `audio_loop()`, and inside
it the transmit branch — mode dispatch across the four audio sources,
the pipeline call, the limiter ceiling, the DAC scaling and the
sidetone — is more than half of the function. For most questions the
answer is in one of `sound_process()`, the transmit branch, or
`setup_audio_codec()`, and the rest can be skipped.

**Why it deserves more caution than the DSP files.** `sound.c` is the
one substantial file with no bench test. `tx_pipeline.c`,
`rx_audio.c`, `fft_filter.c`, `rx_filter.c` and `upsample48k.c` can
each be proven numerically with no hardware attached, so a change to
them is checked before it ever reaches the radio. `sound.c` needs the
codec, the GPIO and an antenna, which means the only verification is
building it and operating. Treat "it compiles" as the start of testing
here, not the end of it.

**If it is ever split up**, the order that keeps the risk lowest is to
move the parts the audio thread doesn't touch first: the mixer helpers
and `setup_audio_codec()` into their own file, then the timing
instrumentation, and only then consider lifting the transmit branch out
of `audio_loop()` into a function of its own — that last one is the
only step that changes code on the real-time path. Each is separately
verifiable by keying up once. None of it is worth doing for tidiness
alone.
