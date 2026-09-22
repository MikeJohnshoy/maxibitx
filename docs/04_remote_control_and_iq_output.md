# 04 — remote control and I/Q output

This covers everything minibitx exposes to the outside world: how an
external control surface tunes and keys the radio, and how the baseband
I/Q produced in
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) gets to
the SDR application actually using it. The two are documented together
because one file — `hpsdr_p1.c` — does both jobs. Every file this
document covers (`hpsdr_p1.c`, `usb_gadget.c`, `iq_stream.c`, `hamlib.c`)
lives under `src/interfaces/` - kept separate from the DSP/radio-control
core (`sound.c`, `rx_audio.c`, `radio.c`, `cw.c`, `vfo.c`, ...) in plain
`src/`, since none of these four decide anything about the signal path
themselves - they only carry state in and out of it for whichever
external app is on the other end.

This document covers how those interfaces are implemented. For the
client-side reference - every command, reply and wire format, and what
an application can and can't do today - see [`06_api.md`](06_api.md).

## The single-entry-point pattern

`radio_tune_to()` and `radio_set_tx()` (`radio.c`) are the *only*
functions that touch hardware to change frequency or key the
transmitter. Every control surface calls into these two functions rather
than poking GPIO or the si5351 directly — so adding a new control
surface never means a second place that can put the hardware in an
inconsistent state. Today there are two callers:

- **Hamlib/rigctld** (`hamlib.c`) — a live frequency control surface
  (`F`), primarily for WSJT-X and similar apps.
- **Kenwood-CAT emulation over USB** (the CAT section of `usb_gadget.c`)
  — a second, independent frequency/PTT control surface (`FA`), for
  control apps like FLRig that can't speak Hamlib's rigctld protocol —
  see below.
- **HPSDR's inbound command parser** (`hpsdr_p1.c`) — calls
  `radio_set_tx()` only, for MOX/PTT (below).

## Hamlib / rigctld server

`hamlib.c` runs a minimal rigctld-compatible TCP server (`hamlib_init()`,
default port 4532, one thread accepting connections and one per client).
It implements a small plain-text rigctl command set:

| Command | Behavior |
|---|---|
| `f` / `F <hz>` | get / set frequency — `F` calls `radio_tune_to()`, which also clears RIT back to 0 (see `j`/`J` below) |
| `t` / `T <0\|1>` | get / set PTT — `T` calls `radio_set_tx()`; any nonzero value means TX (no separate mic/data state) |
| `j` / `J <hz>` | get / set RIT — a receive-only tuning offset, `radio_set_rit()`/`radio_get_rit()` (`radio.c`), range `+/-RIT_MAX_HZ` (`radio.h`, 9999 Hz). Applied to RX's clk2 only; TX's own clk2 (`radio_tx_apply()`) never sees it, so RIT never moves your transmit frequency. Persists across your own TX bursts (restored the moment TX drops back to RX) but auto-clears on the next `F` — a RIT offset dialed in against one frequency has no defined meaning on a different one. |
| `m` / `M <mode> <passband>` | get / set mode — mode itself is real now: `radio_get_mode()`/`radio_set_mode()` (`radio.c`), the single owner `usb_gadget.c`'s `MD` also reads/writes, per `docs/ARCHITECTURE.md`'s build order step 3. `M` rejects an unrecognized mode name (`RPRT -1`, same as an out-of-range `J`) rather than silently accepting it. `CW`/`USB`/`LSB`/`PKTUSB` (→`RADIO_MODE_DIGITAL`) are the names `hamlib.c`'s `mode_to_name()`/`name_to_mode()` recognize — `PKTUSB` is a best-effort stand-in, not yet confirmed against a real WSJT-X rigctld session. `passband` stays cosmetic (echoed back, not applied) — there's still no real passband-width setting to apply it to. Mode selects both the onboard demodulator and the TX audio source (`06_api.md`, "The radio model") |
| `l` / `L <level> <value>` | get / set a hamlib "level" — `AF` (audio/volume, 0.0-1.0), wired to `rx_audio.c`'s `rx_audio_get_volume()`/`rx_audio_set_volume()`; `STRENGTH` (S-meter, read-only); and the `MICGAIN` extension (`sound.c`'s mic_tx_gain); every other hamlib level (`RF`, `SQL`, preamp, ...) gets an error reply, same as an unknown command |
| `u` / `U <func> <0\|1>` | get / set a hamlib "function" — `NARROW` toggles `rx_audio.c`'s stage-3 narrow (~300Hz) post-demod CW filter on/off via `rx_audio_get_narrow_filter()`/`rx_audio_set_narrow_filter()`, and `FFTFILT` picks its implementation (elliptic or FFT). `NARROW` isn't a name real Hamlib ships in its own function table — this server only ever talks to `tools/rigctl_panel.py`, not stock `rigctl`, so there's no compatibility reason to hunt for a closer standard name. Everything else gets an error reply. |
| `chk_vfo` | always reports "not in VFO mode" (single-VFO radio) |
| `dump_state` | minimal capability dump for client negotiation — advertises a real `max_rit` (`RIT_MAX_HZ`) now that `j`/`J` are backed by something; TX ranges come from the `[tx_band]` entries in `hw_settings.ini` (1.8–30 MHz if none loaded) at a flat 5 W, and mode masks list only CW/USB/LSB/PKTUSB; still deliberately reports no XIT/IF-shift/preamp/attenuator/onboard-filter support. The TX ranges are advertisement only — PTT isn't refused outside them |
| `q` / `Q` / `quit` | disconnect |

It's a small command set on purpose: minibitx isn't the thing making
demod/filtering decisions, the SDR app is. Point an SDR app's CAT/rig
control at `127.0.0.1:4532` (rig model "Hamlib NET rigctl") alongside its
HPSDR connection for live retuning.

`l`/`L` and `u`/`U` are the two places this server reaches past pure rig
control into DSP state: `AF` is the only level with anything behind it
(volume of `rx_audio.c`'s local audio), and `NARROW` is the only
function with anything behind it (the local audio's narrow post-demod
selectivity filter) - see
[`dsp_design_notes/rx_audio_demod_design.md`](dsp_design_notes/rx_audio_demod_design.md).
`dump_state`'s `has_get_level` advertises only `RIG_LEVEL_AF` and
`RIG_LEVEL_STRENGTH` (`0x40000008`) and `has_set_level` only
`RIG_LEVEL_AF` (`0x8`), not the full hamlib level set; `has_get_func`/
`has_set_func` stay `0x0` regardless, since `NARROW` isn't a real
`RIG_FUNC` bit to advertise under (see the table above). `tools/rigctl_panel.py`
is a small standalone desktop app (Python/Tkinter, no maxibitx-side
dependency beyond this server) that talks exactly this protocol - a
frequency readout/entry, a volume slider, and a narrow-filter checkbox,
meant to run on a laptop or the Pi's own desktop, connecting to
`<pi-host>:4532` same as any other rigctld client. See `tools/README.md`.

Every command that changes or reports state also echoes to the console,
one line per command, e.g. `rigctl: F 7074000 -> tuned to 7074000 Hz` or
`rigctl: T 1 -> TX on`. `radio_tune_to()`/`radio_set_tx()` (`radio.c`)
themselves print nothing — each control surface logs its own result,
since it's the one that knows which command triggered the change. This
is minibitx's primary window into operational state once startup
finishes; see
[`05_process_and_threading_model.md`](05_process_and_threading_model.md).

## Kenwood-CAT emulation over USB (for FLRig)

`hamlib.c`'s rigctld server works fine for WSJT-X (it has a native
"Hamlib NET rigctl" rig type), but FLRig has no such option — it only
ever speaks CAT over a serial port to what it believes is a real radio.
Rather than build FLRig a TCP-to-serial bridge (`com0com`/`com2tcp` on
Windows, or similar), `usb_gadget.c` implements a second, independent
control surface — in its own clearly-marked "Kenwood TS-480-subset CAT
control" section near the bottom of that file, alongside the UAC2 code
rather than a separate translation unit, since both are just two
functions of the one composite gadget `usb_gadget.c` already owns —
that answers as a Kenwood TS-480 over the USB gadget's own CDC-ACM
serial function (see [`usb_gadget_os_setup.md`](dsp_design_notes/usb_gadget_OS_setup.md)
for the gadget composite-device details) — so FLRig just opens the COM
port Windows assigns the gadget, no extra software involved. The TS-480
subset was picked because it's exactly the CAT dialect the QRP Labs
QMX/QMX+ already emulates for the same reason (old enough to be widely
supported, and QMX has no genuine SSB TX either, so its command set
already excludes modes minibitx can't produce).

**FLRig setup note (field-confirmed):** in FLRig's Rig Control → Setup,
select the **QMX** rig type, not the generic **Kenwood**/TS-480 entry.
Both will tune and key the radio, but the generic Kenwood profile sends
the real TS-480's *full* command set, including split-operation commands
(`FB`/`FR`/`FT`) and menu/filter commands (`EX`/`FW`) that minibitx
doesn't implement — harmless (silently ignored below, same as any real
Kenwood radio would do with a command it doesn't recognize), but a real
source of operator confusion: FLRig's split "VFO B" box under the Kenwood
profile looks like a second frequency display and gets mistaken for a RIT
readout, when it isn't one and RIT traffic (`RT`/`RC`/`RU`/`RD`) never
gets sent by that box at all. The QMX profile sends exactly the reduced
command set this file documents, its RIT control maps directly onto
`RT`/`RC`/`RU`/`RD` below, and it's confirmed on real hardware to make
FLRig's RIT dial move minibitx's own RIT offset correctly end-to-end.

Implemented commands (Kenwood convention: "set" commands get no reply;
only bare "get" queries do):

| Command | Behavior |
|---|---|
| `ID` | get only — always replies `020` (the TS-480's ID code) |
| `FA` / `FB` | get / set frequency, 11-digit Hz — `FA` calls `radio_tune_to()`; `FB` mirrors `FA` on get and is accepted-but-ignored on set (single VFO) |
| `RT` | get / set RIT on/off (`RT0;`/`RT1;`) — `radio_set_rit_enabled()`/`radio_rit_enabled()` (`radio.h`). Unlike Hamlib's `j`/`J`, Kenwood CAT has a genuine independent enable bit: flipping RIT off leaves whatever's dialed in via `RU`/`RD` below untouched, same as a real rig's RIT ON/OFF button vs its RIT knob. |
| `RC` | RIT/XIT clear, no reply — calls `radio_set_rit(0)`, same as Hamlib's `J 0` (zeroes the value **and** disables it, see `radio_set_rit()`'s comment in `radio.c`) |
| `RU` / `RD` | step RIT up/down by a fixed 10 Hz per call (`CAT_RIT_STEP_HZ`), clamped to `+/-RIT_MAX_HZ`, no reply — implicitly re-enables RIT via `radio_set_rit()`, same as turning a real RIT knob does regardless of the ON/OFF button's last state. A real rig's optional step-count suffix (`RU005;`) is accepted but ignored — minibitx has no configured step size to multiply it against |
| `TX` / `RX` | bare, immediate PTT, no reply — calls `radio_set_tx()`, same "local CW key wins" guard as Hamlib's `T` |
| `TQ` | get / set PTT (0/1) — another way to ask for the same thing as `TX`/`RX` |
| `MD` | get / set mode — real now, same `radio_get_mode()`/`radio_set_mode()` Hamlib's `M`/`m` use, translated to/from single-digit Kenwood codes (`1`/`2`/`3` = LSB/USB/CW). DIGITAL reports as `2` (USB) and can't be selected over CAT; an `MD2` while already in DIGITAL is a no-op (`ARCHITECTURE.md` §10 step 12). An unrecognized digit is silently ignored, same convention as any other command this surface doesn't recognize — not Hamlib's `RPRT -1`, this protocol has no error-reply concept. Defaults to `3` (CW), the one mode minibitx can actually transmit |
| `IF` | get only — combined status string (frequency, RIT, TX/RX, mode). RIT's 5-char signed offset and on/off digit are real now (`radio_get_rit()`/`radio_rit_enabled()`); XIT/memory/scan/split/tone are still reported as off/zero — minibitx has none of those. The field width for the RIT portion is unchanged from the all-zero version this replaced — see `usb_gadget.c`'s own comment on this command for the layout's confidence level (reconstructed from general convention, not confirmed against a packet capture) |
| `AG` | get / set AF (volume) gain, Kenwood format (1-digit VFO selector, ignored — single VFO — + 3-digit level 000-255) — calls the same `rx_audio_set_volume()`/`rx_audio_get_volume()` `hamlib.c`'s `l`/`L AF` already uses, just reached over the CAT wire format instead of rigctld's. FLRig's volume slider sends a continuous stream of `AG0nnn;` sets while dragged (not just on release); each one is just applied directly. |

Anything else is silently ignored, matching real Kenwood radios rather
than inventing an error reply convention that doesn't exist in the CAT
protocol. Like every other control surface here, `cat_init()` failing
(most commonly: no USB gadget support on this hardware/kernel, or the
gadget failed to bind) is not fatal — minibitx keeps running on whatever
subset of control surfaces actually came up.

Unlike Hamlib clients such as WSJT-X, FLRig has no async push mechanism
for rig status — it actively polls every "get" query (`FA`, `FB`, `MD`,
`ID`, ...) on every UI cycle to keep its own display current,
bench-confirmed at several times a second regardless of whether anything
changed. Logging every one of those floods the console with
near-identical repeated lines — fixed in `cat_handle_command()`
(`usb_gadget.c`) with `cat_log_get()`: a get's console line only prints
when the reply actually differs from the last reply of the same kind,
the same "don't log unchanged, routine state" principle already applied
to `uac_writer_thread()`'s backoff logging and `hpsdr_p1.c`'s IQ pacer
thread. A *set* (an actual operator action, not a routine poll) always
logs. `AC` (antenna tuner control) is part of FLRig's standard status
poll too, but minibitx has no tuner to report on — recognized and
silently ignored rather than falling into the generic "unrecognized"
branch, which would otherwise log this benign, expected query on every
poll forever.

WSJT-X needs no changes and keeps using the Hamlib server above; this is
purely additive.

## HPSDR Protocol 1 — inbound (control) and outbound (I/Q)

`hpsdr_p1.c` implements a minimal openHPSDR Protocol 1 link over UDP,
and handles both directions of that link:

**Inbound (EP2):** frequency and mode control live entirely in the
rigctld server above — the *only* thing `hpsdr_p1.c` reads from the
inbound stream is the MOX (PTT) bit, since some SDR apps key PTT through
the I/Q link's C0 byte even while using CAT for everything else. (Since
then it also follows the app's RX1 frequency, C&C addr 0x02, while
receiving, and retunes to its TX frequency, addr 0x01, when MOX is set -
see `06_api.md`'s HPSDR section. The transmit I/Q samples are still
ignored.) When it
sees that bit change, it calls `radio_set_tx()` — the same entry point
Hamlib uses, never a separate path — and echoes it to the console as
`hpsdr: MOX -> TX on` / `hpsdr: MOX -> TX off`, tagged `hpsdr:` rather
than `rigctl:` so it's clear which control surface actually drove the
change.

Getting this parser right took a few real bugs shaking out on the
bench, all against `process_ep2_frame()`/`handle_command()`
(`hpsdr_p1.c`): an early, looser version with no exact-length checks on
inbound packets let torn/malformed EP2 frames decode into nonsensical,
wildly-varying C&C addresses with the MOX bit effectively random — the
strict per-packet-type length checks now in `handle_command()` (modeled
on piHPSDR's `hpsdrsim.c`, a mature reference for this side of the
protocol) fixed that. Two more surfaced even with strict lengths in
place: reading MOX from every C&C address (matching `hpsdrsim.c`
literally) still picked up unrelated register data as spurious PTT
activity, fixed by only acting on it from address 0; and comparing the
network's want-TX bit against the shared `in_tx` (which the local key
in `cw.c` also writes) let an unchanged, already-stale network value
look like a fresh request the instant the local key's hang timer
released TX, latching TX on permanently after a single key press. Fixed
with `net_mox` — a variable tracking only the network's own last-seen
MOX bit, independent of `in_tx` — so only a genuine *change* in what
the network is sending can trigger an action, never `in_tx` moving for
an unrelated (local-key) reason.

**Outbound (I/Q):** `hpsdr_send_iq()` (called once per audio-thread
block from `sound_process()`) only ever appends to a lock-free
single-producer/single-consumer ring buffer and returns immediately - a
dedicated pacer thread (`hpsdr_pacer_thread()`) is the sole consumer,
draining it and sending one packet every 1.3125ms (126 samples @
96kHz), paced against an absolute wake time so the long-run rate never
drifts. This replaced an earlier version that packetized and sent
inline, directly from the audio thread, in one tight loop per block -
which fired a whole block's ~9 packets back-to-back within ~1ms, then
sent nothing for the remaining ~10ms until the next block, a bursty
pattern real HPSDR hardware never produces. SDR Console tolerated it;
SparkSDR's jitter buffer apparently did not (the leading suspect for a
~170ms audio warble reported against it specifically - same I/Q
content either way, only its arrival timing differed from what real
hardware would produce). Two earlier fix attempts were tried and
reverted before landing on the ring buffer: `usleep()` between packets
directly on the audio thread caused immediate, continuous xruns (a
capture period is real wall-clock time already fully spent; sleeping
on top of it is a permanent rate deficit, not absorbable jitter, and
the capture ring buffer overran within about 6 iterations); a
mutex-protected queue reproduced the identical xrun symptom for a
different reason - a plain mutex shared between the audio thread's
`SCHED_FIFO` max priority (see
[`08_troubleshooting_and_bringup.md`](08_troubleshooting_and_bringup.md))
and this file's ordinary-priority pacer is a textbook priority-inversion
trap, since the default Linux pthread mutex has no priority-inheritance
protocol. The lock-free design avoids blocking either side by
construction: the producer always advances (dropping a sample rather
than ever waiting on the consumer), and the consumer just drains
whatever's available each tick. I and Q values are scaled up before
sending to make SDR apps happier. This file has no dependency on
`usb_gadget.c` — each holds its own independent copy of the I/Q.

**Outbound status bytes (C0-C4).** Each 512-byte frame the radio sends
starts with a C0 byte whose layout is fixed by the protocol: bits 7:3
say which status field C1-C4 carry, bit 2 is the CW dot paddle, bit 1
the dash paddle, bit 0 PTT. `build_and_send_packet()` now rotates
status fields 0-4 one per frame, as real hardware and sbitx's own
`hpsdr_p1.c` do, and reports everything as zero except a firmware
version in field 0. Until 2026-09-22 it put a 0-31 counter in bits 5:1
instead (the host-to-radio EP2 layout, not the radio-to-host one), so
the dash bit toggled on every frame and the dot bit every other packet
- 750 of 1,000 frames carried a paddle closure in a bench capture - and
fields 0 and 2 carried the dial frequency, which a client reads as the
ADC-overload flag and firmware version. SDR Console ignored all of it;
SparkSDR's receive audio had a loud "buzzsaw" across the dial, the
prime suspect being SparkSDR acting on those phantom paddle closures.
Fixed and bench-checked (no paddle bits set, fields cycling 0-4); the
SparkSDR re-test is pending.

## Lightweight I/Q telemetry stream (`iq_stream.c`)

A third, independent way to get baseband I/Q off the box, built
specifically so a simple client (`tools/rigctl_panel.py`'s spectrum
display) doesn't have to implement either protocol above. Unlike HPSDR
Protocol 1, which keeps exactly one client destination and would
silently hand a second client the stream out from under the first, this
supports several simultaneous subscribers - a UAC2 host, WSJT-X on
HPSDR, and the spectrum panel here can all be pulling I/Q at once,
independently, with none of the three aware of the others (`sound.c`
hands each its own copy, same as the other two).

UDP, port 4536. No discovery/start-stop handshake: any datagram to that
port subscribes; a client stays subscribed by resending one at least
once every 5 seconds, and is dropped (no explicit unsubscribe needed)
after that. Native 96kHz I/Q, packed as 128-sample batches of (int16 I,
int16 Q) pairs behind a small header (magic, sequence number, sample
count) - see
[`dsp_design_notes/iq_stream_design.md`](dsp_design_notes/iq_stream_design.md)
for the exact wire format, the real-time-safety design (the same
lock-free ring buffer + dedicated pacer thread pattern `hpsdr_p1.c`
uses, plus a separately mutex-protected subscriber list - both threads
touching that list are ordinary priority, never the audio thread, which
is what makes a plain mutex safe there but not around the sample ring
buffer itself), and full bench verification.

## USB Audio Class (UAC2) — bidirectional real audio (WSJT-X bridge)

`usb_gadget.c` presents the radio as a standard, full-duplex USB Audio
Class 2.0 device ("sBitx Audio"), if the hardware/kernel support it
(needs a USB device-mode controller and `libcomposite`) - see
[`usb_gadget_os_setup.md`](dsp_design_notes/usb_gadget_OS_setup.md) for the Raspberry Pi 4
config.txt/cmdline.txt changes and GPIO-power caveat this requires; it is
not on by default on a stock Raspberry Pi OS install.

**This gadget no longer carries I/Q at all** (see `ARCHITECTURE.md` §10
step 9 for the full design and why) - an earlier version carried raw
baseband I/Q here, for an external SDR-console app (SDR#, HDSDR, GQRX,
SDR Console) to demodulate itself; that entire path is gone. In its
place: real, already-demodulated 16-bit/48kHz PCM audio, genuinely
bidirectional, meant for a control-surface app like WSJT-X to decode/
encode from directly, the same way it would talk to a real radio's own
USB audio codec - see
[`06_api.md`](06_api.md)'s "Using it with WSJT-X" for the
operator-facing setup steps.

Mono, both directions (`ARCHITECTURE.md` §10 step 11) - an earlier
revision advertised 2 channels per direction with the same sample
duplicated onto both, hedging for host/app compatibility with devices
that assume a stereo interface; removed as unnecessary once real WSJT-X
bring-up made clear that mono is the normal case for a ham-radio
digital-mode audio interface, not an edge case, the same way a SignaLink
or RigBlaster is mono both directions.

- **RX (device → host):** fed from `rx_audio.c`'s own on-target
  demodulation - the same signal the local speaker gets, tapped one
  stage earlier (after the AGC's makeup gain, before the operator's own
  `rx_volume` knob - see `rx_audio.h`'s `uac_out` parameter) so a remote
  decoder's level doesn't drift with the operator's own listening
  volume. Decimated from `sound.c`'s native 96kHz down to the 48kHz this
  gadget advertises, exactly as the old I/Q version was (`decim48k.c`,
  unchanged - see
  [`dsp_design_notes/usb_uac_decimation_design.md`](dsp_design_notes/usb_uac_decimation_design.md)
  for the filter design).
- **TX (host → device):** genuinely used now, unlike the old I/Q
  version's declared-but-unused playback direction. WSJT-X's own
  generated tone arrives over this gadget's capture-side PCM at 48kHz
  and is upsampled to 96kHz (`upsample48k.c` - the interpolating
  counterpart `decim48k.c` never needed until this) before riding into
  the shared TX pipeline (`tx_pipeline.c`) for `RADIO_MODE_DIGITAL`.

One card, two independent PCM substream directions, not two separate
devices: binding the UAC2 function to a UDC makes the kernel register a
single ALSA card ("UAC2Gadget"), whose one device exposes both a
playback substream (what streams out to the host, as its capture/mic
input - `uac_writer_thread()`'s job) and a capture substream (what the
host sends, as its playback/speaker output - the new
`uac_reader_thread()`'s job). Both threads use the same lock-free
single-producer/single-consumer ring-buffer pattern `hpsdr_p1.c`'s I/Q
queue already established, one queue per direction, so a stalled or
absent USB host in either direction can only cost USB audio quality,
never `sound.c`'s real-time audio thread.

Either direction works without the other: RX audio keeps flowing to a
connected WSJT-X even if the capture-side PCM never opens (no host
sending TX audio yet), and none of `uac_init()`, `cat_init()`, or
`hamlib_init()` failing is treated as fatal at startup — minibitx keeps
running on whatever subset of control/streaming surfaces came up
successfully; see
[`05_process_and_threading_model.md`](05_process_and_threading_model.md).
