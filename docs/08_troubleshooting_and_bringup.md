# 08 — troubleshooting and bring-up

Status: has real content now (audio thread xruns, below) - no longer a
candidate to fold back into
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md).

## Scope

Hardware bring-up gotchas that don't belong in the design docs proper:

- The si5351's I2C bus number is Linux-assigned (22 on the sBitx via
  the `i2c-rtc-gpio` overlay, 3 on the zBitx), not a fixed hardware
  address — re-check with `i2cdetect -l` if the si5351 ever stops
  responding after an OS/kernel update, and set `i2c_bus` in
  `hw_settings.ini` if it has moved, per the note in
  [`01_hardware_init_and_control.md`](01_hardware_init_and_control.md).
- maxibitx refusing to start because `hw_settings.ini` doesn't name the
  board, and the zBitx's `RX_LINE` needing the UART off
  ([below](#the-radio-board-at-startup)).
- Anything else discovered during bring-up on real hardware that would
  otherwise get rediscovered the hard way a second time.

## The radio board at startup

**maxibitx exits straight after reading `hw_settings.ini`.** It prints:

```
init: hw_settings.ini must name the radio board with one of:
init:     sbitx_version = SBITX_V3    (sBitx (DE, v2 or v3))
init:     sbitx_version = SBITX_V4    (zBitx)
init: add the sbitx_version line for this radio above the first [section] of data/hw_settings.ini - maxibitx will not start without it
```

and exits with status 1, having touched no GPIO line or clock. The file
has no `sbitx_version` line, names something else (the value is matched
exactly, case included; the log adds `it names "...", which is none of
these`), or wasn't found at all - `data/hw_settings.ini` is read
relative to the working directory, so run maxibitx from the repository
root. The lines before it say which: `init: data/hw_settings.ini not
found`; `... has a key "..." that maxibitx doesn't read - a misspelling
of sbitx_version?`; `... has zbitx's hw= key, which maxibitx doesn't
read`; or `sbitx_version in ... is inside a [section], so it is
ignored`. Add the right line above the first `[section]`. There is no
default; on a zBitx the startup log then says `init: radio board: zBitx
- receive only, transmit is not enabled on this board`.

**On a zBitx, GPIO setup fails on BCM 15.** `gpio: cannot request BCM15
('maxibitx-rx_line'): ...` (typically `Device or resource busy`), then
`init: GPIO setup failed`. BCM 15 is `RX_LINE`, which connects the receiver, and it is
also the UART's RXD pin, so the serial console or the UART driver holds
it. Turn off the serial console and the UART (for example with
`raspi-config`, Interface Options → Serial Port, both off; or remove
`console=serial0,...` from `cmdline.txt` and `enable_uart=1` from
`config.txt`) and reboot.

**On an sBitx, the knobs can't be claimed.** `gpio: cannot request edge
events on BCM17/BCM27/BCM22/BCM9/BCM10/BCM11 ('maxibitx-knobs'): Device or resource busy`,
then `init: front-panel knobs unavailable, continuing without them`. The
volume knob is on BCM 9, 10 and 11, SPI0's MISO, MOSI and SCLK, so SPI
must be off (`dtparam=spi=on` removed from `config.txt`, or
`raspi-config`, Interface Options → SPI, off) - as it must be for
`LPF_C` and `LPF_D` on SPI0's chip selects. The radio runs without the
knobs.

**A knob counts backwards, or two steps a click.** See
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md),
"The front-panel knobs": swap its A and B pins, or change its
`edges_per_detent`, in `radio_hw.c`.

## Audio thread xruns (hw:0,0 capture/playback)

`sound.c`'s audio thread requests `SCHED_FIFO` at the max priority when
it starts (`sound_thread_start()`), matching a fix zbitx's own
`sbitx_sound.c` needed for the same underlying reason: as an ordinary
`SCHED_OTHER` thread, it competes with everything else on the system
and can be preempted long enough to miss an ALSA period. Requested via
`pthread_attr_t` so `pthread_create()` itself fails fast (typically
`EPERM`) if the privilege isn't available, rather than silently falling
back to normal scheduling well after `main()`'s "ready to serve!" line
has already printed. Not fatal if it fails (no root / no
`CAP_SYS_NICE` / no rtprio limit) — it warns once, synchronously, and
retries with default scheduling.

Two distinct xrun failure modes have shown up on real hardware, needing
different fixes:

- **A genuinely wedged device.** `xrun_recover()`'s bare
  `snd_pcm_prepare()` is the standard one-shot fix for a fresh
  `-EPIPE`, but on a device stuck for a more persistent reason,
  `prepare()` itself can fail — `snd_pcm_drop()` (discard whatever's in
  the ring buffer, rather than assume `prepare()` already put the
  device in a clean state) then `prepare()` again is a heavier fallback
  some ALSA drivers need to actually clear a stuck xrun. Both capture
  and playback call sites must check this function's return value and
  stop retrying if it's still negative — a caller that ignores a
  continued failure spins forever, printing "sound: xrun, recovering"
  at the full audio-loop rate with no backoff (a real bug: the playback
  path used to do exactly this, silently discarding the return value
  unlike the capture path beside it — fixed by disabling playback/CW
  sidetone output gracefully instead, with one clear message, rather
  than spinning).
- **No real-time scheduling privilege.** Reported symptom: an immediate
  xrun flood on startup with no HPSDR consumer even connected, right
  after a `"failed to set audio thread to SCHED_FIFO"` warning. Here
  every individual recovery genuinely succeeds — the device isn't
  stuck — but an ordinary-priority thread just isn't scheduled promptly
  enough to feed the next period before the one after that underruns
  too, forever, at the full ~93Hz loop rate. `xrun_note()`'s flood
  tracker rate-limits the logging, prints a one-time hint pointing at
  the real cause (grant `CAP_SYS_NICE`, e.g. `sudo setcap
  cap_sys_nice,cap_dac_override+ep ./maxibitx` - the same capability set
  the Makefile grants, since `setcap` replaces rather than adds, and
  dropping `cap_dac_override` would break `usb_gadget.c`'s configfs
  setup - or raise the rtprio limit), and adds a
  short breather so the retry loop doesn't itself worsen the CPU
  contention causing it.

**Playback buffer depth is settable, for measuring.** `sound.c` gives
playback four periods (42.7 ms) by default and keeps it primed full, so
about three periods less the processing time — 28.0–28.8 ms measured on a
Pi 4 — sit queued ahead of every block written. That queue is both the
local sidetone's latency and the loop's margin against an underrun.
`MAXIBITX_PLAYBACK_PERIODS=2`..`8` overrides the default, to find out what
a given board can hold:

```
MAXIBITX_PLAYBACK_PERIODS=3 ./maxibitx
```

The console confirms it (`sound: MAXIBITX_PLAYBACK_PERIODS=3 ...`, then
the granted buffer on the `sound: opened ... playback` line); anything
outside 2–8 is refused with a message and the default used. Capture stays
at four periods either way, since its depth adds no latency. What the
queue actually is, while running:

```
for i in $(seq 300); do
  awk '$1=="delay"{print $3}' /proc/asound/card0/pcm0p/sub0/status
  sleep 0.013
done | sort -n | awk 'NR==1{min=$1} {max=$1} END{printf "min %d frames (%.1f ms)  max %d frames (%.1f ms)\n", min, min/96, max, max/96}'
```

The minimum is the queue just before a write. Any `sound: xrun` line means
this board can't hold that depth under that load; test under the heaviest
load you run (TX, the panel's spectrum, WSJT-X on the gadget). The why:
[`dsp_design_notes/cw_keyer_design_study.md`](dsp_design_notes/cw_keyer_design_study.md) §7.

**Whether the first element reaches the air.** Sidetone and RF share the
playback queue, so the queue is also how long the TX-up sequence has to
unmute the exciter drive before the first keyed sample arrives at the DAC.
`MAXIBITX_TR_TIMING=1` prints one line per transmission start saying which
wins, timed from the moment TX was requested:

```
tr: drive up at +27.9 ms, first TX sample at the DAC at +14.4 ms (written +0.1, queue 7.0, pipeline 7.3) -> CLIPPED 13.5 ms
```

`spare` means the drive was up before the first sample arrived; `CLIPPED`
means that many milliseconds of the first element were generated into a
muted path. Only a transmission starting from receive goes through the
TX-up sequence, so leave more than the 300 ms hang between tests. It times
the software sequence only; anything the T/R switch or PA needs after
`TX_LINE` is outside it. maxibitx also checks the combination at start-up:
if the playback depth and `ext_ptt_delay_ms` together predict clipping, it
prints a `sound: WARNING` with the estimated milliseconds lost and the two
fixes. The estimate uses Pi 4 timings, so on a slower board it errs on the
hopeful side — the timing line above is the real figure. Background:
[`dsp_design_notes/cw_keyer_design_study.md`](dsp_design_notes/cw_keyer_design_study.md) §8.

**Playback must be fed continuously, not just during a CW burst.**
ALSA's underrun detection is tied to the hardware clock draining the
ring buffer against the software pointer, not to whether `writei()` is
being called — so only writing during an active CW burst let the
device sit with nothing arriving for however long the key was up,
draining its ~43ms buffer and underrunning between every single burst.
The first write of the *next* burst would then hit that stale underrun
and need recovery — exactly the "xrun, recovering" storm previously
seen on every key-down. Writing silence the rest of the time keeps the
device continuously running, the same design real sbitx's own
full-duplex audio path uses.

## Your own signal on the spectrum during TX

The receive chain keeps running during TX, so anything reaching the RX
input shows on the panel's spectrum (and in SparkSDR over HPSDR), 1400 Hz
low because clk1 moves to `bfo_freq` for TX. TX mutes the WM8731's left
line input to prevent that; start-up says how:

```
sound: TX mutes RX capture with the line-input mute ('Line' LEFT)
```

To check it during a transmission, `amixer -c 0 sget 'Line'` should show
the left channel `[off]` and the right (mic) `[on]`. If start-up instead
says `'Line' has no separate LEFT capture switch`, TX is only turning
`'Capture'` LEFT down (-34.5 dB), and a strong line at -1400 Hz with its
image at +1400 Hz is expected while transmitting.
[`rx_gain_and_level_calibration.md`](dsp_design_notes/rx_gain_and_level_calibration.md) §8.

## The key jack

At start-up maxibitx prints which lines it keys from:

```
init: key ready - tip BCM 5, ring BCM 4, edge-timestamped, 3 ms debounce; paddles tip = dot, ring = dash
init: keyer straight, 20 WPM - set with rigctld U KEYER / L KEYSPD or CAT KS
```

maxibitx always starts as a straight key at 20 WPM. **A paddle that seems
dead, or keys only one element per squeeze,** is usually the keyer mode:
check `u KEYER` (0 straight, 1 bug, 2 ultimatic, 3 iambic A, 4 iambic B).
A mode change waits until the keyer is idle, so it doesn't take effect
while a paddle is held.

**A mono plug in the stereo jack** grounds the ring. A contact found
closed at start-up is ignored straight away, and 250 ms later, if it is
still closed:

```
key: ring (BCM 4) closed since start-up - treating as a mono straight-key plug; that contact is ignored
```

The key then works from the tip. If instead the contact opens within the
250 ms, it was a key held down during start-up, and both contacts are in
use:

```
key: ring (BCM 4) opened within 250 ms - a key held down, not a mono plug; both contacts in use
```

Pulling the mono plug opens the ring and brings it back into use (`key:
ring (BCM 4) opened - plug changed, both contacts in use`). A mono plug
inserted **while maxibitx is running** can't be told from a key held down,
and in CW it transmits until it is pulled — restart maxibitx after
changing to one. Both contacts closed at start-up (a mono plug with the
key held, or a squeezed paddle) ignores both until one opens.

**Checking the jack by hand.** Stop maxibitx (it holds both lines), put a
mono plug in the jack with the key open, and read both lines:

```
gpioget -c gpiochip0 -b pull-up 4 5      # libgpiod 2.x: "4"=inactive "5"=active
gpioget gpiochip0 4 5                    # libgpiod 1.x: 0 1
```

The line reading `inactive`/0 is the ring, grounded by the plug's sleeve;
on the DE board that is BCM 4. `-b pull-up` matters: without it an open
contact's reading depends on whatever bias the last user of the line left.

**Contact bounce.** `key_debounce_ms` (top-level in
`data/hw_settings.ini`, above the first `[section]`; 3 if absent, 0–20)
is how long a contact's further edges are ignored after one is taken. The
edge itself is taken at its own time, so this adds no delay; it only
limits the shortest mark or space. A key that still produces stray short
elements wants more; `init: key_debounce_ms loaded from ...` confirms the
value in use.

**Warnings from the input thread.** `key: the kernel dropped N edge(s)`
means the kernel's queue of edges overflowed before the thread read it,
which should never happen with a real key; `key: edge queue full` means
the audio thread stopped taking edges. Either way the key's state is
recovered, but that element's timing isn't.

## TCI clients

**Nothing connects.** The console says `init: TCI server listening on TCP
50001` at start-up, or why not (`tci_port=0`, or the port already in
use). Check the client from a laptop with `tools/tci_client.py --host
<pi>`, which also checks the start-up exchange JTDX and Hamlib depend on.
A client that finds all the slots full gets HTTP 503 and the console
says `tci: connection from ... refused`; raise `tci_max_clients`.

**Seeing what a client sends.** `MAXIBITX_TCI_TRACE=1 ./maxibitx` prints
every TCI command in each direction (`tci: <- 0 vfo:0,0,14074000;`,
`tci: -> all ...`), with the client's slot number. Commands maxibitx
doesn't know are ignored silently, so a client waiting on a reply that
never comes shows up here as a command with no answer.

**Transmitting.** The console reports each TCI PTT and where its audio
comes from: `tci: client 0 TX on (audio from its TCI stream)` is what
JTDX and WSJT-X Improved should produce (they key with source `tci`).
`audio from the USB gadget` or `no TCI stream` means the client keyed
without it - check its TCI audio setting. A client that disconnects while
transmitting releases TX and says so.
