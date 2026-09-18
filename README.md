# maxibitx — an all-mode TX/RX pipeline, built on minibitx

maxibitx is a fresh, separate project (not a fork or branch) started
from [minibitx](https://github.com/MikeJohnshoy/minibitx)'s control and
hardware layer, adding a real all-mode (SSB + CW, v1) TX processing
pipeline in the style of sbitx's own FFT-based approach — "only
better," per the specific bugs already root-caused on sbitx/zbitx. The
full rationale, the shared-FFT-pipeline design decision, and the build
order are in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

**Current state: build order steps 1-8 of `ARCHITECTURE.md`.**
Step 1 carried minibitx over unchanged. Step 2 added the shared FFT
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
what actually runs today and its bench provenance. **Dial accuracy is
now confirmed on air**: the first real CW transmission on this pipeline
was copied by a remote receiver exactly on frequency, and a real
wattmeter check across all nine bands confirmed transmitted power too -
same `scale`/`TX_GAIN_CORRECTION` values as the old scheme throughout,
no re-tuning needed, ~5-6W band to band and comfortably inside this
board's 20+W PA rating. Image rejection is confirmed on air too - a
remote receiver tuned to where this pipeline's mixer chain actually
predicts a leaked image lands (1400Hz from the carrier, not the naive
±700Hz guess - `ARCHITECTURE.md` §10 step 5 has the derivation) heard
and saw nothing, matching the bench-measured ~-70dB suppression. Step 5
has no remaining open items. Step 6 added the RX-side
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
was noticeably slower than minibitx's - root cause was `rx_filter.c`'s
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
default on that basis.

**Step 8 wires up SSB PTT/mic audio, code-complete, on-air unverified.**
`cw.c`'s key-polling function (still the one physical GPIO line,
`CW_KEY`) now reads that line differently depending on the current mode:
a straight key with semi break-in in CW (unchanged), or an immediate mic
PTT switch in USB/LSB - real sbitx's own `sbitx_gtk.c` source confirms
this is genuinely the same physical contact on the hardware ("Farhan
sometimes demonstrates operating CW with his thumb on the mic PTT
switch"), so no new GPIO was needed. `sound.c` now branches its TX audio
generation on mode too: CW keeps feeding `cw.c`'s tone through the
shared `tx_pipeline.c` instance as before; USB/LSB instead feed real mic
audio through that same instance, selecting upper/lower sideband to
match. The WM8731's 'Mic' capture gain, previously muted, is un-muted to
a first-guess level. None of the new mic-audio gain staging has been
bench- or air-checked yet - unlike CW's own wattmeter-confirmed levels,
this is genuinely untested until a first real SSB transmission happens.
See `ARCHITECTURE.md` §10 step 8 for the full detail, including the
sbitx source cross-check that resolved the PTT-wiring question.

**First on-air SSB test found a real bug: no power out at all.** Keying
the mic in USB produced nothing on a wattmeter or a remote receiver.
Root cause: `sound_set_rx_capture()` - called at the start of every TX
burst to protect the RX chain from relay/PA-harmonic bleed - was
zeroing the WM8731's whole `'Capture'` gain element, both stereo
channels at once. That was fine while `'Capture'` only fed the RX path,
but this board's mic almost certainly rides that same element's RIGHT
channel (`'Input Mux'` is locked to `'Line'`, not `'Mic'`, so the mic
likely never touches the codec's own separate internal mic preamp at
all) - so every TX burst was silently zeroing the mic's real level the
instant it started, regardless of any gain constant downstream. Fixed
with a new per-channel capture-volume helper, the same pattern
`sound_set_tx_drive()`/`sound_set_local_monitor()` already used for
"Master" - `sound_set_rx_capture()` now touches only the LEFT (RX)
channel, leaving the mic's RIGHT channel alone through every TX/RX
transition. Also added a Mode selector (CW/USB/LSB/DIGITAL) to
`tools/rigctl_panel.py`, since testing this surfaced a real gap: no way
to see or change mode from the panel made a mode/PTT mismatch one more
silent failure indistinguishable from this bug until ruled out by hand.
See `ARCHITECTURE.md` §10 step 8 for the full writeup, including what
would mean this fix needs a different approach (a console warning to
watch for) if this specific ALSA control turns out not to support
independent per-channel capture after all.

**Re-test: mic audio now audible on the local monitor, still no
measurable power out.** A different, narrower symptom than before - it
confirms the capture-mute fix worked (real mic signal is reaching
`sound.c` again), and points at plain gain-staging rather than a
wiring/muting bug this time: `MIC_TX_INPUT_SCALE` was always a flagged
guess, and a real mic likely peaks far below `cw_get_sample()`'s
near-unity CW tone for ordinary speaking volume - easily enough of a
gap that a wattmeter reads nothing while a speaker amplifier makes the
same signal perfectly audible. Rather than re-guess a compile-time
constant (another edit/rebuild/restart cycle), added a live,
runtime-adjustable `mic_tx_gain` multiplier on top of the fixed unit
conversion - reachable via a new rigctld extension (`l`/`L MICGAIN`)
and a new "Mic Gain (TX, USB/LSB)" slider in `tools/rigctl_panel.py`,
so the next several bisection trials against a real wattmeter can
happen live while transmitting, the same way `AF` volume already
works. Not yet re-tested - next step is keying up and raising that
slider while watching the wattmeter.

**Third data point ruled out gain staging: LSB measured a hard 0W while
USB (identical settings) put out full power.** A level problem would
hit both sidebands equally, so this pointed at something
sideband-specific in `tx_pipeline.c` instead - and turned up two real
bugs in the LSB (`TX_PIPELINE_KEEP_LOWER`) path specifically, which no
bench test had ever actually run before this. First: `tx_pipeline.c`'s
shared filter was tuned with plain `filter_tune()`, whose passband is
deliberately one-sided (positive frequencies only) - fine for USB/CW,
but it meant LSB's needed negative-frequency content was already zeroed
by the filter itself before the explicit sideband-select step ever ran,
leaving nothing to keep at all (measured directly: 0.707 RMS output for
USB's path vs. 0.0000024 for LSB's, same input tone - not mis-placed,
completely absent). Fixed by switching to `filter_tune_real()` (the
same symmetric/mirrored entry point `rx_filter.c` already relies on for
its own real-signal reason), so both sidebands' content survives the
filter and the explicit sideband-zero step is what actually separates
them, as originally designed. Second, smaller bug: the shared IF
bin-rotate (`TX_IF_SHIFT_BINS`) was derived only for USB/CW's kept-upper
case and reused unmirrored for LSB, which lands LSB's content off the
crystal filter's center - fixed with a mirrored `TX_IF_SHIFT_BINS_LSB`,
now selected per-block based on which sideband is active. Both fixes
were required together; verified with a new `tx_pipeline_test.c` Case D
that exercises `TX_PIPELINE_KEEP_LOWER` for the first time, now reading
0dB at LSB's predicted placement and deeply negative at the frequency
the old bug would have produced. Bench-proven only - not yet re-tested
on air; next step is re-trying LSB to confirm it now matches USB.

See `ARCHITECTURE.md` §10 for the measured/verified detail on all eight
steps, including these follow-ups.

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
