# maxibitx

A headless, all-mode (CW, USB, LSB, DIGITAL) radio daemon for the
Raspberry Pi inside an [sBitx](https://github.com/afarhan/sbitx). It
brings up the radio hardware, runs the receive and transmit signal
chains, and serves external software. There's no user interface in
the process: SDR apps, WSJT-X, FLRig and a small desktop control panel
provide the display and controls, over the network or USB.

## Where it came from

**sbitx** is Ashhar Farhan's (VU2ESE) software for his radio. His software was forked
to https://github.com/drexjj/sbitx/tree/main and greatly refined and extended.  **sbitx**
now has many new features, and the codebase and file size has grown. `sbitx.c` and`sbitx_gtk.c`
alone are more than 3,000 and 12,000 lines respectively, with the GTK user interface, DSP and
hardware initialization and control intertwined. That coupling has real costs.
It can be difficult for a new developer to find and understand how things work,
and changes carry a risk of unintended impacts.

**minibitx** asked how little code it takes to run the sBitx hardware
well. It pulled out only what the hardware needs and refined each
piece. wiringPi gave way to the kernel's GPIO character device,
bit-banged I2C to the kernel's I2C driver, and the rest of the DSP and
all of the UI were left to mature external SDR apps.

**maxibitx** starts from minibitx's hardware and control layer and adds
an all-mode transmitter pipeline and an onboard demodulator - without
bringing the coupling back. Every job lives in its own small file,
and no UI runs in the process. Anything an external display or controller needs
goes through a narrow, documented interface
([`docs/06_api.md`](docs/06_api.md)).

The whole daemon is about 6,300 lines of C in 21 files; a big part of that is 
in the external interfaces.  The largest is
the USB gadget providing an audio connection and CAT control interface (~ 1,300 lines).

| Job | Files |
|---|---|
| Startup and shutdown | `maxibitx.c` |
| Radio state: tuning, RIT, mode, T/R switching | `radio.c` |
| Hardware: GPIO, I2C, si5351 clocks, LPF relays, board calibration | `radio_hw.c`, `gpio.c`, `i2c.c`, `si5351v2.c`, `hw_settings.c` |
| Real-time audio thread and WM8731 codec | `sound.c` |
| RX: I/Q mixing, anti-aliasing, demodulation | `vfo.c`, `antialias.c`, `rx_audio.c`, `rx_filter.c` |
| TX: CW keying, shared FFT TX pipeline | `cw.c`, `tx_pipeline.c`, `fft_filter.c` |
| USB audio rate conversion | `decim48k.c`, `upsample48k.c` |
| External interfaces | `interfaces/hamlib.c` (rigctld), `interfaces/hpsdr_p1.c` (HPSDR Protocol 1), `interfaces/usb_gadget.c` (USB audio + Kenwood CAT), `interfaces/iq_stream.c` (I/Q for the control panel) |

The DSP is bench-tested on its own (`make test-fft-filter`,
`test-tx-pipeline`, `test-rx-filter`, `test-rx-audio`,
`test-upsample48k`), without any hardware attached.

## What works

- **Receive:** baseband I/Q to SDR apps over HPSDR Protocol 1 (e.g.
  SDRConsole or SparkSDR). An onboard CW/USB/LSB demodulator feeds the radio's own
  speaker and a USB sound card for WSJT-X or FLDigi. FT8 decodes over USB audio
  to WSJTX match SparkSDR on the same I/Q.
- **Transmit:** CW from a straight key, on frequency, ~5 W on all nine
  bands. USB/LSB from the mic have been on the air, and DIGITAL is
  confirmed by a two-way FT8 contact made with WSJT-X through the USB
  gadget. Current limitations are listed in
  [`docs/03_tx_processing_pipeline.md`](docs/03_tx_processing_pipeline.md).
- **Control:** rigctld-compatible TCP (port 4532), Kenwood TS-480 CAT
  over USB serial, HPSDR, and `tools/rigctl_panel.py` (tuning, mode,
  volume, filters, mic gain and a live spectrum).

## Building and running

```
make
./maxibitx
```

Needs `libasound` and single-precision FFTW (`libfftw3f`); see
`Makefile`. `make` also grants the binary real-time scheduling and the
permission the USB gadget needs (`setcap`). The USB gadget needs a
device-mode USB port and a few kernel options - see
[`docs/07_build_and_deployment.md`](docs/07_build_and_deployment.md).

## Documentation

| Doc | Content |
|---|---|
| [`docs/00_intro.md`](docs/00_intro.md) | Scope, status, and a map of the rest of the docs |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Design rationale and build order, step by step, with measurements |
| [`docs/01_hardware_init_and_control.md`](docs/01_hardware_init_and_control.md) | GPIO, si5351/I2C, WM8731 codec bring-up |
| [`docs/02_rx_processing_pipeline.md`](docs/02_rx_processing_pipeline.md) | Antenna to I/Q and audio, stage by stage |
| [`docs/03_tx_processing_pipeline.md`](docs/03_tx_processing_pipeline.md) | Audio source to antenna: keying/PTT, `tx_pipeline.c`, power, known limitations |
| [`docs/04_remote_control_and_iq_output.md`](docs/04_remote_control_and_iq_output.md) | How the external interfaces are implemented |
| [`docs/05_process_and_threading_model.md`](docs/05_process_and_threading_model.md) | Startup sequence, threads, and a map for reading `sound.c` |
| [`docs/06_api.md`](docs/06_api.md) | The external interfaces - rigctld, I/Q streams, USB audio and CAT - for building an app on maxibitx, plus WSJT-X setup |
| [`docs/07_build_and_deployment.md`](docs/07_build_and_deployment.md) | Build, kernel/overlay dependencies, deployment |
| [`docs/08_troubleshooting_and_bringup.md`](docs/08_troubleshooting_and_bringup.md) | Hardware bring-up gotchas |
| [`docs/09_faq.md`](docs/09_faq.md) | Common questions: where it came from, why the design is the way it is, ALC, the interfaces, what's not done |
| [`docs/12_simple_cw_transceiver.md`](docs/12_simple_cw_transceiver.md) | A simple CW transceiver around maxibitx |
| [`docs/dsp_design_notes/`](docs/dsp_design_notes/) | Measurements, derivations and debugging history for the DSP |
| [`docs/code_comments.md`](docs/code_comments.md) | Comment policy: code says what's true now, docs say how it got there |

## Credits

- Ashhar Farhan (VU2ESE) - the sBitx radio and its original software
- JJ's 64-bit sbitx repository, https://github.com/drexjj/sbitx, which
  the code was based on
- `hpsdrsim.c` protocol details from the piHPSDR project
- [minibitx](https://github.com/MikeJohnshoy/minibitx), maxibitx's direct
  predecessor
