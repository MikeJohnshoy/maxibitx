# maxiBitx — an all-mode TX/RX pipeline, built on miniBitx

maxiBitx is a fresh, separate project (not a fork or branch) built on
[miniBitx](https://github.com/MikeJohnshoy/minibitx)'s control and
hardware layer, adding an all-mode (SSB/CW/DIGITAL) TX processing
pipeline in the style of sbitx's own FFT-based approach "only
better". miniBitx was the first attempt to build a clean SDR platform
the harder I looked the more I kept coming back to Farhan's choices 
in sbitx. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for more
of how we got here.

**Current state: build order steps 1-7 of `ARCHITECTURE.md`.**
Step 1 carried miniBitx over unchanged. Step 2 added the shared FFT
overlap-save filter as a standalone, bench-verified primitive
(`src/fft_filter.c`/`.h`, `make test-fft-filter && ./test-fft-filter`).
Step 3 made mode a real, single-owner value (`radio_set_mode()`/
`radio_get_mode()`) both control surfaces agree on. Step 4 added a new
shared TX pipeline module (`src/tx_pipeline.c`/`.h`,
`make test-tx-pipeline && ./test-tx-pipeline`) implementing CW's slice
of the plan, bench-verified against a synthetic stand-in for `cw.c`'s
sidetone. **Step 5 wired it in live**: `cw.c`'s old direct-to-DAC path
(`cw_tx_carrier`/`TX_IF_OFFSET_HZ`) and `radio_tx_apply()`'s matching
clk2 correction are gone - CW TX now runs on the shared FFT pipeline for
real, not just on the bench. `docs/03_tx_processing_pipeline.md` (and
the "Transmit" line below) describes the *old*, now-replaced scheme;
see that doc's own updated intro and `ARCHITECTURE.md` §10 step 5 for
what actually runs today, its bench provenance, and what's still
outstanding (on-air re-verification of dial accuracy, image rejection,
and transmitted power against a wattmeter - not yet done on real
hardware, only bench-proven so far). Step 6 added the RX-side
counterpart, bench-only: a new `src/rx_filter.c`/`.h` module
(`make test-rx-filter && ./test-rx-filter`) implementing the same shared
FFT engine for `rx_audio.c`'s stage 3, pitch/width now live parameters
instead of a baked-in design. **Step 7 wired it into `rx_audio.c` for
real**: both the original 8-pole elliptic filter and the new FFT filter
now run continuously side by side, with a new selector
(`rx_audio_set_narrow_filter_impl()`, reachable remotely via rigctld's
new `u`/`U FFTFILT` and a new checkbox in `tools/rigctl_panel.py`'s RX
Filter panel) choosing which one the operator actually hears - elliptic
stays the default. `src/rx_audio_test.c`
(`make test-rx-audio && ./test-rx-audio`) integration-tests this new
wiring directly. What's still outstanding: the on-air listening
comparison itself (does the FFT filter sound at least as good for real
CW copy) - the code is complete and tested, but that judgment can only
be made on real hardware, not on a bench; `rx_audio.c`'s
`narrow_filter_coeffs[]` stays in the tree until it is.

**First real on-air test of step 7** found receive audio working and the
elliptic filter still effective, plus follow-ups (`ARCHITECTURE.md` §10
step 7's own entries have the full detail), all now resolved. Startup
was noticeably slower than miniBitx's - root cause was `rx_filter.c`'s
new, larger `FFTW_MEASURE` plan search compounding with
`tx_pipeline.c`'s pre-existing one, both paid at every process start -
fixed by a new `fft_filter.c` `filter_new_ex()` that lets both use
`FFTW_ESTIMATE` instead. A real xrun flood also showed up on playback -
traced (via a temporary `sound.c` timing diagnostic, now opt-in via
`MAXIBITX_LOOP_TIMING=1`) to `tx_pipeline_new()`'s `FFTW_MEASURE` search
running long enough, synchronously, to drain the playback buffer before
the audio thread that keeps it fed ever got to run - fixed by
reordering `sound_thread_start()` so the buffer is primed right before
that thread starts, not before an unrelated setup cost gets to run in
between. Confirmed clean on the user's own hardware: no more xruns, loop
timing rock-solid for the whole run. And "Use FFT filter" not producing
a clearly noticeable audible difference from the elliptic filter turned
out not to be a bug either: the user's own console log shows rigctld's
`STRENGTH` S-meter - fed by the exact same signal that drives the
speaker - reading comparably low noise levels under both filters, well
below the unfiltered reading. The FFT filter really is rejecting a
similar amount of total noise energy; the elliptic filter's own
resonant ripple just makes that rejection sound far more dramatic to
this operator's ear. That account was then checked directly against
both filters' actual measured frequency response (the FFT filter's live
`fir_coeff`, read straight from the running code, against the elliptic
cascade evaluated analytically) rather than left as a guess - the FFT
filter turned out to be the narrower, deeper filter of the two by every
measure (tighter -3dB width, a stopband floor 20-30dB below the
elliptic's own ripple-limited one), so it isn't quietly under-filtering;
the resonance-vs-flat-response explanation holds up. Elliptic stays the
default on that basis. See `ARCHITECTURE.md` §10 for the
measured/verified detail on all seven steps, including these follow-ups.

---

miniBitx was a project to experiment with software from the sbitx codebase. 
The 'mini' in miniBitx means we're assembling the minimal set of code necessary to configure and operate the sbitx hardware with the best performance possible.
Code for each required function has been pulled from the sbitx baseline, refined and added to miniBitx.
miniBitx can now be compiled and run on the Rpi-4 in the sbitx to demonstrate and test performance.
Mature, highly developed external Software Defined Radio (SDR) applications are being used with miniBitx to find the upper limit of the sbitx processing chain.
Lessons learned in this project can be folded back into sbitx or used in other projects.

The miniBitx receive processing pipeline is largely dictated by the sbitx hardware.
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

A secondary miniBitx objective was to replace code dependent on deprecated libraries, so wiringPi has been replaced with libgpio.  The 'bit banging' code used for i2c bus was replaced with i2c support built into the kernel.  

miniBitx is quite small - much of the code is in the interface software that passes data through various protocols (hpsdr protocol 1, USB audio and control gadget, and UDP interface) to external SDR applications.

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
| [`docs/03_tx_processing_pipeline.md`](docs/03_tx_processing_pipeline.md) | What TX support exists today (still miniBitx's) and what's planned (`ARCHITECTURE.md`) |
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
