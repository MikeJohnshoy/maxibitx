# 01 — sbitx hardware: initialization and control

This covers everything that has to be brought up before any signal ever
flows: GPIO lines, the si5351 clock generator and the I2C bus it rides
on, and the WM8731 audio codec. It stops at the point where the receive
signal chain can actually start moving samples — see
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) for what
happens to those samples once they arrive.

## Startup order, in `main()` (`minibitx.c`)

1. `radio_hw_gpio_init()` — GPIO lines and RX-safe idle state (below)
2. `si5351bx_init()` / `si5351bx_setfreq(1, bfo_freq)` / `si5351_reset()`
   — oscillator bring-up (below)
3. `radio_hw_detect_version()` / `radio_hw_ina260_configure()` — board
   revision and power monitor, probed right after the si5351 bring-up
   above since they share its I2C bus (below)
4. `vfo_init_phase_table()` / `vfo_start()` / `radio_tune_to()` —
   software RX VFO and initial tuning (covered in
   [`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md))
5. Networking and control surfaces — HPSDR, Hamlib/rigctld, USB gadget
   (covered in
   [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md))

Each step above prints one console line reporting its own result, in a
consistent `init: ...` format, ending with `minibitx: radio hardware
initialization complete` once every step has run. See
[`05_process_and_threading_model.md`](05_process_and_threading_model.md)
for what the console reports after that point.
5. `setup_audio_codec()` / `sound_thread_start()` — WM8731 codec and
   capture stream (below)

The ordering matters for one reason in particular: GPIO init happens
*first*, before anything that could conceivably key the transmitter
exists yet.

## GPIO setup and the RX-safe idle state

`radio_hw_gpio_init()` (`radio_hw.c`) requests `TX_LINE`, `TX_POWER`,
`EXT_PTT`, and the four LPF select lines (`LPF_A`–`LPF_D`) as outputs
through `gpio.c`'s wrapper around the Linux GPIO character-device API
(`/dev/gpiochip0`), and `CW_KEY` as an input with its pull-up enabled:

```c
line_tx_line  = gpio_request_output(TX_LINE,  0, "maxibitx-tx_line");
line_tx_power = gpio_request_output(TX_POWER, 0, "maxibitx-tx_power");
line_ext_ptt  = gpio_request_output(EXT_PTT,  0, "maxibitx-ext_ptt");
line_lpf_a    = gpio_request_output(LPF_A,    0, "maxibitx-lpf_a");
line_lpf_b    = gpio_request_output(LPF_B,    0, "maxibitx-lpf_b");
line_lpf_c    = gpio_request_output(LPF_C,    0, "maxibitx-lpf_c");
line_lpf_d    = gpio_request_output(LPF_D,    0, "maxibitx-lpf_d");
line_cw_key   = gpio_request_input(CW_KEY, 1, "maxibitx-cw_key");
```

Unlike the old wiringPi-based version, there's no separate "set the pin
mode, then write it low" sequence — each `gpio_request_output()` call
claims the line and drives it to its initial value (`0`, here) as one
atomic kernel request, so there's no window where a line briefly holds
whatever power-on/pinctrl default it had before minibitx touched it.

`EXT_PTT` and `TX_LINE` low is the T/R relay's RX-idle state. Because
this runs before the si5351, the VFO, the network threads, or either
control surface (Hamlib, HPSDR's MOX handling) exist, there is no code
path in the process's lifetime where the radio could power on
transmitting — the relay and PTT lines are guaranteed low before
anything capable of calling `radio_set_tx()` is even initialized.

`TX_POWER` is also set low at boot; its exact purpose is inherited from
sbitx and unconfirmed here (see `radio_hw.h`).

### Migrating off wiringPi: BCM pin mapping

minibitx used to drive these pins through wiringPi, which numbers pins
in its own scheme rather than the SoC's BCM GPIO numbers. Since
wiringPi is unmaintained upstream (and has no Pi 5 support), `radio_hw.c`
was moved onto `gpio.c`'s direct character-device API, which takes BCM
offsets — so every pin constant in `radio_hw.h` changed from a wiringPi
number to the corresponding BCM number. The mapping below was derived
from a `gpio readall` capture on real sBitx v2 hardware (Pi 4, bench,
2026-09) with nothing running at the time — informative for pin
*identity* and *direction* (a pin already latched as `OUT` by a previous
run confirms which physical pins get driven as outputs, regardless of
whether anything is running right now), but not for the specific logic
levels captured, which were just whatever a previous run happened to
leave behind rather than a live read of the radio's current state:

| `radio_hw.h` name | wiringPi # (old) | BCM # (current) | Physical pin | Role |
|---|---|---|---|---|
| `TX_LINE` | 4 | 23 | 16 | T/R relay control |
| `TX_POWER` | 27 | 16 | 36 | set low at boot, purpose unconfirmed |
| `EXT_PTT` | 26 | 12 | 32 | external PTT |
| `LPF_A` | 5 | 24 | 18 | LPF band select |
| `LPF_B` | 6 | 25 | 22 | LPF band select |
| `LPF_C` | 10 | 8 | 24 | LPF band select (shares SPI0's CE0 pin, unused as SPI here) |
| `LPF_D` | 11 | 7 | 26 | LPF band select (shares SPI0's CE1 pin, unused as SPI here) |
| `CW_KEY` | 7 | 4 | 7 | straight key input, pull-up, active low |

If this ever needs porting to different hardware (a different Pi model,
a different board layout), re-derive this table the same way — from a
real `gpio readall` (or equivalent) on that specific board — rather than
assuming these BCM numbers carry over.

## Board revision detection

`radio_hw_detect_version()` probes I2C address `0x8` with a 4-byte block
read. If that read fails, it reports `SBITX_DE` (original sbitx, no
power/SWR bridge board); if it succeeds, `SBITX_V2` (v2-and-later
hardware, power/SWR bridge present). It's called once, from `main()`,
right after the si5351/I2C bus comes up — its result is logged
(`init: board revision detected: ...`) but nothing else in the codebase
branches on it yet; it exists as a hook for hardware-revision-dependent
behavior to attach to later.

## LPF bank switching

`set_lpf_40mhz(frequency)` selects one of four low-pass filter relays
based on the tuned frequency:

| Frequency | Relay |
|---|---|
| < 5.5 MHz | `LPF_D` |
| < 10.5 MHz | `LPF_C` |
| < 18.5 MHz | `LPF_B` |
| < 30 MHz | `LPF_A` |

It's a no-op if the new selection matches the last one (tracked in a
static `prev_lpf`), so retuning within a band doesn't chatter the relays.
This is called from `radio_tune_to()` — see
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) — not
independently.

## INA260 power monitor

`radio_hw_ina260_configure()` writes `0x6127` (continuous mode, default
averaging) to the INA260's config register at I2C address `0x40`. It's
called once, from `main()`, alongside board revision detection above —
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
each clock actually does). `si5351bx_init()` (`si5351v2.c`) powers down
all three clocks and brings up the I2C connection it needs; `main()`
then explicitly starts `clk1` at its RX value
(`xtal_filter_center + RX_IF_FREQ_HZ`) before calling `si5351_reset()`.
Unlike `clk2` (which `radio_tune_to()` sweeps constantly) or the older
single-value scheme this replaced, `clk1` isn't touched again until the
first TX burst — `radio_tx_apply()` (`radio.c`) retunes it to `bfo_freq`
for the duration of TX and restores this RX value the moment TX ends;
see [`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md) and
[`dsp_design_notes/antialias_filter_design.md`](dsp_design_notes/antialias_filter_design.md)
§3 for why RX and TX need different `clk1` values in the first place.

The si5351 sits on I2C bus 22 (`SI5351_I2C_BUS` in `si5351v2.c`), sharing
the physical bus with the board's RTC via the `i2c-rtc-gpio` device tree
overlay. That bus number came from `i2cdetect -y 22` showing a device at
`SI5351_ADDR` (`0x60`) — not GPIO23/22 as originally assumed. It's a
Linux-assigned bus number, not a fixed hardware address, so it can
change across kernel/config updates; see
[`08_troubleshooting_and_bringup.md`](08_troubleshooting_and_bringup.md)
if the si5351 ever stops responding after an OS update.

`i2c.c` wraps this as a thin layer over `/dev/i2c-N` and the standard
SMBus ioctls (byte read/write, block read/write) — nothing sbitx- or
si5351-specific lives there.

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
  (`sound_set_rx_capture()`) touches `'Capture'` LEFT only: muting both
  channels silenced USB/LSB TX audio on the first on-air SSB test
  (`ARCHITECTURE.md` §10 step 8).

Once the mixer is configured, `sound_thread_start("hw:0,0")` opens the
ALSA capture (and playback) PCM devices at the fixed 96 kHz sample rate
and starts the audio thread that repeatedly calls `sound_process()`.
That call is the boundary this document stops at: by the time it
returns, raw IF samples are already flowing in from an open, configured
capture stream. What happens to those samples from there —
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md).
