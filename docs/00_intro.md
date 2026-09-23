# 00 — maxibitx introduction

**Origin:** maxibitx started from
[MikeJohnshoy/minibitx](https://github.com/MikeJohnshoy/minibitx)'s
architecture, carried over as a step-1 skeleton. It has since grown an
all-mode (CW/SSB/DIGITAL) TX pipeline and an onboard CW/USB/LSB
demodulator on top of that same control/hardware layer. The design
rationale and build order for that work, step by step, is
[`ARCHITECTURE.md`](ARCHITECTURE.md); these docs describe the code as
it is now.

## What this is

maxibitx runs on the Raspberry Pi inside an sbitx radio, in place of the
`sbitx` software that shipped with it. It brings the hardware up and
then serves three kinds of client:

- SDR applications (SparkSDR and other HPSDR Protocol 1 apps), which
  receive baseband I/Q over UDP and do their own display,
  demodulation and filtering.
- Digital-mode and logging software such as WSJT-X, which sees the
  radio as a USB sound card plus a USB serial port (Kenwood CAT)
  through the Pi's USB gadget.
- Remote control: a rigctld-compatible TCP port, used by
  `tools/rigctl_panel.py` (a small desktop control panel with a
  spectrum display) and Hamlib clients.

The sBitx code base grew to support multiple operating modes with a powerful
user interface. My goal was to strip out everything but what was needed to make the
hardware work right, and leave as much as possible of the signal processing and user-interface
to the growing collection of high quality SDR applications.  

## What this is not

maxibitx has no waterfall and no user interface of its own; the
control panel and external apps provide those. The I/Q it streams is
not demodulated — an SDR app does that. Its one onboard demodulator,
`rx_audio.c`, produces CW, USB or LSB audio (DIGITAL demodulates as
USB) for the box's own speaker/headphone output and for the USB audio
gadget — see
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) and
[`dsp_design_notes/rx_audio_demod_design.md`](dsp_design_notes/rx_audio_demod_design.md).
Mode is one value owned by `radio.c` (`radio_get_mode()`/
`radio_set_mode()`), shared by every control surface, and it selects
both that demodulator and the TX path. There is no dependency on the
original sbitx codebase at runtime; minibitx was built by extracting
the minimum set of functions from sbitx needed to let an external SDR
app drive the hardware, and maxibitx runs stand-alone the same way.

## Status

Receive works: antenna to baseband I/Q, streamed over HPSDR Protocol 1
and to the control panel's spectrum display, tunable via HPSDR,
rigctld or Kenwood CAT. The onboard demodulator lets the box's own
speaker/headphone output be used with no external app running. FT8
over the USB audio gadget has been checked on air and decodes on par
with SparkSDR on the same I/Q. USB/LSB voice reception hasn't been
tested on air yet.

Transmit: CW from a straight key on the sbitx key input, with
Blackman-Harris shaping, runs through the shared FFT TX pipeline
(`tx_pipeline.c`) and has been checked on air — on frequency, image
suppression as predicted, and a flat ~5 W across the nine bands.
USB and LSB from the mic use the same pipeline and have been on the
air (power out, each on its correct side of the dial); their carrier
placement was measured with the test-tone generator and corrected.
DIGITAL from the USB audio gadget is confirmed on the air: a two-way
FT8 contact with WSJT-X on 2026-09-23, so the host's audio reached
another operator's decoder and their reply came back through the
receive chain. Voice power and ALC calibration are still to do
(`ARCHITECTURE.md` §10).

## How the rest of these docs are organized

Roughly bottom-up, following the signal and control paths through the
code:

- [`01_hardware_init_and_control.md`](01_hardware_init_and_control.md) —
  bringing up the GPIO lines, the si5351 oscillator, the I2C bus, and the
  WM8731 audio codec before any signal processing can happen.
- [`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) — the
  receive signal chain itself, antenna to baseband I/Q, plus the onboard
  CW/USB/LSB demodulator that taps the same I/Q for the local speaker
  and the USB audio gadget.
- [`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md) — the
  transmit signal chain: each mode's audio source, keying/PTT,
  `tx_pipeline.c`, the analog mixers, and power levels.
- [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md)
  — how the external interfaces are implemented.
- [`05_process_and_threading_model.md`](05_process_and_threading_model.md)
  — how `main()` brings all of the above up, and the thread structure
  that keeps it running.
- [`06_api.md`](06_api.md) — the reference for external software: every
  interface's commands and formats, the shared radio state, WSJT-X
  setup, and what isn't available yet.
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — maxibitx's own design
  rationale and build order: why this is a fresh repo rather than a
  minibitx branch, the shared FFT TX/RX pipeline decision, and what's
  still open.
- [`dsp_design_notes/`](dsp_design_notes/) — standalone design write-ups,
  measurements and debugging history for the DSP code.
- [`07_build_and_deployment.md`](07_build_and_deployment.md) — building
  maxibitx and the kernel/OS pieces it depends on.
- [`08_troubleshooting_and_bringup.md`](08_troubleshooting_and_bringup.md)
  — hardware bring-up gotchas that don't fit neatly elsewhere.
- [`09_faq.md`](09_faq.md) — common questions, each pointing at the doc
  that carries the full story: where the project came from, why the
  design is the way it is, how ALC works, why there are four external
  interfaces, and what isn't done.
- [`12_simple_cw_transceiver.md`](12_simple_cw_transceiver.md) — a
  simple CW transceiver built around maxibitx (stub).

Documents in the `0x` range describe how the code works internally,
except [`06_api.md`](06_api.md), the reference for external software;
documents numbered `10` and up describe how to use it;
`ARCHITECTURE.md` records the design decisions and build order.
