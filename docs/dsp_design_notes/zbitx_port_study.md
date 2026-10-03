# Porting maxibitx to the zBitx and Pi Zero 2W: a study

Status: **proposed** - written before any code, on the `zmax` branch.
§9 records the decisions taken on it so far.

The request: run maxibitx's headless radio and its external interfaces
on the zBitx hardware with a Raspberry Pi Zero 2W, in place of the
sBitx and Pi 4 it runs on today. None of the zBitx's own front-panel or
user-interface code is to be kept. The radio hardware is described as
virtually identical, with one more low-pass filter to switch, possibly
different GPIO pins, and an extra control line for the T/R switch.

The zBitx code read for this study is
[drexjj/zbitx](https://github.com/drexjj/zbitx/tree/devcwmod), branch
`devcwmod`, commit `746e181` (2026-10-01), mainly `src/sbitx.c`,
`src/sbitx_gtk.c`, `src/sbitx_sound.c`, `src/i2c.c` and
`data/default_hw_settings.ini`. Where this study says the zBitx does
something, that is what that code does; it has not been checked against
a schematic. Predictions are labelled as predictions.

## 1. The answer in brief

**Feasible, and mostly confined to the board layer.** Once the zBitx's
wiringPi pin numbers are converted to BCM numbers, every signal the two
boards share is on the same pin. The codec is the same WM8731 with the
same mixer controls, run at the same 96 kHz with the same 1024-frame
periods. The key jack is on the same two pins. The DSP, the keyer and
all five external interfaces need no change.

What does change is small and specific:

- a second T/R line (RX_LINE) with its own switching sequence;
- a fifth LPF line, on the pin maxibitx uses for EXT_PTT;
- a different LPF band plan, and LPF relays used only while
  transmitting;
- a different I2C bus number, shared with the zBitx's RP2040 front
  panel;
- a fresh calibration (crystal filter centre, BFO, per-band TX scale).

That is a few hundred lines. Every difference between the boards is
kept in one place, `radio_hw.c`, behind a board named explicitly in
`hw_settings.ini` (§5, §9).

**The real unknown is the Pi Zero 2W's CPU** (§4.1). maxibitx uses
3.1 ms of each 10.67 ms block on a Pi 4. A prediction for the Zero 2W
puts that uncomfortably close to the budget. It must be measured
before anything else is built on it, and there are known, contained
ways to cut the cost if it is too high.

**Two hardware facts need confirming before transmit is enabled**:
which LPF relay serves which band (§3.2), and whether anything about
the RP2040 front panel matters when nothing talks to it (§3.7).

## 2. How the zBitx code tells the boards apart

zbitx keeps one codebase for the sBitx and the zBitx. `hw_settings.ini`
carries `hw=4` on a zBitx (`sbitx_version = SBITX_V4`), and if the key
is missing the code assumes a zBitx. The ATtiny85 power/SWR bridge at
I2C 0x8 that maxibitx probes to tell the sBitx DE from the v2 is not
fitted to the zBitx, so maxibitx's probe would report "sBitx DE".

maxibitx reads the same `hw_settings.ini` format, but does not use
`hw=`, and does not default to either board. A line naming the board
is required, and maxibitx will not start without one (§9).

## 3. The hardware, signal by signal

### 3.1 Pins

zbitx uses wiringPi numbering. Converted to BCM, which is what
maxibitx's `gpio.c` uses:

| Signal | zBitx (wiringPi → BCM) | maxibitx on the sBitx (BCM) |
|---|---|---|
| TX_LINE, the PA / T/R relay | 4 → **23** | 23 |
| TX_POWER, held low | 27 → **16** | 16 |
| LPF_A | 5 → **24** | 24 |
| LPF_B | 6 → **25** | 25 |
| LPF_C | 10 → **8** | 8 |
| LPF_D | 11 → **7** | 7 |
| LPF_E | 26 → **12** | 12 is EXT_PTT |
| RX_LINE, connects the receiver | 16 → **15** | not used |
| Key ring (dash, mic PTT) | 7 → **4** | 4 |
| Key tip (dot) | 21 → **5** | 5 |

Two consequences:

- **No EXT_PTT on the zBitx.** BCM 12 drives LPF_E there. The zBitx
  board profile must not claim BCM 12 as EXT_PTT, and
  `ext_ptt_delay_ms` has no meaning on it.
- **RX_LINE is on BCM 15, the UART's receive pin.** The serial console
  and the UART must be off on the zBitx's Pi
  (`enable_uart=0`, no `console=serial0` in `cmdline.txt`), or the
  kernel holds that pin and the GPIO request fails.

The other GPIO lines zbitx's GTK code configures (the encoder pins) are
inputs for an sBitx front panel and play no part on the zBitx.

### 3.2 The low-pass filters

**What the zbitx code does.** In `set_lpf_40mhz()`, on a zBitx:

| Frequency | Relay |
|---|---|
| below 5.5 MHz | LPF_D |
| 5.5 to 10.5 MHz | LPF_C |
| 10.5 to 21.5 MHz | LPF_B (on the sBitx, only to 18.5 MHz) |
| 21.5 to 30 MHz | LPF_A |

LPF_E is claimed at startup and driven low, but **is never selected**.

The zbitx `dev` branch (commit `43522ef`, 2026-09-29) does exactly the
same, and so does the repository's first upload (`d8ecb71`,
2025-03-31): the 21.5 MHz split for the zBitx, LPF_E never selected,
and no LPF selected on a receive retune. The only LPF change since was
the T/R sequence's relay handling (`3258ad5`, 2026-04-25). So this
mapping has been on the air on zBitx radios since the code was first
published.

**What the found description says.** A description of the zBitx's LPF
bank (source not identified) gives five filters: 80 m; 40/30 m;
20/17 m; 15 m, "split off" from the 20/17 m filter; and 12/10 m.

Those disagree about 15 m. If the description is right, LPF_E is the
15 m filter and the code sends 15 m through the 20/17 m filter, whose
cutoff would be below 21 MHz. That would cost output power on 15 m and
load the PA. If the code is right, LPF_B on the zBitx covers 15 m and
LPF_E is something else, or not fitted. The code's long use on the air
favours it: a filter cutting off below 21 MHz would have shown up as
low output on 15 m. The description's wording reads like a generated
summary, so it should not be relied on alone.

**How to settle it**, in order of preference:

1. The zBitx schematic, or Jesse (W9JES), who maintains the zbitx code.
2. On the bench, before transmitting into an antenna: a low-power
   carrier into a dummy load on 21.2 MHz, once through LPF_B and once
   through LPF_E. A filter whose cutoff is below 21 MHz shows as a
   clear loss of output.

**The LPFs are not in the receive path on the zBitx.** zbitx's
`set_rx1()` selects an LPF on retune only when `sbitx_version < 4`.
Its zBitx T/R sequence selects one at the start of each transmission
and releases all of them on the way back to receive. So on the zBitx
the receiver reaches the antenna through RX_LINE, not through the LPF
bank. That makes the LPF question a transmit question only, and the
receive-only first step (§7) does not depend on it.

### 3.3 The T/R sequence

zbitx's `tr_switch_v4()`, reduced to its hardware steps:

| | zBitx, `devcwmod` | maxibitx on the sBitx today |
|---|---|---|
| **to TX** | RX_LINE low; all LPFs off; select the band's LPF; wait 10 ms; TX_LINE high | mute RX capture; clk1, clk2 to TX; EXT_PTT high; wait `ext_ptt_delay_ms`; TX_LINE high; exciter feed up |
| **to RX** | TX_LINE low; all LPFs off; wait 5 ms; RX_LINE high | exciter feed down; EXT_PTT low; wait 5 ms; TX_LINE low; clocks to RX; unmute capture |

The proposed zBitx sequence for maxibitx keeps maxibitx's order for the
parts the boards share, and takes zbitx's order for the relays:

**Receive to transmit**

1. Mute the receive input (the codec's line-input mute).
2. clk1 to `bfo_freq`, clk2 to the dial plus `xtal_filter_center`.
3. RX_LINE low: disconnect the receiver.
4. Select the band's LPF (all others off).
5. Wait 10 ms for the LPF relay to settle. This replaces the
   `ext_ptt_delay_ms` wait, which has nothing to wait for on the zBitx.
6. TX_LINE high: PA on.
7. Exciter feed (`Master` right) up.

**Transmit to receive**

1. Exciter feed down.
2. TX_LINE low: PA off.
3. All LPFs off.
4. Wait 5 ms.
5. Clocks to their receive values.
6. RX_LINE high: reconnect the receiver.
7. Unmute the receive input.

The 10 ms LPF settle is in the keying path in the same place the 20 ms
EXT_PTT delay is on the sBitx, so CW's first-element timing should be
no worse. `MAXIBITX_TR_TIMING` will show it.

Cycling the LPF relay on every transmission is what zbitx does and is
known to work. Whether the relay could simply stay selected through a
CW session is a later question, and one for the schematic.

### 3.4 The si5351 and the I2C bus

zbitx opens `/dev/i2c-3`; maxibitx has the si5351 on bus 22
(`SI5351_I2C_BUS` in `si5351v2.c`), the bus the sBitx's
`i2c-rtc-gpio` overlay creates. Bus numbers depend on the overlays, so
the bus should become a setting, `i2c_bus` in `hw_settings.ini`, with
each board's usual value as its default. `i2cdetect -l` and
`i2cdetect -y <bus>` on the zBitx will confirm it: the si5351 at 0x60
and the front panel at 0x0a should both answer there.

The si5351 driver needs no change: zbitx's `si5351v2.c` and maxibitx's
come from the same original, and only the bus differs.

### 3.5 The codec

zbitx sets the same controls on the same card (`hw:0`): `Input Mux`
to line in, `Line` on, `Capture`, `Master`, `Output Mixer HiFi`, and
`Output Mixer Mic Sidetone` off. The overlay is the same
`audioinjector-wm8731-audio`. maxibitx's `setup_audio_codec()` and its
use of the line-input mute during transmit should carry over unchanged.
The capture level (`RX_CAPTURE_GAIN_PERCENT`) may want a different value
on the zBitx; `rx_clip_check()` will say so.

### 3.6 The key jack and the microphone

The tip and ring are on BCM 5 and BCM 4, as on the sBitx, with pull-ups.
`key_input.c` needs nothing new. The microphone reaches the codec's
right input as on the sBitx, if the zBitx wires it the same way; that
is for the transmit step to confirm.

### 3.7 The RP2040 front panel

The zBitx's display, controls and power/SWR measurement live on an
RP2040 at I2C address 0x0a, on the same bus as the si5351. zbitx polls
it and writes display text to it. Headless maxibitx would never address
it, which should be harmless: it is an I2C slave and sends nothing
unless asked.

What is not known:

- whether the panel shows anything useful, or looks broken, when nothing
  talks to it;
- whether it handles anything the radio needs, such as the power button
  or a shutdown request to the Pi;
- what it reports. zbitx reads forward power, SWR and battery voltage
  from it as text, which maxibitx could later expose: today maxibitx
  has no transmit metering at all.

These are worth asking Jesse, and are checked by running the receive
step (§7) on the real radio.

### 3.8 Calibration

The zBitx needs its own values, measured on the board:

- `xtal_filter_center`. zbitx's default `bfo_freq` is 40,048,000 Hz
  against the sBitx's 40,035,000 Hz, so the zBitx's crystal filter may
  not be at 40,012,400 Hz. On receive, the filter's passband shows as a
  hump in the band noise on any panadapter; centring that hump on 0 Hz
  gives the centre.
- `bfo_freq`, about 22.6 kHz above that centre, as on the sBitx.
- `[tx_band]` scales, by the wattmeter procedure in
  [`tx_power_calibration.md`](tx_power_calibration.md).

**zbitx's own `[tx_band]` scales must not be reused.** They scale
zbitx's transmit path, not maxibitx's: zbitx's 80 m value is 0.00326
against maxibitx's 0.00085 on the sBitx, nearly four times the
amplitude. Copying a zbitx `hw_settings.ini` keeps its `cal` and `bfo_freq`
(it also needs the `sbitx_version` line, §9), but its `[tx_band]` entries have to be replaced, starting
well below full power.

## 4. The Pi Zero 2W

### 4.1 CPU

**Measured on the Pi 4:** the audio thread's processing takes 3.08 ms
of the 10.67 ms block (`ARCHITECTURE.md` §10 step 7).

**Where that time goes**, measured on an x86 development machine with
the shipped sources, per 1024-sample block:

| Stage | Time per block |
|---|---|
| `rx_audio_process()` as a whole | 480 to 550 µs |
| of which the stage-3 FFT filter | about 20 µs |
| of which the stage-3 elliptic filter | about 8 µs |
| anti-alias FIR, I and Q | 18 to 30 µs |
| `decim48k.c` | 8 µs |

So nearly all of the receive cost is **stage 1**, the 327-tap complex
FIR in `rx_audio.c`: 1.3 million double-precision multiply-adds per
block, 125 million per second. Its loop sums into one pair of
accumulators, so each tap waits for the previous one and the compiler
cannot vectorize it. That makes its speed depend on the core's
floating-point latency rather than its width.

**Prediction for the Zero 2W.** The Zero 2W's Cortex-A53 at 1 GHz
typically runs this kind of serial floating-point code two to three
times slower than the Pi 4's Cortex-A72 at 1.5 GHz. At three times,
3.1 ms becomes about 9 ms of the 10.67 ms budget. That would leave
little margin for the interfaces, Wi-Fi interrupts and the kernel.
maxibitx has never been run on a Zero 2W, so there is no measurement
yet. In favour: the full zbitx program, with a GTK interface and more
DSP, runs on the same board.

**Measure first.** `MAXIBITX_LOOP_TIMING=1` prints the per-block
process time on the real board, with no code change.

**If it is too slow**, in order of size:

1. **Stage 1 in single precision with several accumulators**, so the
   compiler can use NEON's four float lanes. A few lines in one
   function; expected to be the largest single gain. Single precision
   is ample for audio: its 24-bit mantissa is far below the noise.
2. **Run only the selected stage-3 filter**, as
   [`rx_narrow_filter_fft_vs_elliptic.md`](rx_narrow_filter_fft_vs_elliptic.md)
   §17 describes. A small saving, since stage 3 is cheap.
3. **Stage 1 as an FFT overlap-save filter**, with `fft_filter.c`,
   which already handles complex filters. Two 2048-point transforms a
   block instead of 1.3 million multiply-adds.
4. **More playback periods** (`MAXIBITX_PLAYBACK_PERIODS`), which buys
   margin against occasional overruns at the cost of keying latency.

Items 1 to 3 would also help the Pi 4, so they belong on `main`.

### 4.2 Memory

The Zero 2W has 512 MB. maxibitx's buffers, rings and FFTW plans are a
few megabytes, and the TCI send queues are capped at 8 MB per client.
Not a constraint.

### 4.3 Wi-Fi

The Zero 2W has 2.4 GHz Wi-Fi only, and no Ethernet. What each I/Q
stream needs, payload only:

| Stream | Rate |
|---|---|
| HPSDR Protocol 1, 24-bit I/Q at 96 kHz | about 6.3 Mbit/s |
| TCI I/Q, float32 at 96 kHz | about 6.1 Mbit/s |
| TCI I/Q at 48 kHz | about 3.1 Mbit/s |
| iq_stream, int16 at 96 kHz | about 3.1 Mbit/s per subscriber |
| TCI receive audio, float32 at 48 kHz | about 1.5 Mbit/s mono, 3.1 stereo |

One I/Q client should fit; several at once may not, and UDP streams
(HPSDR, iq_stream) drop packets rather than slow down. Prediction, to
be measured: the panel's spectrum plus one SDR application should be
fine; TCI's 48 kHz I/Q is the lighter choice for a panadapter over
Wi-Fi.

### 4.4 USB gadget

The Zero 2W's USB data port supports device mode (`dwc2`), so the
UAC2 sound card and the Kenwood CAT port should work as on the Pi 4.
What needs checking is whether the zBitx brings that port out to a
connector, and whether anything else uses it.

### 4.5 Operating system

The same build: `libasound`, `libfftw3f`, `make`'s `setcap`. On the
zBitx's Pi, in addition to what
[`07_build_and_deployment.md`](../07_build_and_deployment.md) lists:
the UART off (§3.1), and the I2C bus that carries the si5351 (§3.4).
Both 32-bit and 64-bit Raspberry Pi OS should build it; 64-bit is what
it has been developed on.

## 5. Design in maxibitx

**One codebase, one binary, the board named in `hw_settings.ini`.**

```
sbitx_version = SBITX_V3    # sBitx DE, v2 or v3
sbitx_version = SBITX_V4    # zBitx
```

`hw_settings.c` reads the line. If the file has no such line, or the
value is anything else, maxibitx prints what to add and exits before it
touches a GPIO line or a clock (§9).

**Every board difference in one place.** `radio_hw.c` holds a profile
for each board, data only:

```c
struct board {
  const char *name;
  int tx_enabled;         // 0: every transmit request refused
  int i2c_bus;            // the si5351's bus; i2c_bus= in the ini overrides
  int ext_ptt_pin;        // -1: none (zBitx)
  int rx_line_pin;        // -1: none (sBitx)
  int lpf_in_rx_path;     // sBitx 1, zBitx 0
  int lpf_settle_ms;      // zBitx 10, before TX_LINE
  struct { int below_hz; int pin; } lpf[6];  // band plan, 0-terminated
};
```

and is the only code that reads it. Everything else calls operations
that mean the same on every board, and never asks which board it is on:

| `radio_hw.c` operation | sBitx (SBITX_V3) | zBitx (SBITX_V4) |
|---|---|---|
| `radio_hw_init()` | claim TX_LINE, TX_POWER, EXT_PTT, LPF A-D | claim TX_LINE, TX_POWER, RX_LINE (high), LPF A-E |
| `radio_hw_i2c_bus()` | 22 | 3, to be confirmed (§3.4) |
| `radio_hw_tune(freq)` | select the band's LPF | nothing: the LPFs are not in the receive path |
| `radio_hw_tx_permitted()` | 1 | 0 until step 2 |
| `radio_hw_relays_tx(on)` | EXT_PTT; wait `ext_ptt_delay_ms`; TX_LINE | refuses until step 2; then §3.3's sequence |
| `radio_hw_tx_settle_ms()` | `ext_ptt_delay_ms` | the LPF settle, 10 ms |
| `radio_hw_board_name()` | "sBitx (DE, v2, v3)" | "zBitx" |

The callers then lose their own hardware knowledge:

| File | Change |
|---|---|
| `hw_settings.c/.h` | read `sbitx_version` (required) and `i2c_bus` (optional) |
| `radio_hw.c/.h` | the profiles and the operations above; the 0x8 probe removed |
| `radio.c` | `radio_tune_to()` calls `radio_hw_tune()`; `radio_tx_apply()` calls `radio_hw_relays_tx()` between its clock and codec steps; `radio_set_tx()` refuses when `radio_hw_tx_permitted()` is 0 |
| `si5351v2.c` | the bus from `radio_hw_i2c_bus()` |
| `sound.c` | the start-up T/R timing warning uses `radio_hw_tx_settle_ms()` |
| `maxibitx.c` | report the board |
| docs | `00_intro.md`, `01`, `03`, `07`, `08`, and this study |

The 0x8 probe goes because it no longer decides anything: on the sBitx
it only told the DE from the v2 in a log line, and on the zBitx it
would answer wrongly. The INA260 probe stays.

Nothing in the DSP, the keyer or the interfaces changes, and the sBitx
behaves exactly as now, once its `hw_settings.ini` names it. That is
what lets `zmax` merge back to `main`.

## 6. The `zmax` branch

A branch is right for the experimental phase: the zBitx work can be
tried on the radio without touching what runs on the sBitx. The aim
should still be to merge it back, with the board chosen at run time,
rather than to keep a lasting fork. Fixes to the DSP and the interfaces
then need making once, not twice.

In practice:

- Create `zmax` from `main` on GitHub; files for this work go there.
- Keep the zBitx changes to the board layer (§5), so `main`'s own
  changes merge into `zmax` cleanly. Merge `main` into `zmax` now and
  then.
- CPU fixes from §4.1 that help both boards go to `main` first.
- When transmit works on the zBitx and the sBitx still passes its
  checks, merge `zmax` into `main`.

## 7. Plan

**Step 0: prepare the Pi.** Raspberry Pi OS on the Zero 2W in the
zBitx; the codec overlay; the UART off; `i2cdetect` to find the
si5351's bus; build `main` unchanged to confirm the toolchain. Do not
run it yet: it would drive BCM 12 as EXT_PTT and leave RX_LINE
unclaimed.

**Step 1: receive only.** The board profiles (§5), with
`sbitx_version = SBITX_V4`:

- claim the zBitx pins, with RX_LINE high, TX_LINE low and all LPFs off;
- the si5351 on the configured bus;
- **transmit refused** on the zBitx, from every source, with a log line
  saying why (§9);
- the sBitx unchanged, apart from the required `sbitx_version` line.

Then measure:

- the per-block process time and any xruns, with `MAXIBITX_LOOP_TIMING`,
  over a long run, with each interface in use;
- the crystal filter centre, from the noise hump (§3.8);
- I/Q over Wi-Fi to SparkSDR, sdrOxide and the panel, and lost packets;
- the USB gadget, if the port is reachable: WSJT-X decoding FT8;
- whether the front panel minds being left alone.

If the process time is too high, apply §4.1's fixes before going on.

**Step 2: transmit.** Only after the LPF band plan is confirmed (§3.2),
and by changing the zBitx profile's `tx_enabled`:

- the zBitx T/R sequence (§3.3) and the confirmed LPF table;
- into a dummy load, with `max_power` set low and `[tx_band]` scales
  starting well below zbitx's values;
- the wattmeter calibration, band by band;
- CW timing with `MAXIBITX_TR_TIMING`, SSB from the microphone, and
  DIGITAL through the gadget or TCI.

**Step 3, optional: the front panel.** Read forward power, SWR and
battery voltage from the RP2040, if its protocol allows it, and expose
them through rigctld and TCI.

**Step 4: merge** `zmax` into `main` (§6).

## 8. Decisions and questions

1. **The LPF band plan**: from the schematic or Jesse, before step 2.
2. **The I2C bus** on the zBitx's Pi: `i2cdetect -l`.
3. **The USB data port**: is it brought out on the zBitx?
4. **The front panel**: does it control power or shutdown, and is it
   fine left idle?
5. **Fixes for the Zero 2W's CPU**: decided after step 1's measurement.

## 9. Decisions

Taken after the study was first written:

- **One codebase.** The sBitx and the zBitx run the same maxibitx, and
  the `zmax` work merges back to `main`.
- **The board is named in `hw_settings.ini`**, with
  `sbitx_version = SBITX_V4` for a zBitx and
  `sbitx_version = SBITX_V3` for an sBitx DE, v2 or v3.
- **No default.** If `hw_settings.ini` is missing, has no
  `sbitx_version` line, or names anything else, maxibitx refuses to
  start, saying which line to add. An existing sBitx installation needs
  `sbitx_version = SBITX_V3` added to its `hw_settings.ini` before
  this version will run. zbitx's own `hw=4` is not read.
- **Board differences in one place**: `radio_hw.c`'s profiles and
  operations (§5), with no board checks elsewhere.
- **No transmit on the zBitx until step 2 is complete.** Two guards,
  both in `radio_hw.c`: `radio_hw_tx_permitted()` makes `radio_set_tx()`
  refuse every transmit request, from every interface and the key, with
  a log line saying transmit is not enabled on the zBitx yet; and the
  zBitx's relay sequence does not exist until step 2, so even a request
  that got past the first guard could not raise TX_LINE.
