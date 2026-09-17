# maxibitx — an all-mode TX/RX pipeline, built on minibitx

maxibitx is a fresh, separate project (not a fork or branch) started
from [minibitx](https://github.com/MikeJohnshoy/minibitx)'s control and
hardware layer, adding a real all-mode (SSB + CW, v1) TX processing
pipeline in the style of sbitx's own FFT-based approach — "only
better," per the specific bugs already root-caused on sbitx/zbitx. The
full rationale, the shared-FFT-pipeline design decision, and the build
order are in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

**Current state: build order steps 1-2 of `ARCHITECTURE.md`.**
Everything below this point in this README, and the rest of `docs/`,
still describes what the actual radio pipeline does *today* — which is
exactly what minibitx does, unchanged (step 1). Step 2 added the shared
FFT overlap-save filter as a standalone, bench-verified primitive
(`src/fft_filter.c`/`.h`, `make test-fft-filter && ./test-fft-filter`)
— proven against synthetic tones, not yet wired into the real audio
path. `cw.c`/`rx_audio.c`/mode handling are all still untouched; see
`ARCHITECTURE.md` §10 for the measured bench numbers and what's next.

---

minibitx is a project to experiment with software from the sbitx codebase. 
The 'mini' in minibitx means we're assembling the minimal set of code necessary to configure and operate the sbitx hardware with the best performance possible.
Code for each required function has been pulled from the sbitx baseline, refined and added to minibitx.
minibitx can now be compiled and run on the Rpi-4 in the sbitx to demonstrate and test performance.
Mature, highly developed external Software Defined Radio (SDR) applications are being used with minibitx to find the upper limit of the sbitx processing chain.
Lessons learned in this project can be folded back into sbitx or used in other projects.

The minibitx receive processing pipeline is largely dictated by the sbitx hardware.
Some significant changes in the digital signal processing software design are being experimented with.

```
  Antenna
     |
     v
  Low Pass Filter (LPF) bank (select one)
     |
     v
  Mixer 1  <---  clk2, si5351 RX LO (varies with tuning)
     |            mixes received signal to xtal_filter_center - the
     |            crystal filter's own real, measured center
     v
  Crystal filter centered at ~40.0124 MHz, based on hardware spec 
     |
     v
  Mixer 2  <---  clk1, si5351 (fixed while receiving - xtal_filter_center
     |           + RX_IF_FREQ_HZ; switches to bfo_freq only for the
     |           duration of TX - see 03_tx_processing_pipeline.md),
     |           shifts output of crystal filter to 24kHz baseband
     v
  Low IF, centered at RX_IF_HZ (24000 Hz)
     |
     v
  ADC / wm8731 audio codec (sound.c, 96 kHz sample rate) gain set experimentally
     |  
     v
  Software VFO (vfo.c, "lo" in radio.c) <--- FIXED at RX_IF_HZ (24000 Hz)
     |            sound.c: sound_process() calls vfo_read_iq() per sample
     v            converts real value A/D output to analytic I&Q at baseband
  Baseband I/Q (centered at 0 Hz)
     |
     v
  Anti-alias FIR (antialias.c, 21 taps, applied separately to I and Q)
     |
     +---> hpsdr_p1.c / usb_gadget.c (UAC2) / iq_stream.c - baseband I/Q handed to an
     |       external SDR app (04_remote_control_and_iq_output.md)
     |
     +---> rx_audio.c - optional local CW demod, straight to the
             WM8731's own speaker/headphone output, no external app
             needed (dsp_design_notes/rx_audio_demod_design.md)

```
A transmit processing pipeline also exists (just imagine the reverse of the process above), currently for CW transmission only.

A secondary minibitx objective is to replace code dependent on deprecated libraries, so wiringPi has been replaced with libgpio.  The 'bit banging' code used for i2c bus was replaced with i2c support built into the kernel.  

minibitx is quite small - much of the code is in the interface software that passes data through various protocols (hpsdr protocol 1, USB audio and control gadget, and UDP interface) to external SDR applications.

Changes: 
- bit-banging code replaced with kernel functions
- wiringPi replaced with libgpio
- DSP processing no longer FFT-based
  - cw receive mode uses AGC across entire IF bandwidth to set usable signal into A/D convertor
  - CW receive processing uses a FIR filter for unwanted image rejection
  - sharp 8-pole elliptic filter for 300 Hz cw filter
  - on TX, cw waveform is built at high end of baseband IF, and then mixed to crystal filter freq where the unwanted product is well outside the crystal filter
- hpsdr_p1.c and usb_gadget.c (UAC2) have been refined through experience gained with some windows SDR apps, but they sre not plug-and-play yet


## Building

```
make
```

Produces a single `maxibitx` binary from the sources in `src/`. Requires
`libasound`, and the usual `pthread`/`libm`/`libdl` (see
`Makefile` for the exact link line).

## Running

```
./maxibitx
```

Brings up the radio hardware, starts the audio and network threads, and
listens for control connections — a rigctld-compatible server on TCP
4532, and an HPSDR Protocol 1 UDP listener. A composite USB gadget provides I&Q data as audio and a CAT control interface.  Additional interfaces will be experimented with.

## Documentation

The `docs/` folder has the detailed breakdown of how the inherited code
works (organized roughly from the hardware up) plus maxibitx's own
architecture and roadmap:

| Doc | Content |
|---|---|
| [`docs/00_intro.md`](docs/00_intro.md) | Project scope, status, and a map of the rest of the docs |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | **maxibitx's own design rationale and build order** — why this is a fresh repo, the shared FFT TX/RX pipeline decision, what's still open |
| [`docs/01_hardware_init_and_control.md`](docs/01_hardware_init_and_control.md) | GPIO, si5351/I2C, WM8731 codec bring-up |
| [`docs/02_rx_processing_pipeline.md`](docs/02_rx_processing_pipeline.md) | Antenna to baseband I/Q, stage by stage |
| [`docs/03_tx_processing_pipeline.md`](docs/03_tx_processing_pipeline.md) | What TX support exists today (still minibitx's) and what's planned (`ARCHITECTURE.md`) |
| [`docs/04_remote_control_and_iq_output.md`](docs/04_remote_control_and_iq_output.md) | rigctld, HPSDR control/IQ, USB Audio Class output |
| [`docs/05_process_and_threading_model.md`](docs/05_process_and_threading_model.md) | Startup sequence and thread structure |
| [`docs/dsp_design_notes/`](docs/dsp_design_notes/) | DSP work (e.g. the anti-alias FIR) and other design docs |
| [`docs/07_build_and_deployment.md`](docs/07_build_and_deployment.md) | Build, kernel/overlay dependencies, deployment notes |
| [`docs/08_troubleshooting_and_bringup.md`](docs/08_troubleshooting_and_bringup.md) | Hardware bring-up gotchas |
| [`docs/10_external_digital_modes_wsjtx.md`](docs/10_external_digital_modes_wsjtx.md) | Using maxibitx with WSJT-X and similar digital-mode apps |
| [`docs/11_general_coverage_sdr_receiver.md`](docs/11_general_coverage_sdr_receiver.md) | Using maxibitx as a general-coverage SDR receiver |
| [`docs/12_simple_cw_transceiver.md`](docs/12_simple_cw_transceiver.md) | Building a simple CW transceiver around maxibitx |

## Credits

- Inspired by Ashhar Farhan's (VU2ESE) original sbitx code
- Code was based on JJ's 64-bit repository at https://github.com/drexjj/sbitx
- hpsdrsim.c from the piHPSDR project
