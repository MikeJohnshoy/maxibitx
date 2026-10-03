# 01 — sbitx hardware: initialization and control

This covers everything that has to be brought up before any signal ever
flows: GPIO lines, the si5351 clock generator and the I2C bus it rides
on, and the WM8731 audio codec. It stops at the point where the receive
signal chain can actually start moving samples — see
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) for what
happens to those samples once they arrive.

## Startup order, in `main()` (`maxibitx.c`)

1. `hw_settings_load()` — `data/hw_settings.ini`, including the
   `sbitx_version` line that names the board
2. `radio_hw_select_board()` — the board profile; maxibitx exits here if
   the file doesn't name a board it knows
   ([Board selection](#board-selection), below)
3. `radio_hw_gpio_init()` — the board's GPIO lines and RX-safe idle
   state (below)
4. `si5351bx_init(radio_hw_i2c_bus())` / `si5351bx_setfreq(1, ...)` /
   `si5351_reset()` — oscillator bring-up on the board's I2C bus (below)
5. `radio_hw_ina260_configure()` — power monitor, probed right after the
   si5351 bring-up above since it shares its I2C bus (below)
6. `vfo_init_phase_table()` / `vfo_start()` / `radio_tune_to()` —
   software RX VFO and initial tuning (covered in
   [`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md))
7. `cw_init()` / `key_input_start()` — the keyed tone, then the key jack
   and its input thread ([The key jack](#the-key-jack), below)
8. `rx_audio_init()` — the onboard demodulator
9. Networking and control surfaces — Hamlib/rigctld, TCI, HPSDR,
   iq_stream, USB gadget, CAT (covered in
   [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md))
10. `setup_audio_codec()` / `sound_thread_start()` — WM8731 codec and
    capture stream (below)

Each step above prints one console line reporting its own result, in a
consistent `init: ...` format, ending with `maxiBitx: radio hardware
initialization complete, ready to serve!` once every step has run. See
[`05_process_and_threading_model.md`](05_process_and_threading_model.md)
for what the console reports after that point.

The ordering matters for one reason in particular: GPIO init happens
before anything that could conceivably key the transmitter exists yet,
and only once the board is known, so no pin is ever driven with another
board's meaning.

## GPIO setup and the RX-safe idle state

`radio_hw_gpio_init()` (`radio_hw.c`) requests the selected board's
output lines through `gpio.c`'s wrapper around the Linux GPIO
character-device API (`/dev/gpiochip0`), each driven to its receive
state as part of its request:

```c
err |= claim(board->tx_line_pin, 0, "maxibitx-tx_line");
err |= claim(board->tx_power_pin, 0, "maxibitx-tx_power");
err |= claim(board->ext_ptt_pin, 0, "maxibitx-ext_ptt"); // -1: none, skipped
err |= claim(board->rx_line_pin, 1, "maxibitx-rx_line"); // -1: none, skipped
for (int i = 0; board->lpf_pins[i] >= 0; i++)
  err |= claim(board->lpf_pins[i], 0, "maxibitx-lpf");
```

| Line | sBitx (BCM) | zBitx (BCM) | Receive state |
|---|---|---|---|
| `TX_LINE` | 23 | 23 | low |
| `TX_POWER` | 16 | 16 | low |
| `EXT_PTT` | 12 | — | low |
| `RX_LINE` | — | 15 | high (receiver connected) |
| LPF relays | 24, 25, 8, 7 | 24, 25, 8, 7 | all off |
| BCM 12, unused | — | 12 | low |

On the zBitx, BCM 12 is zbitx's `LPF_E`, a line with no filter behind
it, held low as zbitx does, and `RX_LINE` is BCM 15, the UART's RXD pin, so the serial
console and UART must be disabled for maxibitx to claim it. The log
line `init: GPIO configured for the <board>: ...` says what was claimed.

The key jack's two contacts are inputs, claimed separately by
`key_input_start()` (`key_input.c`) in one request with pull-ups and edge
events — see [The key jack](#the-key-jack) below.

Unlike the old wiringPi-based version, there's no separate "set the pin
mode, then write it low" sequence — each `gpio_request_output()` call
claims the line and drives it to its initial value as one
atomic kernel request, so there's no window where a line briefly holds
whatever power-on/pinctrl default it had before minibitx touched it.

`EXT_PTT` (where there is one) and `TX_LINE` low is the T/R relay's
RX-idle state. Because this runs before the si5351, the VFO, the network threads, or either
control surface (Hamlib, HPSDR's MOX handling) exist, there is no code
path in the process's lifetime where the radio could power on
transmitting — the relay and PTT lines are guaranteed low before
anything capable of calling `radio_set_tx()` is even initialized.

`TX_POWER` is also set low at boot; its exact purpose is inherited from
sbitx and unconfirmed here (see `radio_hw.c`).

### Migrating off wiringPi: BCM pin mapping

minibitx used to drive these pins through wiringPi, which numbers pins
in its own scheme rather than the SoC's BCM GPIO numbers. Since
wiringPi is unmaintained upstream (and has no Pi 5 support), `radio_hw.c`
was moved onto `gpio.c`'s direct character-device API, which takes BCM
offsets — so every pin number (the board profiles in `radio_hw.c`, and
the key jack's in `radio_hw.h`) is a BCM number. The sBitx mapping below
was derived from a `gpio readall` capture on real sBitx v2 hardware (Pi 4, bench,
2026-09) with nothing running at the time — informative for pin
*identity* and *direction* (a pin already latched as `OUT` by a previous
run confirms which physical pins get driven as outputs, regardless of
whether anything is running right now), but not for the specific logic
levels captured, which were just whatever a previous run happened to
leave behind rather than a live read of the radio's current state:

| Name | wiringPi # (old) | BCM # (current) | Physical pin | Role |
|---|---|---|---|---|
| `TX_LINE` | 4 | 23 | 16 | T/R relay control |
| `TX_POWER` | 27 | 16 | 36 | set low at boot, purpose unconfirmed |
| `EXT_PTT` | 26 | 12 | 32 | external PTT |
| `LPF_A` | 5 | 24 | 18 | LPF band select |
| `LPF_B` | 6 | 25 | 22 | LPF band select |
| `LPF_C` | 10 | 8 | 24 | LPF band select (shares SPI0's CE0 pin, unused as SPI here) |
| `LPF_D` | 11 | 7 | 26 | LPF band select (shares SPI0's CE1 pin, unused as SPI here) |
| `KEY_RING_GPIO` | 7 | 4 | 7 | key jack ring: dash, straight key, mic PTT; pull-up, active low |
| `KEY_TIP_GPIO` | 21 | 5 | 29 | key jack tip: dot, straight key; pull-up, active low |

If this ever needs porting to different hardware (a different Pi model,
a different board layout), re-derive this table the same way — from a
real `gpio readall` (or equivalent) on that specific board — rather than
assuming these BCM numbers carry over.

## The key jack

The key jack is a stereo socket: tip on BCM 5, ring on BCM 4, sleeve on
ground, each contact pulled up and closing to ground. Measured on a DE
board with a mono plug in the jack and the key open, maxibitx stopped:

```
$ gpioget -c gpiochip0 -b pull-up 4 5      # libgpiod 2.x
"4"=inactive "5"=active
```

The mono plug's sleeve grounds the ring, so the line reading `inactive` is
the ring. (libgpiod 1.x: `gpioget gpiochip0 4 5`, which prints `0 1`.)

`key_input_start()` claims both in one request with edge events on both
edges, so the kernel queues every edge with a `CLOCK_MONOTONIC` timestamp
taken in its interrupt handler, and a thread of its own (`SCHED_FIFO`, one
step below the audio thread) sleeps in `ppoll()` until one arrives. What a
contact means:

| Mode | Tip (BCM 5) | Ring (BCM 4) |
|---|---|---|
| CW, CWR — straight key | straight key | straight key |
| CW, CWR — bug | dot (automatic) | dash (manual) |
| CW, CWR — ultimatic, iambic A/B | dot | dash |
| USB, LSB | — | mic PTT (sbitx's `PTT` line) |
| DIGITAL | — | — |

The keyer mode and speed are `rigctld` `U KEYER` / `L KEYSPD` (or CAT
`KS`); `U PADREV 1` swaps the paddle roles (tip = dash, ring = dot), which
has no effect on a straight key. Closing either contact also stops any
text the keyer is sending (rigctld `b`, CAT `KY`). Either contact keys the straight key, so
a stereo plug wired to either works, and a mono plug works too: at
start-up a contact found closed is ignored from the first moment, and if it
stays closed for 250 ms the console says it is being treated as a ring
grounded by a mono plug. The contact comes back into use when it opens —
the plug has been changed. A mono plug inserted while maxibitx is running
is indistinguishable from a key held down, so restart after changing to
one. Debounce is `key_debounce_ms` in `hw_settings.ini` (3 ms if absent).
How the edges reach the transmitted signal:
[`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md) and
[`cw_keyer_design_study.md`](dsp_design_notes/cw_keyer_design_study.md) §16.

## Board selection

maxibitx runs on two radio boards, and every difference between them
lives in `radio_hw.c`, as one profile per board (`struct board`,
`boards[]`). `data/hw_settings.ini` names the board with a top-level
line, above the first `[section]`:

| Line | Board |
|---|---|
| `sbitx_version = SBITX_V3` | sBitx DE, v2 or v3 |
| `sbitx_version = SBITX_V4` | zBitx |

`main()` passes the value to `radio_hw_select_board()` straight after
`hw_settings_load()`. If the file is missing, has no such line, or names
anything else (the match is exact, case included), maxibitx prints the
valid lines and exits with status 1, before it touches any GPIO line or
clock. There is no default, so a board is never driven with another
board's pins. zbitx's own `hw=` key is not read; if it is present, or a
top-level key looks like a misspelling of `sbitx_version`, the log says
so. On success the log reads `init: radio board: <name>`, with
` - receive only, transmit is not enabled on this board` added on a
board that may not transmit.

The rest of the code asks nothing about the board; it calls
`radio_hw_tune()`, `radio_hw_tx_permitted()`, `radio_hw_relays_tx()`,
`radio_hw_tx_settle_ms()` and `radio_hw_i2c_bus()`, which mean the same
on every board. The zBitx profile is receive only: `radio_hw_tx_permitted()`
is 0, so `radio_set_tx()` refuses every transmit request, and its relay
half of the T/R sequence refuses to go to transmit as well. The design:
[`zbitx_port_study.md`](dsp_design_notes/zbitx_port_study.md) §5 and §9.

## LPF bank switching

`radio_tune_to()` calls `radio_hw_tune(frequency)` on every retune — see
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md). On the
sBitx, whose LPFs are in the receive path, it selects the band's relay
from the board's plan, all others off; on the zBitx, whose LPFs carry
only the transmitter, it does nothing. Each board's plan:

| Frequency | Bands | sBitx | zBitx |
|---|---|---|---|
| below 5.5 MHz | 80, 60 m | `LPF_D` (BCM 7) | `LPF_D` (BCM 7) |
| 5.5 to 10.5 MHz | 40, 30 m | `LPF_C` (BCM 8) | `LPF_C` (BCM 8) |
| 10.5 to 18.5 MHz | 20, 17 m | `LPF_B` (BCM 25) | `LPF_B` (BCM 25) |
| 18.5 to 21.5 MHz | 15 m | `LPF_A` (BCM 24) | `LPF_B` (BCM 25) |
| 21.5 to 30 MHz | 12, 10 m | `LPF_A` (BCM 24) | `LPF_A` (BCM 24) |
| 30 MHz and above | | none | none |

Both boards have these four filters and no others.

Selection is a no-op if the new relay matches the last one (tracked in a
static `prev_lpf`), so retuning within a band doesn't chatter the relays.

## INA260 power monitor

`radio_hw_ina260_configure()` writes `0x6127` (continuous mode, default
averaging) to the INA260's config register at I2C address `0x40`. It's
called once, from `main()`, right after the si5351 bring-up below —
`init: INA260 power monitor configured`, or a non-fatal
`init: INA260 power monitor not responding, continuing without it` if
the write fails. `read_voltage_current()` then reads the voltage and
current registers (1.25 mV/LSB and 1.25 mA/LSB respectively) on demand,
treating an all-ones current reading as out-of-range/invalid rather than
a real value. Not currently called from anywhere — available for future
status/telemetry reporting, not for any control decision. See
[`05_process_and_threading_model.md`](05_process_and_threading_model.md)
for how minibitx currently reports operational state instead (per-command
console echoes, not a periodic status line).

## si5351 oscillator and I2C bus

The si5351 generates both mixer LOs used in the RX chain (see
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) for what
each clock actually does). `si5351bx_init(bus)` (`si5351v2.c`) powers
down all three clocks and brings up the I2C connection on the bus it is
given, `radio_hw_i2c_bus()`; `main()`
then explicitly starts `clk1` at its RX value
(`xtal_filter_center + RX_IF_FREQ_HZ`) before calling `si5351_reset()`.
Unlike `clk2` (which `radio_tune_to()` sweeps constantly) or the older
single-value scheme this replaced, `clk1` isn't touched again until the
first TX burst — `radio_tx_apply()` (`radio.c`) retunes it to `bfo_freq`
for the duration of TX and restores this RX value the moment TX ends;
see [`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md) and
[`dsp_design_notes/antialias_filter_design.md`](dsp_design_notes/antialias_filter_design.md)
§3 for why RX and TX need different `clk1` values in the first place.

Every clock is derived from the si5351's reference oscillator, a nominal
25 MHz TCXO. `hw_settings.ini`'s `cal` key gives its measured frequency,
in sbitx's own `[tcxo]` section (accepted at the top level too);
`hw_settings_load()` passes it to `si5351_set_calibration()` before any
clock is set, and with it left out the nominal 25,000,000 is used. An
error here moves transmit and receive in *opposite* directions, both in
proportion to the operating frequency (1 ppm is 7 Hz at 7 MHz, 15 Hz at
15 MHz). This board measures at its nominal 25,000,000, checked against
WWV on receive. Measuring it:
[`dsp_design_notes/tx_test_tones_and_alc.md`](dsp_design_notes/tx_test_tones_and_alc.md),
"Frequency calibration".

The si5351 is at `SI5351_ADDR` (`0x60`) on the bus `radio_hw_i2c_bus()`
returns: the board profile's usual bus, unless `hw_settings.ini` has a
top-level `i2c_bus` key, which overrides it. On the sBitx that is bus
22, the physical bus it shares with the board's RTC via the
`i2c-rtc-gpio` device tree overlay (`i2cdetect -y 22` shows a device at
`0x60`); on the zBitx it is bus 3. The startup log names it
(`init: si5351 oscillator ready on I2C bus N, ...`). It's a
Linux-assigned bus number, not a fixed hardware address, so it can
change across kernel/config updates; `i2c_bus` is the fix without a
rebuild. See
[`08_troubleshooting_and_bringup.md`](08_troubleshooting_and_bringup.md)
if the si5351 ever stops responding after an OS update.

`i2c.c` wraps this as a thin layer over `/dev/i2c-N` and the standard
SMBus ioctls (byte read/write, block read/write) — nothing sbitx- or
si5351-specific lives there.

Because the sBitx's bus is bit-banged by the kernel rather than served
by a hardware peripheral, I2C traffic there is CPU time. One `si5351bx_setfreq()` is
17 separate transactions, and a T/R transition writes two clocks. What that
costs, why none of it reaches the real-time audio path, and which half of
it is redundant when RIT is zero:
[`dsp_design_notes/cw_breakin_and_i2c_costs.md`](dsp_design_notes/cw_breakin_and_i2c_costs.md).

## WM8731 audio codec bring-up

`setup_audio_codec()` (`sound.c`) configures the codec entirely through
ALSA mixer controls — no direct I2C register access from minibitx itself
(the kernel's `wm8731` driver, brought up via the
`dtoverlay=audioinjector-wm8731-audio` device tree overlay, does those
writes on minibitx's behalf):

```c
sound_mixer("hw:0", "Input Mux", 0);                       // 'Line In'
sound_mixer("hw:0", "Line", RX_LINE_INPUT_ON);            // on/off switch, not a gain
sound_mixer("hw:0", "Capture", RX_CAPTURE_GAIN_PERCENT);  // the real analog gain, 70%
sound_mixer("hw:0", "Mic", MIC_CAPTURE_GAIN_PERCENT);     // 50% - possibly inert, below
sound_set_local_monitor(LOCAL_SPEAKER_GAIN_PERCENT);      // 'Master' LEFT = local speaker, 100%
sound_mixer_channel("hw:0", "Master", SND_MIXER_SCHN_FRONT_RIGHT, 0); // 'Master' RIGHT = exciter, muted
sound_mixer("hw:0", "Output Mixer HiFi", 1);
sound_mixer("hw:0", "Output Mixer Line Bypass", 0);
sound_mixer("hw:0", "Output Mixer Mic Sidetone", 0);
```

A few things about this codec that were learned the hard way:

- **`'Line'` is a switch, `'Capture'` is the gain.** `'Line'` looked like
  a gain control at first glance, but `amixer -c 0 sget 'Line'` shows a
  plain on/off switch (`cswitch` only). The real analog gain ahead of the
  ADC is the separate `'Capture'` control - see
  [`dsp_design_notes/rx_gain_and_level_calibration.md`](dsp_design_notes/rx_gain_and_level_calibration.md)
  §3 for the full story.
- **`sound_mixer()` must set every capability an element has.** It used
  to be an else-if chain (switch, *or* volume, *or* enum). An ALSA simple
  element can have a mute switch and a volume register under one name -
  `'Master'` does - so only its switch was ever toggled, and its volume
  stayed wherever the codec's power-on reset left it, whatever percent
  was asked for. Now each capability is set independently, and a missing
  element name is logged rather than silently ignored
  (`sound_mixer_dump()` prints what an element really supports).
- **`'Master'` LEFT and RIGHT go to different places.** LEFT drives the
  local speaker/headphone amp; RIGHT drives the mainboard's diode mixer,
  i.e. the TX exciter feed. They used to share one value, so every TX/RX
  transition clobbered whichever purpose wasn't active. Now LEFT is set
  once at startup and RIGHT is raised only during TX
  (`sound_set_tx_drive()`, `radio.c`'s `TX_MASTER_VOL`) - see
  [`dsp_design_notes/rx_audio_demod_design.md`](dsp_design_notes/rx_audio_demod_design.md)
  §9.
- **LEFT runs at 100%.** `LOCAL_SPEAKER_GAIN_PERCENT` started at 70 and
  was raised to 100 after an on-air report (2026-09) that "100%" volume
  was still too quiet. Running this analog stage wide open costs nothing:
  the digital headroom lives upstream in `rx_audio.c` (its AGC target
  sits at ~25% of the int32 clamp). Listening volume is `rx_audio.c`'s
  digital `rx_volume`, which was itself later rescaled (`RX_VOLUME_MAX`)
  once 100% here made it too loud.
- **The mic comes in on `'Capture'` RIGHT, probably not via `'Mic'`.**
  `'Input Mux'` is fixed to `'Line'`, so the physical mic most likely
  reaches the ADC on Line-In RIGHT, and `'Mic'` (the codec's own mic
  preamp path) may do nothing on this board - unconfirmed; check with
  `amixer -c 0 sget 'Mic'`. That's why the TX capture mute
  (`sound_set_rx_capture()`) touches the LEFT channel only - the
  line-input mute, `'Line'` LEFT: muting both channels silenced USB/LSB
  TX audio on the first on-air SSB test (`ARCHITECTURE.md` §10 step 8).
  Why it is the switch and not `'Capture'`'s gain:
  [`rx_gain_and_level_calibration.md`](dsp_design_notes/rx_gain_and_level_calibration.md) §8.

Once the mixer is configured, `sound_thread_start("hw:0,0")` opens the
ALSA capture (and playback) PCM devices at the fixed 96 kHz sample rate
and starts the audio thread that repeatedly calls `sound_process()`.
That call is the boundary this document stops at: by the time it
returns, raw IF samples are already flowing in from an open, configured
capture stream. What happens to those samples from there —
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md).
