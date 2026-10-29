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
- A receiver whose spectrum is full of ghosts: the codec's start-up
  timing pattern, maxibitx's check for it, and the raw-capture dump for
  looking at what the codec delivers
  ([below](#a-spectrum-full-of-ghosts)).
- Surveying the receiver's own spurs with a dummy load, on a new board
  or after a hardware change ([below](#the-receivers-own-spurs)).
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
default; on a zBitx the startup log then says `init: radio board: zBitx`.

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

## A spectrum full of ghosts

Now and then (about one start in 25 on one sBitx) the WM8731 starts with
its sample timing modulated in a pattern that repeats every 128 samples.
It makes 96 kHz from a 12 MHz crystal in its USB mode, 125 clocks a
sample (`/sys/kernel/debug/regmap/1-001a/registers`: register 8 = 0x1d),
and in this state its sampling instants wobble with that cycle. Every
received signal then gets copies at multiples of 750 Hz, the strongest
3750 Hz away: a strong station off the side of the display throws a
picket fence of them across it, and the copies pushed past either end of
the codec's band fold back and tune backwards. The spectrum looks wavy,
with real signals among many ghosts. A transmission doesn't clear it;
restarting maxibitx usually does.

The codec's registers are the same in this state as in a good start.
What clears it is restarting the codec's digital interface (register 9,
ACTIVE, off and on), and maxibitx checks for it and does that at
start-up (`src/codec_check.h`). A second after the audio starts it
measures the pattern on the right channel, the mic line, which is quiet
in receive: the power of its sample-to-sample differences, folded over
the 128-sample cycle, has a strong 5th harmonic in this state (about 0.9
of the mean) and almost none otherwise (under 0.01). One of these comes
between `sound: running` and the `ready to serve!` line, which waits up
to 5 s for it (if the check can't decide by then - transmitting at
start-up, say - its line follows later):

```
codec: sampling clean (0.002)
```

```
codec: started with a 128-sample timing pattern (0.935) - every signal would have ghosts every 750 Hz; restarting its interface
codec: sampling clean after 1 interface restart (0.003)
```

It tries up to three restarts. If it says `still patterned` or `couldn't
restart its interface`, restart maxibitx. The restart can also be done
by hand while maxibitx runs (the bus is the number in the regmap
directory's name, `1-001a`; `-f` because the kernel driver owns the
device):

```
sudo i2cset -f -y 1 0x1a 0x12 0x00    # register 9, ACTIVE = 0
sudo i2cset -f -y 1 0x1a 0x12 0x01    # ACTIVE = 1
```

It was found with the raw-capture dump. To see what the codec is
delivering, send maxibitx SIGUSR1:

```
pkill -USR1 maxibitx
```

It records the next second of the raw capture (both channels, exactly as
`snd_pcm_readi()` gave them, before any processing) and writes it to
`/tmp`, saying so on the console:

```
capture_dump: SIGUSR1 - recording 1 s of the raw capture
capture_dump: wrote /tmp/maxibitx_capture_20261026_174829.wav - 1 s, 96000 Hz, 2 channels, S32, 14074000 Hz DIGI, crystal filter centre 40012400 Hz
```

The radio carries on as before. The file is a 32-bit stereo WAV (L = the
RX IF, R = the mic), 768 kB. `tools/capture_check.py` reads it (on the Pi
or anywhere). Take one from a good start too, on the same band, to
compare. What each fault looks like:

| `capture_check.py` shows | in a good capture | points to |
|---|---|---|
| timing pattern on R about 0.9; mirror on L well above 0, a strong signal's copies 3750 Hz apart | under 0.01 | the codec's timing pattern, above |
| mirror about 24 kHz | about 0, signals 70 dB or more above their mirrors | - |
| mirror near 1, every signal the same few dB above its mirror; even RMS several dB from odd | even and odd within 0.1 dB | a gain difference between alternate samples |
| mirror near 1; a sample repeats the one before it most of the time, on even or on odd samples | about 0% | the codec running at half the rate, each sample sent twice |
| far fewer or more low bits always 0 than the codec's (8 for 24 bits in 32), nearly all samples positive, high level | 8, half positive | the I2S frame read a bit clock off |
| L and R correlated, or the mic on L | about 0 | the channels swapped or mixed |

Two things show in every capture, good or bad, and are not faults: the
noise floor rippling with an 8 kHz period, and hum at multiples of
93.75 Hz (96000/1024, the audio block rate) on both channels.

## The receiver's own spurs

Every receiver makes some signals of its own: harmonics of the Pi's,
the codec's and the si5351's clocks, regulator noise, products of the
oscillators. With a 50 Ω dummy load on the antenna jack,
`tools/spur_survey.py` finds them across the ham bands and says from
how each moves with the dial what it is — a fixed RF frequency coupled
into the input, a fixed offset made after the first mixer, a backward
image, or an oscillator mixing product — and which clocks have a
harmonic there. Worth running on a new board, after a hardware change,
and when a birdie is heard:

```
python3 tools/spur_survey.py --host sbitx.local
```

About 8 minutes for 80 m to 10 m. It prints each spur with its level
over the floor and its suspects, and writes CSVs and a plot. Then, to
find where a spur comes from, run again with one thing changed and
compare:

```
python3 tools/spur_survey.py --host sbitx.local --lpf-off --out relays_open
python3 tools/spur_survey.py --compare spur_survey_20261029_140000_spurs.csv relays_open_spurs.csv
```

- `--lpf-off` (every LPF relay open): spurs that vanish come in through
  the antenna path; those that stay are made or coupled on the board.
- `--drive 4` (the si5351 at 4 mA instead of 8, `L SIDRIVE`, restored
  after): spurs from its clocks' harmonics, and the products they make,
  should drop. A lower drive also means less LO at the mixers; before
  keeping one, check a real signal's level at each (a steady station
  on the S-meter, or `lpf_sweep.py`'s LPF_A level), and transmit power
  on a wattmeter, since the per-band power scales were set at 8 mA.
  maxibitx starts at 8 mA every time.
- By hand: HDMI, Wi-Fi and USB devices off, a linear supply in place of
  a switching one, or `L XTALCENTER` moved 2 kHz (fixed-RF spurs stay;
  mixing products move).

Keep the dummy load's lead short and dressed the same way for every
run: a spur the Pi radiates into it is real, but its level depends on
the lead. The background:
[`rx_artifacts_and_spur_survey_study.md`](dsp_design_notes/rx_artifacts_and_spur_survey_study.md).

## What the console shows

Once start-up is done, the console reports what happens to the radio:
tuning, mode, PTT and its refusals, filter and keyer changes, LPF
switching, clients connecting and leaving, xruns. It does not report what
clients ask about it. rigctld's reads (`f`, `l STRENGTH`, `u MORSE`, ...)
print nothing, so the control panel's twenty-odd reads a second stay off
the screen; CAT prints a read only when its answer changes. A front-panel
knob prints one line when it stops (`knobs: tuned to 14060200 Hz`,
`knobs: volume 54%`), and a push prints the new step or the mute.

To see everything a rigctld client sends, start maxibitx with
`MAXIBITX_RIGCTL_TRACE=1` (the `init: Hamlib/rigctld listening` line
says it's on); for TCI, `MAXIBITX_TCI_TRACE=1`, above.
