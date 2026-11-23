# maxibitx

NOTE: THIS BRANCH RUNS ON BOTH ZBITX AND SBITX HARDWARE

A headless, all-mode (CW, USB, LSB, DIGITAL) radio daemon for the
Raspberry Pi inside an [sBitx](https://github.com/afarhan/sbitx). It
brings up the radio hardware, runs the receive and transmit signal
chains, and serves external software. There's no user interface in
the process: SDR apps, WSJT-X, FLRig and a small separate desktop control panel
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

**maxibitx** started from minibitx's hardware and control layer and adds
an all-mode transmitter pipeline and an onboard demodulator - without
bringing the coupling back. Every job lives in its own small file,
and no UI runs in the process. Anything an external display or controller needs
goes through a narrow, documented interface
([`docs/06_api.md`](docs/06_api.md)).

The whole daemon is about 11,500 lines of C in 30 files; a big part of that is 
in a collection of external interfaces. These provide a variety of ways to connect
applications to maxibitx.

| Job | Files |
|---|---|
| Startup and shutdown | `maxibitx.c` |
| Radio state: tuning, RIT, mode, T/R switching | `radio.c` |
| Hardware: board profiles, GPIO, I2C, si5351 clocks, LPF relays, board calibration | `radio_hw.c`, `gpio.c`, `i2c.c`, `si5351v2.c`, `hw_settings.c` |
| Real-time audio thread and WM8731 codec | `sound.c` |
| RX: I/Q mixing, anti-aliasing, demodulation | `vfo.c`, `antialias.c`, `rx_audio.c`, `rx_filter.c` |
| TX: CW keying, shared FFT TX pipeline | `cw.c`, `key_input.c`, `keyer.c`, `morse.c`, `tx_pipeline.c`, `fft_filter.c` |
| USB audio rate conversion | `decim48k.c`, `upsample48k.c` |
| External interfaces | `interfaces/hamlib.c` (rigctld), `interfaces/hpsdr_p1.c` (HPSDR Protocol 1), `interfaces/usb_gadget.c` (USB audio + Kenwood CAT), `interfaces/iq_stream.c` (I/Q for the control panel), `interfaces/tci.c`, `tci_cw.c`, `tci_stream.c`, `tci_ws.c` (TCI: control, audio, I/Q and CW text over a WebSocket) |

`make test` runs every bench test below and says which, if any, failed.
The DSP is bench-tested on its own (`make test-fft-filter`,
`test-tx-pipeline`, `test-rx-filter`, `test-rx-audio`,
`test-rx-audio-impulse`, `test-upsample48k`, `test-cw`, `test-key-input`,
`test-keyer`, `test-keyer-straight`), and so are the TCI
server (`test-tci-ws`, `test-tci`, over loopback) and the board layer
(`test-radio-hw`, with GPIO and I2C stubbed), without any hardware attached.

## Features & Capabilities

### Hardware & Architecture
- **Multi-board support:** Runs natively on **sBitx** (DE, v2, v3) and **zBitx** hardware from a single codebase, configured via `data/hw_settings.ini`.
- **Modular headless architecture:** Separates real-time DSP and hardware control (~11,500 lines of C across 30 focused files) from the UI so display or control changes stay cleanly isolated.
- **Native Linux kernel I/O:** Uses Linux kernel GPIO character devices and native I2C drivers for clean, deterministic hardware control.

### Receive & Transmit DSP
- **All-mode transceiver pipeline:** Full support for **CW**, **USB**, **LSB**, and **DIGITAL** modes across all nine HF bands (80m–10m).
- **Refined CW keying:** Supports straight keys and iambic paddles with proper element weighting, clean envelope shaping, no dropped leading/trailing elements, and ~5 W output across all bands.
- **Timing-adaptive CW decoding:** Fundamentally timing-based decoder that adapts to character/element timing variations and shifting signal conditions.
- **Onboard demodulation & filtering:** Dedicated CW, USB, and LSB demodulators drive the speaker directly alongside audio routing for digital mode software.
- **Verified voice & digital operation:** Proven on-air with SSB voice and two-way FT8 contacts via WSJT-X over the USB audio gadget.

### External Interfaces & Control
- **HPSDR Protocol 1:** Streams wideband baseband I/Q over UDP to desktop SDR clients such as **SDR Console** and **SparkSDR**.
- **Transceiver Control Interface (TCI):** Built-in WebSocket server (port `50001`) carrying rig control, bidirectional audio, I/Q streams, and CW text over one connection for **sdrOxide**, **JTDX**, **WSJT-X Improved**, loggers, and Hamlib's TCI backend.
- **Hamlib & Kenwood CAT:** Network rig control via `rigctld`-compatible TCP (port `4532`) plus Kenwood TS-480 CAT emulation over USB serial.
- **USB Audio & Serial Gadget:** Presents the radio as a standard USB sound card and serial port to a host computer for single-cable digital operation.
- **Standalone Touch Control Panel:** Includes `tools/rigctl_panel.py`, a lightweight external GUI offering tuning, mode/filter selection, volume/mic gain, and a live spectrum and waterfall display.

### Built-in Verification
- **Hardware-free bench suite:** `make test` runs isolated unit and loopback tests across the DSP chains (`tx_pipeline`, `rx_filter`, `rx_audio`, CW/keyer), TCI WebSocket server, and stubbed board hardware layers with no radio attached.

## Building and running

```
make
./maxibitx
```

`data/hw_settings.ini` must name the radio board, on a line above its
first `[section]`: `sbitx_version = SBITX_V3` for an sBitx DE, v2 or
v3, or `sbitx_version = SBITX_V4` for a zBitx.
Without it maxibitx prints the valid lines and exits, before touching
any hardware. Run it from the repository root, where it finds
`data/hw_settings.ini`.

Needs `libasound` and single-precision FFTW (`libfftw3f`); see
`Makefile`. `make` also grants the binary real-time scheduling and the
permission the USB gadget needs (`setcap`). `make test` builds and runs
every bench test. `./update` stashes local changes and pulls the latest;
then `make` again.

The Pi needs the codec's overlay (`dtoverlay=audioinjector-wm8731-audio`),
the si5351's I2C bus (on an sBitx, the `i2c-rtc-gpio` overlay it shares
with the RTC), and the user in the `gpio` group; see
[`docs/01_hardware_init_and_control.md`](docs/01_hardware_init_and_control.md).
A zBitx also needs the UART off
([`docs/08_troubleshooting_and_bringup.md`](docs/08_troubleshooting_and_bringup.md)).
The USB gadget needs a device-mode USB port and a few kernel options:
[`docs/dsp_design_notes/usb_gadget_OS_setup.md`](docs/dsp_design_notes/usb_gadget_OS_setup.md).

## Documentation

| Doc | Content |
|---|---|
| [`docs/00_intro.md`](docs/00_intro.md) | Introduction to maxibitx: the whole system in one stand-alone document, including the RX and TX pipelines in detail |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Design rationale and build order, step by step, with measurements |
| [`docs/01_hardware_init_and_control.md`](docs/01_hardware_init_and_control.md) | GPIO, si5351/I2C, WM8731 codec bring-up |
| [`docs/02_rx_processing_pipeline.md`](docs/02_rx_processing_pipeline.md) | Antenna to I/Q and audio, stage by stage |
| [`docs/03_tx_processing_pipeline.md`](docs/03_tx_processing_pipeline.md) | Audio source to antenna: keying/PTT, `tx_pipeline.c`, power, known limitations |
| [`docs/04_remote_control_and_iq_output.md`](docs/04_remote_control_and_iq_output.md) | How the external interfaces are implemented |
| [`docs/05_process_and_threading_model.md`](docs/05_process_and_threading_model.md) | Startup sequence, threads, and a map for reading `sound.c` |
| [`docs/06_api.md`](docs/06_api.md) | The external interfaces - rigctld, I/Q streams, USB audio and CAT, TCI - for building an app on maxibitx, plus WSJT-X setup |
| [`docs/08_troubleshooting_and_bringup.md`](docs/08_troubleshooting_and_bringup.md) | Hardware bring-up gotchas |
| [`docs/09_faq.md`](docs/09_faq.md) | Common questions: where it came from, why the design is the way it is, ALC, the interfaces, what's not done |
| [`docs/dsp_design_notes/`](docs/dsp_design_notes/) | Measurements, derivations and debugging history for the DSP |
| [`docs/code_comments.md`](docs/code_comments.md) | Comment policy: code says what's true now, docs say how it got there |

## Credits

- Ashhar Farhan (VU2ESE) - the sBitx radio and its original software
- Jesse Kujawa (W9JES) - the 64-bit sbitx repository,
  https://github.com/drexjj/sbitx, which the code was based on
- `hpsdrsim.c` protocol details from the piHPSDR project
- [minibitx](https://github.com/MikeJohnshoy/minibitx), maxibitx's direct
  predecessor
- Evan Hand (AC9TU), Robert Benedict (KD8CGH) and Juan Agrinsoni (WP3DN) - encouragement
  throughout

## License

GNU General Public License v3.0, to the extent that it does not violate
the rights of the authors whose open-source code this was drawn from -
see [`LICENSE`](LICENSE) and
[`docs/00_intro.md`](docs/00_intro.md#10-licensing-and-acknowledgements).
