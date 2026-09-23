# maxibitx — architecture and design rationale

Status: §10 steps 1-7 done. Step 1: this tree is minibitx's `src/`,
`docs/`, `data/`, and `tools/` carried over unchanged (only
`src/maxibitx.c`'s filename and the Makefile's output binary name
changed), confirmed building and running on real hardware. Step 2: the
shared FFT overlap-save filter (`src/fft_filter.c`/`.h`) is ported and
bench-verified against synthetic tones (`src/fft_filter_test.c`). Step
3: mode is a real, single-owner value now (`radio_set_mode()`/
`radio_get_mode()`, `radio.c`), agreed on by both control surfaces. Step
4: a new, parallel shared TX pipeline module (`src/tx_pipeline.c`/`.h`)
implements CW's slice of §5's plan and is bench-verified against a
synthetic stand-in for `cw.c`'s sidetone (`src/tx_pipeline_test.c`). Step
5: `tx_pipeline.c` is wired into `cw.c`/`sound.c` for real — CW TX now
runs on the shared FFT pipeline live, not just on the bench;
`cw_tx_carrier`/`TX_IF_OFFSET_HZ` and `radio_tx_apply()`'s
`CW_PITCH_HZ` clk2 correction are gone. Step 6: a new, parallel RX
narrow-filter module (`src/rx_filter.c`/`.h`) implements §5's RX plan —
the same shared FFT engine, with pitch/width as live parameters instead
of `rx_audio.c`'s baked-in elliptic design — bench-verified against
synthetic tones (`src/rx_filter_test.c`); `fft_filter.c` gained
`filter_tune_real()` for this (a real, symmetric bandpass, distinct from
`filter_tune()`'s one-sided SSB-style interval). **Step 7 wired it in
live**: `rx_audio.c` now runs BOTH stage-3 implementations continuously
and picks between them via a new selector
(`rx_audio_set_narrow_filter_impl()`), elliptic still the default —
reachable remotely via a new rigctld command (`u`/`U FFTFILT`) and a new
checkbox in `tools/rigctl_panel.py`'s RX Filter panel, so the operator
can A/B the two on real signals before `narrow_filter_coeffs[]` is ever
removed for good. **First on-air test of step 7 found real audio
working**, plus follow-ups, all now resolved: a startup-delay regression
(RX's new, larger `FFTW_MEASURE` plan compounding with TX's pre-existing
one — fixed via a new `filter_new_ex()` letting both `rx_filter.c` and
`tx_pipeline.c` use `FFTW_ESTIMATE` instead); a real xrun flood on
playback, traced (via a temporary, now opt-in `loop timing` diagnostic
in `sound.c`) to a startup-sequencing race — `tx_pipeline_new()`'s
`FFTW_MEASURE` search ran long enough, synchronously, to drain the
playback buffer before the audio thread that refills it ever got to run
— fixed by reordering `sound_thread_start()` and confirmed clean on the
user's real hardware, no more xruns at all; and the FFT filter's audible
effect question, now answered by the user's own console log: the
`STRENGTH` S-meter (fed by the same signal that drives the speaker)
shows the FFT filter genuinely rejecting a comparable amount of total
noise energy to the elliptic filter — not a bug — but the elliptic
filter's own resonant ripple makes that rejection *sound* far more
dramatic to this operator's ear, so elliptic stays the sensible default
per §10 step 7's follow-up entry. That explanation was then checked
directly against both filters' actual measured shapes (`fir_coeff` read
live, elliptic's biquad cascade evaluated analytically) rather than
left as an assumption — the FFT filter turned out to be the *narrower,
deeper* filter of the two by every measure (-3dB width, stopband floor),
so "little effect on the hiss" isn't the FFT filter under-filtering;
it's consistent with the same resonance-vs-flat-response explanation,
now on firmer footing. **Step 5's TX side has its own on-air
confirmation now too**: the first real CW transmission on the new live
`tx_pipeline.c` path was copied by a remote receiver exactly on
frequency — dial accuracy confirmed in both directions (RX via W1AW,
step 7; TX via this remote copy, step 5). A real wattmeter check across
all nine bands then confirmed transmitted power too, same `scale`/
`TX_GAIN_CORRECTION` values throughout, no changes needed — a
conservative ~5-6W band to band, well inside this board's 20+W PA
rating. A remote receiver tuned exactly to where this pipeline's own
mixer math predicts a leaked image would land (1400Hz from the wanted
carrier, not the ±700Hz a naive guess suggests — step 5's own entry has
the derivation) heard and saw nothing, confirming the bench-measured
~-70dB image suppression on air too. Step 5 has no remaining open
items. See §10 for the actual
measured/verified detail on all seven steps.
What's left for RX specifically: the on-air listening comparison itself
(step 7's own entry) — the code is complete and integration-tested
(`src/rx_audio_test.c`), but "sounds at least as good for real CW copy"
is the operator's judgment call on real hardware, not something
bench-verifiable. That judgment has now been made once, and it went
against the FFT implementation — but the bench could not reproduce the
reported selectivity problem, and instead found a large time-domain
difference at keying transitions; see step 7's on-air entry and
[`dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md`](dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md).
Elliptic remains the default, and no code changed. This document is both the
design rationale that justified starting `maxibitx` as its own repo
(not a minibitx branch, not an sbitx fork-in-place) and the plan for
§10's remaining steps. Everything below is grounded in minibitx's
actual source as inherited into this tree (`radio.c`, `cw.c`, `sound.c`,
`rx_audio.c`, `vfo.c`, and `docs/03_tx_processing_pipeline.md`) and in
the specific bugs already root-caused on sbitx/zbitx — not a
from-scratch guess at what a "better" pipeline might need.

## 1. Relationship to minibitx and sbitx

maxibitx is a fresh repo. It is not a minibitx fork with TX bolted on,
and not an sbitx fork refactored in place — it's built up the way
minibitx itself was: starting from minibitx's own control/hardware layer
almost unchanged (`radio.c`, `radio_hw.c`, `gpio.c`, `si5351v2.c`,
`vfo.c`, `hamlib.c`, `usb_gadget.c`, `hpsdr_p1.c` — none of these care
what mode is transmitting, they just sequence hardware and carry state),
and adding a new TX audio pipeline alongside `rx_audio.c` the same way
`rx_audio.c` was added alongside the RX I/Q path: one more consumer/
producer wired into `sound.c`'s existing audio callback, not a rewrite
of anything that already works.

sbitx is the reference for *scope* (all-mode, not headless-only) but not
for *implementation* — its actual TX chain (FFT-based, GTK-coupled) is
exactly the thing being avoided, for reasons in §2. Where sbitx's own
bench-verified numbers exist (its `calibrate_band_power()` per-band
scale table, its `rx_pitch`-equivalent dial-accuracy correction), reuse
the data, not the code shape.

## 2. What "better than sbitx" means here

Not a vague aspiration — five categories of already-diagnosed sbitx/
zbitx bugs, and what maxibitx does structurally so each category can't
recur the same way.

**UI/display work sharing blocking resources with the real-time audio
path.** This is the single largest and most expensive bug category on
record: `zbitx_poll()`'s blocking bit-banged I2C exchange to the remote
display shares a real mutex (`i2c_bus_mutex`) with the audio thread's
own `query_swr()`, so a slow display update directly stalls audio via
lock contention — not just GTK-thread scheduling. Measured cost: 52-66ms
per `zbitx_poll()` call with only 2 fields dirty, and up to ~2000-2256ms
when a MODE click marks ~200 fields dirty and they all get pushed in one
blocking call. Multiple point fixes were tried (per-call field caps,
resumable scan cursors, CW-TX gating) and even after landing, a team
reviewer rejected part of the fix for regressing display responsiveness
elsewhere — a sign the coupling itself, not any one call site, is the
problem. minibitx already sidesteps this by having **no UI in-process at
all** (control is external: rigctld, CAT, HPSDR). maxibitx keeps that:
no GTK, no local display driver, no shared mutex between any display
path and the audio thread, full stop. If a local touchscreen UI is
wanted later (minibitx's own `mb-radio` companion-app idea, already on
the roadmap separately), it stays a **separate process** talking to
maxibitx over a narrow socket protocol — the same reason `mb-radio` was
kept out of minibitx's own process in the first place.

**Rate-mismatch bugs — state cached or read at the wrong rate.** Two
confirmed zbitx bugs share one shape: `cw_key_state` was refreshed only
at UI poll rate (~10-20Hz) while consumed at the 96kHz audio rate
(iambic timing errors), and a stale `cached_pitch`/`last_pitch` static
caused both a missed first dit and a blocking fldigi XML-RPC call firing
on every `modem_rx()` invocation. minibitx's own `cw.c` already gets
this right by construction — `cw_poll_key()` runs once per audio block
inside the audio thread itself, not on a separate UI timer — and
maxibitx's new TX audio code follows the same rule: anything the
audio-rate path consumes is either read at audio rate directly, or its
producer/consumer rate relationship is written down as an explicit
comment at the read site, not assumed.

**Spectrum/waterfall/meter computation living inline in the DSP
function itself.** An unthrottled RX spectrum FFT inside `rx_linear()`
and a per-call windowed FFT spectrum block inside `tx_process()` were
both independently found to be causing ALSA underruns — fixed by
throttling/removing them, not by making the FFT faster. `rx_audio.c`
already establishes the right pattern (the AGC's envelope follower is
the only "extra" computation riding along the audio path, and it was
specifically moved upstream, to the raw input, after two rounds of
proving that tapping a downstream stage's output coupled unrelated
concerns together — see `docs/dsp_design_notes/rx_audio_demod_design.md`
§8.7/§8.8). Any metering/spectrum display maxibitx adds later reads a
copy of the samples on its own thread/rate, never computes anything
inline in the function real-time audio depends on.

**Blocking calls near the real-time path.** The fldigi XML-RPC bug
above is the clearest example: a network call, gated by a stale cache
variable, firing from inside a function in the audio-adjacent chain.
maxibitx's control surfaces (`hamlib.c`, `usb_gadget.c`) already only
ever set/read plain in-process state (`radio_set_tx()`, etc.) — no
control surface calls out to a subprocess or another host synchronously,
and the new TX audio code introduces no new one either.

**DSP algorithm bugs already found and fixed once.** `rx_linear()`'s
noise reduction went through an inverted spectral-subtraction sigmoid,
an ANR Wiener filter reading post-subtraction data instead of pre-,
complex-bin smoothing causing phase cancellation, and a miscalibrated
noise bias — all fixed, and superseded by minimum-statistics noise
tracking (Martin's algorithm) + MMSE-LSA gain estimation as the actual
target design. If/when maxibitx's own RX chain (built on minibitx's
`rx_audio.c`) grows real ANR, it starts from the *already-corrected*
algorithm, not a fresh spectral-subtraction attempt that re-earns the
same four bugs. (RX DSP quality is out of v1 scope per §3/§8 below —
noted here because it's a "don't re-pay this cost" item for whenever it
does come up.)

## 3. v1 scope

SSB (USB/LSB) + CW, sharing **one** TX audio pipeline rather than two
parallel ones — see §5 for why that's a revision from this doc's first
draft. A `DIGITAL` mode value is also reserved in v1 as a placeholder
for externally-generated digital-mode audio (WSJT-X on a host PC,
arriving over the existing mic/line-in path) — see §5, §8. Onboard
digital-mode encode/decode, AM/FM, and any onboard RX DSP quality work
beyond what minibitx already has remain explicitly deferred — see §8.

## 4. The central technical decision: SSB needs sideband separation done in software, not analog hardware

This is the one place "generalize cw.c" is the wrong instinct, and it's
worth stating precisely why, because the wrong assumption here would
cost real bench time. It's also the one place this doc has been
directly revised after checking it against real sbitx source
(`tx_process()`/`fft_filter.c` in `mj_zbitx`) rather than the design
article alone — see the note at the end of this section.

minibitx's CW TX chain (`docs/03_tx_processing_pipeline.md`) works by
generating a single software tone at `CW_PITCH_HZ + TX_IF_OFFSET_HZ`
(~23.3kHz) and handing it to a real (not I/Q) analog balanced modulator,
which necessarily produces both `bfo_freq - tone` and `bfo_freq + tone`.
The doc is explicit about this: *"There's no phasing network or
Hilbert-transform stage here — this hardware has a single real balanced
modulator... and a real signal times a real LO always produces both sum
and difference, no way around it."* What makes CW come out
single-sideband anyway is that `bfo_freq` sits ~22.6kHz off the crystal
filter's measured center (`xtal_filter_center`), so the *sum* product
lands ~28.5kHz above the passband's upper edge (measured -61dB there,
~40dB confirmed on air) while the *difference* product lands almost
exactly at the filter's peak. That's rejection of **one mixing product
against the other**, ~45kHz apart — a coarse job, well within a filter
measured at ~35kHz wide with its skirt only starting to roll off ±17.4kHz
from center.

Generalizing that trick naively to voice — feed band-limited mic audio
into the same real mixer instead of a keyed tone — does not produce SSB.
A real signal times a real LO produces upper *and* lower images of
*everything* in that signal, by the modulation theorem, regardless of
where the LO sits. Voice content spans roughly 300-2700Hz; separating
its own upper sideband from its own lower sideband needs on the order of
a few hundred Hz of discrimination *around zero pitch* — nothing like
the ~45kHz gap the crystal filter is actually being asked to referee
today. The filter that made CW's ~40dB possible is roughly 15-20x too
coarse to do this job for voice. Feeding raw mic audio into
`cw_get_tx_sample()`'s mixer would transmit double sideband (both USB
and LSB simultaneously) — not narrowly, not "a bit fuzzy," just
literally DSB, the way §8 of that document's own math predicts.

So SSB needs genuine single-sideband generation *before* the signal
ever reaches the DAC/analog-mixer/crystal-filter chain. Farhan's own
sBitx design article gives his reason for not doing this the classic
analog **phasing** way (a hardware LO quadrature splitter plus two
matched analog audio chains): it's extremely sensitive to RF phase
imbalance and component-tolerance mismatches, where even a fraction of
a degree or a few percent of gain mismatch measurably degrades opposite-
sideband suppression — a real, well-documented failure mode of analog
phasing exciters, and a fair reason to avoid building one from
homebrew-grade parts.

**But — checked directly against `mj_zbitx/src/tx_process()` and
`fft_filter.c` rather than assumed from the article — his actual
solution still separates the sidebands digitally, in software; it
doesn't hand that job to the crystal filter.** `tx_process()` runs the
mic audio through an FFT overlap-save filter (`tx_filter`, `filter_new()`/
`filter_tune()`) tuned to keep only one side of the spectrum around zero
— `filter_tune(tx_filter, 300.0/96000, 3000.0/96000, 5)` for USB, the
mirrored negative range for LSB — multiplies the block's FFT by that
response, and then *explicitly zeroes* whichever sideband's bins are
unwanted on top of that, rather than trusting the filter's stopband
alone. That is single-sideband construction happening in the frequency
domain, mathematically the same job a Hilbert-transform phasing exciter
does (build a one-sided-spectrum/analytic signal), just implemented as
FFT bin selection instead of a time-domain FIR. The crystal filter's
job downstream is the same coarse one it already has for minibitx's CW
— cleaning up mixing products/harmonics from the upconversion — not
separating USB from LSB; that already happened in the FFT stage.

Read this way, Farhan's rationale is "avoid *analog* phasing hardware,"
not "avoid software sideband separation" — he moved the delicate part
into software, where precision is free, and kept the filter for the job
analog hardware is actually reliable at. That's the same principle this
section already argued for; **the revision is in the implementation
maxibitx should copy.** Rather than a from-scratch time-domain Hilbert
FIR, port the proven, on-air-validated technique already sitting in
`fft_filter.c`/`tx_process()`: an FFT overlap-save filter with a
runtime-tunable passband, plus an explicit zero of the unwanted
sideband's bins. This is not unfamiliar territory for this codebase
either way — `rx_audio.c`'s own stage 1 is already a complex
(Hilbert-style) bandpass filter for RX image rejection, and `vfo.c`'s
`vfo_read_iq()` is the complex-mixing primitive stage 2 uses — but
`fft_filter.c`'s approach is real, working code for the specific job of
building single-sideband audio, not just an adjacent technique to
generalize from.

One genuine bonus this uncovered: `filter_tune()`'s passband edges are
plain runtime arguments (`set_rx_filter()` calls it the same way for
RX, including CW/CWR's narrow filter) — meaning a maxibitx SSB exciter
(or a CW narrow filter) built this way is trivially retunable at
runtime, unlike minibitx's own `rx_audio.c` stage 3, which is a *fixed*,
offline-`scipy.signal.ellip`-designed IIR precisely because nothing
like `filter_tune()` exists in minibitx today.

**Decided: this isn't left as a someday-maybe migration — RX's narrow
CW filter moves onto the same shared FFT pipeline too, not just TX.**
`rx_audio.c` stage 3's fixed 700Hz-center/300Hz-width elliptic design
was flagged as "a big negative" once CW pitch and filter width came up
as things an operator should actually be able to change — a fixed,
offline-designed IIR can't do either without regenerating coefficients.
Bringing RX onto the same `filter_tune()`-based pipeline (§5) — same
block size as TX/SSB, not a separately-sized "narrow CW" block — gives
both pitch *and* width as live, continuous operator controls, on both
transmit and receive, for the cost of one shared mechanism instead of
two (`rx_audio.c`'s bespoke IIR plus a separate TX exciter). The
accepted trade, quantified rather than hand-waved: at the same block
size sbitx itself uses everywhere (`filter_new(1024, 1025)`, Kaiser
`beta=5`), the realizable transition is roughly ~300Hz per side versus
the elliptic design's measured ~135Hz per side — a real, roughly 2x
softer skirt. That's the deliberate cost of a common, variable-pitch,
variable-width, single-pipeline architecture instead of a sharper but
fixed, TX/RX-duplicated one, and it's being made with the numbers in
hand, not discovered later on the bench.

## 5. Proposed pipeline — one FFT chain for CW and SSB, TX and RX

This section originally covered TX only; per §4's decision it now
covers RX's narrow CW filtering too. `rx_audio.c`'s stage 1 (wide
complex bandpass, image rejection) and stage 2 (mix to pitch, take the
real part) stay conceptually the same — what changes is stage 3: instead
of the fixed `narrow_filter_coeffs[]` IIR, the local CW monitor's
selectivity comes from the same `filter_tune()`-style FFT stage TX uses,
at the same block size, with pitch and width both exposed as live
parameters (`filter_tune(rx_cw_filter, (pitch-width/2)/Fs, (pitch+width/2)/Fs, beta)`,
recomputed whenever either changes — cheap, no offline design step).

### TX pipeline — one pipeline, CW and SSB both

First draft of this doc gave CW and SSB separate paths (`cw.c`'s
existing keyed-tone mixer, left alone, alongside a new SSB exciter).
Checking `mj_zbitx/src/sbitx.c`'s real `tx_process()` end to end turned
up a better answer: sbitx doesn't run CW through a separate path at
all. One pipeline handles both, and the *only* branch point is what
supplies the block's input samples:

```c
else if (r->mode == MODE_CW || r->mode == MODE_CWR || r->mode == MODE_FT8)
    i_sample = modem_next_sample(r->mode) / 3;
else
    i_sample = (1.0 * input_mic[j]) / 2000000000.0;   // voice modes
```

Everything downstream is shared, unconditionally: the same forward FFT,
the same `fft_out[i] *= tx_filter->fir_coeff[i]` passband multiply, the
same explicit "zero out the LSB" / "zero out the USB" branch (`MODE_CW`
groups with USB's treatment, `MODE_CWR` with LSB's — matching the
pairing minibitx's RX side already uses), and the same final "rotate to
tx_bin" bin-shift that places the result at the IF frequency. maxibitx
should follow this exactly rather than keeping `cw.c`'s TX-carrier path
separate:

```
  Per-sample source, chosen by mode:
  - CW:  iambic-keyer/straight-key envelope's current sample
         (cw.c's existing envelope logic, or the iambic keyer module
         from the earlier keyer design discussion — unchanged either way,
         it just now feeds this pipeline instead of the DAC directly)
  - SSB: mic audio (sound.c already captures this - mic_buf[] exists,
         passed into sound_process() as input_mic, currently
         (void)input_mic;)
  - DIGITAL: real audio from an external digital-mode app (WSJT-X on a
         host PC) - see step 9 below for what was actually built, which
         supersedes this section's original placeholder plan. Rather
         than reaching input_mic over the existing mic/line-in path (the
         plan this bullet originally described), the operator asked for
         a dedicated path instead: usb_gadget.c's UAC2 gadget carries
         real 16-bit/48kHz PCM audio in both directions over the same
         USB link WSJT-X already uses for CAT, and sound.c pulls this
         mode's i_sample source from there (uac_pull_audio_tx(),
         upsampled to 96kHz by upsample48k.c) instead of from mic_buf.
         Kept as a distinct mode value (not reused MODE_USB) purely so
         ALC/power calibration and CAT mode reporting can differ later -
         see §8, §9. Onboard generation/decoding (an actual FT8
         encoder/decoder running on-target) stays deferred; this is only
         about getting a host app's own audio to and from the exciter.
     |
     v
  Shared FFT overlap-save filter                 (port fft_filter.c's
   (passband = one sideband around 0Hz,           filter_new()/filter_tune()/
   mirrored for the other side), + explicit        window_filter() from
   zero of the unwanted sideband's bins            mj_zbitx - the same
     |                                             machinery tx_process()
     v                                             and set_rx_filter()
  Bin-rotate to the IF frequency                   already use for every
   (replaces cw.c's separate TX_IF_OFFSET_HZ        mode, CW included)
   NCO and radio_tx_apply()'s CW_PITCH_HZ
   residual-correction math with one shared
   shift used by every mode)
     |
     v
  Inverse FFT -> DAC right channel, Mixer 2       (unchanged - the existing
   (clk1), crystal filter, Mixer 1 (clk2),         radio_tx_apply() chain
   PA, LPF bank                                    from docs/03_tx_processing_
                                                    pipeline.md doesn't care
                                                    what generated the IF
                                                    signal on the DAC's
                                                    right channel)
```

What this changes concretely:

- **`cw.c` loses its `cw_tx_carrier` NCO and the whole `TX_IF_OFFSET_HZ`/
  residual-correction bookkeeping** (`docs/03_tx_processing_pipeline.md`'s
  own admission that `CW_PITCH_HZ`'s relationship to `xtal_filter_center`
  leaves "a small, real residual... not an oversight" that
  `radio_tx_apply()` has to specially correct for). One shared
  bin-rotation constant places *every* mode's audio at the IF frequency
  instead of two independently-derived numbers that have to happen to
  agree. `cw_tone` (the bare-pitch sidetone oscillator) stays — it still
  feeds `output_speaker` directly, exactly like `tx_process()`'s
  `output_speaker[j] = i_sample * sidetone` for non-voice modes — but it
  now *also* feeds the shared pipeline as `i_sample`, rather than a
  second NCO doing the IF shift itself.
- **CW's TX-side pitch/IF placement becomes as freely retunable as
  SSB's.** Distinct from the earlier RX-audio-monitor pitch discussion
  (`rx_audio.c`'s fixed elliptic filter, still a separate, still-open
  question on the receive side) — this is specifically the TX carrier
  placement, and merging it into the FFT pipeline means changing it is
  just changing what feeds `modem_next_sample()`, no coefficient
  regeneration anywhere.
- **One pipeline to test and maintain instead of two.** The iambic-keyer
  design from the earlier discussion (block-rate paddle read + decision
  logic, per-sample envelope output) is unaffected — it just now hands
  its per-sample output to this pipeline's `i_sample` instead of straight
  to the DAC.

The genuine new cost, not present in `cw.c`'s current direct-to-DAC
approach at all: CW's TX now inherits the FFT overlap-save pipeline's
block-processing latency, and its 5ms Blackman-Harris envelope edges
need to survive block-boundary processing cleanly — see §9.

**Mic audio is not new plumbing** — `sound.c` already captures it every
block (`mic_buf[i] = cap_buf[i*2+1]`, stereo capture, R = Mic by
existing convention/comment) and passes it into `sound_process()`; the
only thing stopping it from being used today is `sound_process()`'s own
`(void)input_mic;` and the WM8731 "Mic" ALSA control being deliberately
muted at startup (`sound_mixer("hw:0", "Mic", 0)`). Both are one-line
changes once the shared pipeline exists to do something with the
signal.

### RX pipeline — the local CW monitor moves onto the same mechanism

`rx_audio.c`'s stage 3 (currently `narrow_filter_coeffs[]`, a fixed
8-pole elliptic bandpass) is replaced by an FFT overlap-save stage built
the same way as TX's — `filter_new()`/`filter_tune()`/`window_filter()`,
same Kaiser-window construction, though NOT the same block/impulse
sizing (see §10 step 6 for why a narrow ~300Hz CW passband needs a much
longer impulse response than TX's wide 300-3000Hz one) — operating on
the already-demodulated audio coming out of stage 2. Two live parameters
instead of zero: pitch (where stage 2 already mixes to, unchanged) and
width (now a real, live-tunable passband argument instead of a baked-in
300Hz). No coefficient regeneration, no rebuild, for either.

**Step 6, done, bench-only:** built as its own module, `src/rx_filter.c`/
`.h` (`rx_filter_new()`/`rx_filter_retune(pitch_hz, width_hz)`/
`rx_filter_process_block()`/`rx_filter_free()`), not a direct reuse of
`tx_pipeline.c` — this filter shapes an already-real signal in place
rather than building a one-sided analytic signal and moving it to an IF,
so it needed a genuinely different passband primitive:
`fft_filter.c`/`.h`'s new `filter_tune_real()`, which passes a mirrored
pair of intervals (`[low,high]` and `[-high,-low]`) instead of
`filter_tune()`'s single one-sided one — a real signal's spectrum is
symmetric about 0Hz by construction, and a plain `filter_tune()` passband
would discard that mirror half as if it were an unwanted image, the same
mistake `tx_pipeline.c`'s sideband-zero step deliberately (and
correctly) makes on purpose for SSB construction. Bench-verified against
synthetic tones (`src/rx_filter_test.c`) — see §10 step 6 for the
measured passband/shape/retune/latency numbers.

**Step 7, done, code-complete:** `rx_filter.c` is wired into
`rx_audio.c` for real — both stage-3 implementations now run
continuously (see rx_audio.c's own comment for why), with
`rx_audio_set_narrow_filter_impl()` picking which one's output reaches
stage 4, elliptic still the default. Reachable remotely via rigctld's
new `u`/`U FFTFILT` and `tools/rigctl_panel.py`'s new checkbox, for the
same on-air A/B comparison this exists for. `src/rx_audio_test.c` is a
new integration smoke test proving this wiring itself (not `rx_filter.c`'s
DSP, already covered by step 6's own harness) - see §10 step 7 for the
numbers and the one bug it caught in the test itself, not the code.

This directly resolves the original "how do we handle `CW_PITCH_HZ`
changing" question from earlier in this project: on RX, changing pitch
now just means re-tuning stage 2's existing mixing oscillator *and*
re-calling `rx_filter_retune()` with the new center — both cheap, both
already-necessary operations, no separate offline `scipy.signal.ellip`
step to keep in sync with the pitch value the way `narrow_filter_coeffs[]`
required. Width becomes a second, independent live control the fixed
IIR never offered at all.

## 6. Mode selection becomes real

**Step 3, done:** `m`/`M` (rigctld) and `MD` (Kenwood CAT) used to be
cosmetic-only — stored, never acted on, "since minibitx has no onboard
demod." Both now agree on one real, single owner in `radio.c`
(`radio_set_mode()`/`radio_get_mode()`, `CW`/`USB`/`LSB`/`DIGITAL` for
v1 — `DIGITAL` a placeholder value only, see §5) — the exact pattern RIT
already established in minibitx, applied to mode instead of a tuning
offset. See §10 step 3 for the translation-table details on each
surface and the (harmless) default-mismatch bug this fixed.

**Partly done as of step 5:** the shared TX pipeline is live for CW
(`sound.c`'s audio thread now feeds `cw_get_sample()`'s output through
`tx_pipeline_process_block()` every TX block, replacing `cw.c`'s old
direct-to-DAC path) - but `sound.c` still only ever calls it with a
hardcoded `TX_PIPELINE_KEEP_UPPER` sideband, gated purely on
`cw_tx_active()`, not on `radio_get_mode()`. **Still not done:** actually
reading that real mode value to choose which `i_sample` source feeds the
pipeline (the keyer's envelope output for CW - already wired - vs. mic
audio for USB/LSB, and for `DIGITAL` the same mic/line-in audio as SSB,
sourced from a host PC's digital-mode app instead of a human voice) and
which sideband-zero branch applies - the same single mode check
`tx_process()` already uses for these decisions. That dispatch has
nothing to select between yet, since CW is still the only mode with any
`i_sample` source wired in at all - it becomes real work once SSB's
mic-audio path exists as a second option to choose between (needed
before step 8's on-air SSB test, though not itself a separately-numbered
step in §10 yet); mode being real (step 3) was the precondition for
that, not this work itself.

**Open question, not yet answered by the existing hardware layer: what
triggers PTT for voice?** `DIGITAL` doesn't add a new answer here —
WSJT-X and friends key a rig the same way any CAT client does
(rigctld's PTT command), so this is the identical open question, not a
second one. CW's PTT trigger is `cw.c`'s own key-poll loop
watching a physical GPIO line (`CW_KEY`). There's no equivalent for
voice today — `EXT_PTT` (GPIO12) is wired as an *output* (the relay/PA
gate `radio_hw_set_ptt()` drives), not an input footswitch or mic-PTT
button. Two paths, not mutually exclusive: (a) remote/CAT PTT already
works unchanged today (`hamlib.c`'s `T`, `usb_gadget.c`'s `TX`/`RX`/
`TQ` already call `radio_set_tx()` generically) — enough to bench-test
SSB TX with no new wiring at all; (b) a physical PTT input (footswitch
or mic button) would need a new GPIO input line in `radio_hw.c`,
mirroring `CW_KEY`'s pattern, if/when a mic is actually plugged in on
the bench rather than tested via CAT alone.

**Precedent worth reusing when paddle input is added: real sbitx shares
one GPIO between mic-PTT and the paddle's DOT line.** Verified directly
in `mj_zbitx/src/sbitx_gtk.c`: `#define PTT (7)` and `#define DASH (21)`
(both `pinMode(..., INPUT)`), read by a plain polled `key_poll()`:

```c
int key_poll() {
  int key = CW_IDLE;
  int input_method = get_cw_input_method();
  if (input_method == CW_STRAIGHT) {
    if ((digitalRead(PTT) == LOW) || (digitalRead(DASH) == LOW))
      key = CW_DOWN;
  } else {
    if (digitalRead(PTT) == LOW) key |= CW_DASH;
    if (digitalRead(DASH) == LOW) key |= CW_DOT;
    if (key == (CW_DASH | CW_DOT)) key = CW_SQUEEZE;
  }
  return key;
}
```

Same physical pin, two meanings, picked by the `CW INPUT` setting
(`get_cw_input_method()`): straight key reads it as a key-down line;
iambic paddle reads the same line as DOT (with GPIO 21 as DASH) and
OR's in a squeeze state. That's a real, shipped way to get mic-PTT and
one paddle contact from a single input pin — directly applicable if
maxibitx ever wants paddle input without burning two new GPIO lines
beyond what a mic-PTT footswitch would already need.

One correction to a claim that came in secondhand alongside this: it's
been described elsewhere as interrupt-driven, timing a `key_isr()`.
That function doesn't exist anywhere in `mj_zbitx` — `key_poll()` above
is the entire mechanism, a plain `digitalRead()` pair, called from
`cw_poll()` at whatever cadence the caller provides (this session
separately confirmed `sbitx_gtk.c` calls it every `ui_tick()`,
nominally 1ms, specifically during CW modes, precisely *because* plain
polling would otherwise be too coarse for iambic timing — not an ISR
substituting for that need). That's good news for maxibitx's own
plan: the earlier iambic-keyer design (block-rate paddle read +
decision logic, feeding `cw_set_key_state()`) is the same kind of
polling real sbitx actually ships, not a simplification of some
interrupt-driven scheme that would need to be matched. (`EXT_PTT`,
GPIO 26 in this same repo, is unrelated — a KF7YDU-added *output* pin
for keying an external accessory's PTT, not this input line.)

## 7. Day-one requirements carried in from §2, made concrete

- RT scheduling and the correct capabilities (`cap_sys_nice`, not just
  `cap_sys_time`) go into the build/deployment script from the start,
  not discovered under time pressure after underruns show up on
  hardware.
- `PERIOD_FRAMES`/ALSA period sizing gets re-validated once the shared
  FFT pipeline adds real per-block compute — minibitx's current value is
  already bench-tuned for the existing (lighter) RX+CW-TX load, not for
  a heavier TX-side filter running every block, and this now applies to
  CW's TX path too (§5), not just SSB's.
- No spectrum/meter/waveform-display computation goes inside the new
  TX audio function itself — any of that lives in its own consumer,
  throttled, reading a copy.
- Every piece of state the new TX audio path reads gets a one-line
  comment on its actual production rate at the read site if it isn't
  obviously audio-rate already (the `cw_key_state`/`cached_pitch`
  lesson).
- No blocking calls (network, IPC, filesystem) anywhere in or adjacent
  to the audio thread.
- **BFO/LO-leakage — flagged as needing a bench check, not assumed
  solved.** The zbitx 24kHz spur traced to the BFO (si5351) running
  continuously through CW's inter-element key-up gaps while PTT stayed
  asserted. minibitx's own `radio_tx_apply()` holds `clk1`/PTT/the relay
  asserted for the *whole* keying burst too (`CW_HANG_POLLS` keeps
  things up across inter-character gaps, same structural shape as
  zbitx's exposure) — worth a spectrum-analyzer check on minibitx's own
  CW TX before assuming this is already fixed by the architecture split,
  and the same question applies to natural pauses/breaths in SSB.

## 8. Explicitly deferred (not v1)

- Onboard digital-mode TX/RX (FT8 etc. generated/decoded on-target
  instead of by an app on a host PC) — bigger DSP lift, revisit after
  SSB proves out. Note this is narrower than it sounds: `DIGITAL` as a
  *mode value* (host-PC-generated audio riding the existing mic/line-in
  path into the shared pipeline, §5) is in v1 - only the on-target
  encoder/decoder is deferred.
- AM/FM — not relevant to this HF hardware.
- Any local UI/display — stays a separate process if it happens at all
  (§2).
- RX DSP *quality* work (ANR/noise reduction) beyond what minibitx's
  `rx_audio.c` already has — §2's note on starting from the corrected
  algorithm applies whenever this does get picked up, but it's not part
  of getting SSB TX working. This is distinct from the stage-3 narrow
  filter's *architecture* migrating onto the shared FFT pipeline (§5) —
  that migration is in v1 scope (it's what makes CW pitch/width live
  controls), it just isn't a new noise-reduction feature.

## 9. Open questions needing bench time, not more design

- **Porting `fft_filter.c`'s overlap-save filter, not redesigning it.**
  Block size, Kaiser beta, and passband edges are already bench-chosen
  in `mj_zbitx` (`filter_new(1024, 1025)`, `kaiser_beta=5`, 300-3000Hz
  edges) — start from those, not a fresh design. What *does* need
  checking on minibitx's hardware: FFT overlap-save block processing
  adds real algorithmic latency (roughly a block's worth of samples —
  at a 1024-sample block/96kHz that's in the same ~10ms range as
  minibitx's existing `PERIOD_FRAMES` cadence), which a short
  time-domain FIR wouldn't have added as much of. Fine for voice, but
  worth confirming against the real-time audio budget alongside §7's
  other "re-validate `PERIOD_FRAMES` once real per-block compute exists"
  item, since this is exactly that kind of new per-block compute. Step
  6's `rx_filter.c` gave this a real, measured number on the RX side
  specifically (not just TX): **16.00ms** group delay (bench-measured
  via an impulse response, not just the `(M-1)/2` formula), against the
  elliptic IIR's ~2.6ms — a real ~6x latency cost for the local CW
  monitor specifically, worth an actual on-air listening judgment (step
  7) rather than assuming "fine for voice" also means "fine for a CW
  operator's own sidetone-adjacent monitor," since nothing bench-only
  can settle that.
- **Actual achievable sideband rejection.** CW measured ~40dB against a
  filter-theoretic ~61dB, attributed to this specific board's filter
  unit and diode-mixer nonlinearity, not the architecture. SSB's
  rejection ceiling is set by how cleanly the FFT stage's explicit
  bin-zeroing and filter passband edges hold up against real voice
  audio's full spectrum (a new source of error CW's single tone never
  exercised), not by the crystal filter at all this time — needs its
  own bench characterization once Stage A/B exist, not an assumption
  that CW's ~40dB carries over.
- **Power calibration for continuous voice vs. keyed carrier.**
  `TX_GAIN_CORRECTION`/the per-band scale table were bench-derived
  against a steady CW carrier. Voice has a fundamentally different
  peak-to-average ratio and needs its own ALC/compression story before
  the existing `TX_SAMPLE_CLAMP`/headroom constants can be trusted for
  it. `DIGITAL` will need a third profile, not voice's: a multi-tone or
  single-tone digital-mode signal runs near-constant envelope at
  close to 100% duty cycle, closer to CW's steady-carrier case than to
  speech's peaky one, but sourced through the mic/line-in path with
  whatever gain-staging voice assumes - worth a bench check of its own
  once `DIGITAL` actually has a real source feeding it, not an
  assumption that either existing calibration carries over.
- **PTT source for bench testing** — §6's open question; likely
  resolved simply (CAT-only for first bench tests) rather than needing
  new GPIO wiring immediately.
- **On-air listening check for the wider RX skirt, not just the math.**
  The ~300Hz-vs-~135Hz-per-side transition-width difference (§4) is a
  real, computed number, but "still usable for single-signal CW copy in
  real band congestion" is a listening-quality judgment the math alone
  doesn't settle — bench/on-air check this against minibitx's existing
  elliptic filter before treating the wider skirt as a non-issue.
- **Width control's real range.** Width becomes a live parameter (§5),
  but how narrow can an operator actually dial it before running into
  the shared block's own frequency resolution (`Fs/N` per bin) as a
  hard floor, and how wide before the transition eats meaningfully into
  the passband — needs bench-mapping once the shared filter exists, not
  assumed from the nominal design point alone.
- **CW's envelope across FFT block boundaries.** `cw.c`'s 5ms
  Blackman-Harris rise/fall has never had to survive a processing block
  boundary before — merging CW into the shared pipeline (§5) means it
  now does, every keying transition. Overlap-save's own carry-over
  mechanism (`fft_m[]`, the "previous M samples" continuity `tx_process()`
  already handles) should account for this — it's shipped, working CW
  on real sbitx hardware — but it's a new class of thing for maxibitx to
  bench-verify rather than assume, given `cw.c`'s current direct-to-DAC
  path has no block boundary to cross at all.
- **`fftw3` as a new real dependency.** Merging CW in means the TX path
  always runs the FFT pipeline, not just for SSB — a real library
  dependency and per-block CPU cost minibitx's codebase doesn't carry
  today. Not new risk (it's exactly what already runs in production on
  both the Pi Zero 2W and Pi 4 zbitx boards), but it's a cost class
  maxibitx didn't have before, and it folds into the `PERIOD_FRAMES`
  re-validation above.
- **`FFTW_MEASURE` plan-creation cost at startup, not just per-block
  cost while running.** Confirmed as a real, user-visible issue on
  first on-air test (§10 step 7's follow-up entry) — two separate
  `FFTW_MEASURE` searches (`tx_pipeline_new()` at N=2048,
  `rx_filter_new()` at N=4096) both run during `maxibitx`'s own startup
  sequence, with no wisdom-file cache to amortize either across process
  restarts (this section's own first bullet already flagged the missing
  cache; this is that cost actually showing up as a noticeable startup
  delay, not just a theoretical one). `rx_filter.c` now uses
  `FFTW_ESTIMATE` instead (via the new `filter_new_ex()`), which removed
  its share of the delay — construction time dropped from ~240ms to
  ~0.2ms on this project's dev machine (not the user's actual Pi Zero
  2W — still needs on-hardware confirmation) with per-block cost
  unaffected (~0.016ms either way, against a ~10.667ms budget).
  `tx_pipeline_new()` now makes the same trade (§10 step 7's second
  follow-up entry) — turned out not to be optional once a real xrun
  flood traced back to exactly this search running, synchronously,
  *after* `sound_thread_start()` primed the playback buffer but *before*
  the audio thread that keeps it fed was created, long enough (186.7ms
  measured on this dev machine, vs. the primed buffer's own ~42.7ms) to
  drain that buffer before real writes ever resumed. A real wisdom-file
  cache (sbitx's own `WISDOM_MODE`) would still be the more complete fix
  if `FFTW_ESTIMATE`'s per-block cost (checked and fine on this dev
  machine for both N=2048 and N=4096, not yet independently confirmed on
  the user's Pi) ever turns out to matter on some board — still not
  implemented.

## 10. Proposed build order

1. **Done.** Bootstrap the repo from minibitx's control/hardware layer,
   unchanged (`radio.c`, `radio_hw.c`, `gpio.c`, `si5351v2.c`, `vfo.c`,
   `hamlib.c`, `usb_gadget.c`, `hpsdr_p1.c`, `sound.c`) — RX and CW TX
   (still `cw.c`'s current direct-to-DAC path at this point) should
   build and run exactly as minibitx does, proving the fork didn't
   break anything before any new code exists. Not yet verified by an
   actual build/bring-up on real hardware — do that before starting
   step 2.
2. **Done, bench-only.** Ported `fft_filter.c`'s design math
   (`make_kaiser`/`window_filter`/`filter_tune`) and wrote a
   self-contained `struct filter` (`src/fft_filter.c`/`.h`) that owns
   its own overlap-save state and FFTW plans per instance, instead of
   sbitx's shared global buffers — see the file headers for why. Bench-
   verified against synthetic complex tones before ever touching real
   hardware, the same methodology `test_rx_audio.c` uses
   (`src/fft_filter_test.c`, `make test-fft-filter && ./test-fft-filter`,
   itself not part of the shipped binary — same convention). Real
   numbers, at minibitx's actual Fs=96000/L=1024/M=1025:
   - **Passband**: 0.00dB (a tone at 1500Hz in a 300–3000Hz band).
   - **Stopband**: real sbitx's own `filter_tune()` leaves every
     filter's passband gain at an uncorrected +20·log10(N) (+66dB at
     this N) that its `rx_linear()`/`tx_process()` never visibly
     compensates for — presumably absorbed, unremarked, into whatever
     other empirically-bench-tuned gain constant happens to follow it.
     Caught here by literally measuring +66.23dB where 0dB was
     expected; fixed by using `1/N²` in `filter_tune()`'s initial gain
     instead of sbitx's `1/N` (one of the two FFT round-trips inside
     `window_filter()` was going uncompensated) — verified back to
     0.00dB. This port's filter is unity-gain by construction; any gain
     constant added later (step 9) calibrates real analog/mixer gain,
     not partly undoing an unlabeled FFT-normalization artifact too.
   - **Sideband separation (the central decision, §4)**: two tones at
     ±1031.25Hz inside a shared ±3000Hz passband — without the explicit
     bin-zero step, both measure ~0dB (the filter alone genuinely can't
     tell them apart, confirming it's the zero doing the work, not the
     filter's own rolloff); with it, the wanted tone stays at 0dB and
     the unwanted one drops to **-164dB** — real numbers behind the
     "explicit zero gives near-perfect rejection, not just a filter's
     finite asymptotic rolloff" reasoning in §4.
   - **Transition width**: crosses -54dB roughly 340Hz past a 3000Hz
     edge (interpolated between measured points) — close to §4's
     ~300Hz/side Kaiser-formula estimate, confirming that math held up
     against the real ported implementation, not just on paper.
   - New dependency confirmed at build time, not just anticipated:
     `libfftw3-dev`, linked via `-lfftw3f` (single precision) — not yet
     installed/tested on an actual Pi, only in this bench environment.
   Not yet done: wiring this into the real pipeline (later steps in this
   build order), or a wisdom-file cache for `filter_new()`'s
   `FFTW_MEASURE` plans (see `fft_filter.c`'s comment on why that's
   deferred, not skipped).
3. **Done, state only.** `radio_set_mode()`/`radio_get_mode()` added to
   `radio.c` (`enum radio_mode`: `CW`/`USB`/`LSB`/`DIGITAL`) and wired
   into both control surfaces — `hamlib.c`'s `m`/`M` (translated to/from
   Hamlib mode names, an unrecognized name now rejected with `RPRT -1`)
   and `usb_gadget.c`'s `MD` (translated to/from single-digit Kenwood
   codes, an unrecognized digit silently ignored — that surface's own
   convention). Fixes a real, if harmless, pre-existing bug this
   surfaced: the two control surfaces' old independently-cosmetic mode
   variables defaulted to different values (`hamlib.c`: `USB`;
   `usb_gadget.c`'s CAT surface: `CW`) that could never have agreed with
   each other even by coincidence, since nothing tied them together.
   Now there's one real value both agree on, defaulting to `CW` — the
   one mode minibitx can actually transmit. Still doesn't gate anything
   downstream — no `i_sample` source or sideband-zero branch exists to
   gate until step 4 wires the shared pipeline into `sound.c`/`cw.c`.
   `PKTUSB`/Kenwood digit `9` for `RADIO_MODE_DIGITAL` are both
   best-effort guesses (see `hamlib.c`/`usb_gadget.c`'s own comments),
   not confirmed against a real WSJT-X rigctld session or a QMX packet
   capture — low-stakes for now since nothing reads `RADIO_MODE_DIGITAL`
   yet either.
   - **Follow-up: `tools/rigctl_panel.py` could never actually select
     `DIGITAL` at all.** While troubleshooting the FT8 decode-count
     investigation above (step 9's follow-up), clicking `DIGITAL` in the
     panel's mode selector appeared to instantly "snap back" to `USB`.
     Root cause: `hamlib.c`'s `name_to_mode()` only ever recognized this
     step's own `PKTUSB` (the real Hamlib name for `RADIO_MODE_DIGITAL`,
     chosen for compatibility with genuine Hamlib clients) - the panel
     sends `"M DIGITAL 2400"` by name (its own friendlier convention,
     predating this cross-check), which `name_to_mode()` had never
     recognized, so `set_mode` rejected it with `RPRT -1` and
     `radio_set_mode()` was never even called. The very next poll's `m`
     query then correctly reported whatever mode was already active
     (typically `USB`) - not a display glitch, the mode change had
     genuinely never happened, every single time `DIGITAL` was clicked.
     **Fixed on both ends of the mismatch:** `name_to_mode()` now also
     accepts `"DIGITAL"` as an alias for `RADIO_MODE_DIGITAL` (alongside
     `"PKTUSB"`, unchanged); `mode_to_name()` still reports `"PKTUSB"` on
     the way out (kept for real Hamlib-client compatibility, same
     reasoning as when this step chose it), so `rigctl_panel.py`'s own
     `apply_mode()` now maps a `"PKTUSB"` reply back to displaying
     `"DIGITAL"` - the half of the fix that makes the radio button
     actually hold once selected, instead of matching nothing in its own
     name list and freezing on whatever was last shown. Verified with an
     isolated test replicating both functions' exact logic (case
     sensitivity, the unchanged CW/USB/LSB/PKTUSB paths, and a full
     click→set_mode→poll→display round trip matching the real sequence)
     plus the panel's own patched `apply_mode()` logic in Python -
     real end-to-end verification needs the actual gadget/rigctld
     running, not available in this environment. Full rebuild clean
     under `-Wall -Wextra`; every existing DSP test harness still
     passes unchanged (none of this touches `rx_audio.c`).
4. **Done, bench-only.** Wrote a new, parallel shared TX pipeline module
   (`src/tx_pipeline.c`/`.h`) implementing §5's plan for CW's own slice
   of it: `fft_filter.c`'s passband filter (300-3000Hz, beta 5 — the
   CW/USB-grouped passband real sbitx's own `tx_process()` uses, per §9),
   an explicit sideband-zero (CW groups with USB's "keep the upper half"
   treatment), and a shared IF bin-rotate meant to eventually replace
   `cw.c`'s own `cw_tx_carrier`/`TX_IF_OFFSET_HZ` NCO *and*
   `radio_tx_apply()`'s `CW_PITCH_HZ` residual correction on clk2 — not
   just the NCO. Bench-verified against a synthetic stand-in for `cw.c`'s
   real sidetone (`src/tx_pipeline_test.c`,
   `make test-tx-pipeline && ./test-tx-pipeline`, same "not part of the
   shipped binary" convention as step 2's harness). **`cw.c`/`radio.c`/
   `sound.c` are completely untouched by this step** — this is a proven,
   parallel replacement, not a live one; the real running binary still
   transmits CW exactly as it did after step 1, unchanged. Real numbers:
   - **IF placement**: the ideal shift (`bfo_freq - xtal_filter_center -
     CW_PITCH_HZ` = 22600 - 700 = 21900Hz — landing the wanted mixing
     product exactly on `xtal_filter_center`, with **no** residual
     correction needed anywhere downstream) rounds to the nearest bin at
     this pipeline's Fs/N = 46.875Hz resolution: 467 bins = 21890.625Hz
     actual, a **9.375Hz residual** — a real, honestly-quantified cost of
     moving IF placement into the FFT domain, but roughly **75x smaller**
     than the old direct-NCO scheme's fixed 700Hz residual
     (`docs/03_tx_processing_pipeline.md`'s "Known limitations"), and
     small enough that it may not need a compensating correction at all
     once this is ever wired in live (to be confirmed on air, not
     assumed).
   - **Placement/gain accuracy (Case A)**: the wanted tone measures
     **0.00dB** at the predicted 22590.625Hz (700 + 21890.625) — exactly
     where the derivation above predicts, at unity gain.
   - **Sideband/image rejection (Case B)**: using CW_PITCH_HZ's real,
     non-bin-aligned 700Hz (not a substituted bin-aligned stand-in, since
     this is meant to prove the actual system, not an idealized one) and
     a long (64-block, ~680ms) coherent integration to get a clean
     reading despite that: without the explicit zero, wanted and image
     measure identically (0dB each — the filter alone genuinely can't
     tell them apart, confirming the zero is what does the work, not the
     filter's rolloff, the same finding step 2's own case C already
     made); with it, the image drops to **-70.05dB** — comfortably past
     the crystal filter's own ~40dB analog rejection this whole scheme is
     meant to preserve or improve on.
   - **Block-boundary sanity (Case C, §9's flagged check)**: a synthetic
     480-sample linear key-up ramp (standing in for `cw.c`'s real
     Blackman-Harris envelope, private to that file) straddling an
     internal 1024-sample block-processing seam shows no localized
     discontinuity — max sample-to-sample 2nd-derivative magnitude
     within ±8 samples of the seam (1.812) is not larger than the max
     across the whole ramp (1.821); overlap-save's history carry-over
     handles an amplitude change across the boundary as cleanly as it
     already handled a steady tone in step 2.
   - **Two real bugs caught by this bench harness before either would
     have reached hardware** (the same discipline that caught step 2's
     gain bug): (1) a naive per-block bin-rotate is not phase-continuous
     across block boundaries unless `shift_bins * L / N` is an integer —
     it isn't here (`L/N` is exactly 1/2 by this pipeline's fixed
     1024/1025 sizing, and 467 is odd), so an early version silently
     flipped the carrier's sign 180° every other block, measuring the
     wanted tone at **-97.75dB** instead of ~0dB; fixed with a per-block
     correction that, thanks to `L/N=1/2` exactly, collapses to a plain
     alternating ±1 (no trig, no float drift) rather than a general
     running-phase NCO. (2) constructing single-sideband audio by
     zeroing one spectral half and taking the real part inherently halves
     the amplitude (a real input tone splits into two equal-amplitude
     complex exponentials; discarding one and taking the real part of
     what's left reproduces only that surviving half) — measured as a
     real **-6.02dB**, fixed with an explicit ×2 gain, keeping this
     pipeline unity-gain by construction rather than letting a future,
     unrelated calibration constant silently absorb it (exactly the
     failure mode step 2's writeup above already flags in real sbitx's
     own `filter_tune()`).
   Not yet done: actually wiring this into `cw.c`/`sound.c` for a live
   CW cutover (step 5 below), or retuning/reusing this module for SSB's
   mic-audio `i_sample` source (`tx_pipeline_retune()` exists for LSB's
   mirrored passband but is untested — nothing has called it yet).
5. **Done, confirmed on air** - CW through this path is on frequency,
   with image suppression as predicted and a flat ~5 W across the nine
   bands.
   Wired `tx_pipeline.c` into `cw.c`/`sound.c` for CW only: the actual
   live cutover from `cw.c`'s direct-to-DAC path to step 4's
   bench-proven module.
   - `cw.c` lost `cw_tx_carrier`/`TX_IF_OFFSET_HZ` entirely -
     `cw_get_sample()` is now the only tone this file generates, feeding
     both the local sidetone monitor and (as `i_sample`)
     `tx_pipeline.c`'s shared pipeline, matching real sbitx's own
     `output_speaker[j] = i_sample * sidetone` pattern (§5).
   - `radio_tx_apply()` (radio.c) lost its `- CW_PITCH_HZ` residual
     correction on clk2 - TX now uses the exact same
     `freq_hdr + xtal_filter_center` formula RX does, since
     `tx_pipeline.c`'s bin-rotate already aims directly at
     `xtal_filter_center` (step 4's derivation).
   - `sound.c`'s audio thread: `cw_get_sample()` is still called exactly
     once per sample (its envelope advance depends on that), but its
     result is now collected into a `TX_PIPELINE_BLOCK_LEN`-sized buffer
     and run through `tx_pipeline_process_block()` once per audio block
     (`TX_PIPELINE_KEEP_UPPER`, matching CW's sideband) instead of
     reading a second, IF-shifted sample directly. A block whose actual
     size doesn't match `TX_PIPELINE_BLOCK_LEN` (should only happen on a
     genuinely abnormal short/interrupted ALSA read, since
     `PERIOD_FRAMES` is negotiated to match `TX_PIPELINE_BLOCK_LEN` by
     design) skips the pipeline for that one block rather than risk
     feeding it a misaligned overlap-save history - the exciter output
     goes silent for that block only, sidetone unaffected, logged once.
   - `fft_filter.c`/`tx_pipeline.c` moved from bench-only into the real
     build (`Makefile`'s `SRC`/`OBJ`, `-lfftw3f` added to `LDFLAGS`) -
     `libfftw3-dev`/`libfftw3f` is now a real runtime dependency of the
     shipped `maxibitx` binary, not just this bench environment's.
   **What's genuinely still open, not yet bench- or air-verified:**
   whether the new path reproduces the *same transmitted power* as the
   old one under a real keyed envelope, not just the same amplitude for
   a steady bench tone (step 4's Case A measured 0.00dB for a *steady*
   full-scale tone; an envelope's own spectral content should sit well
   inside the 300-3000Hz passband and transfer the same way, but that's
   an inference from the steady-tone data, not something separately
   measured) - `TX_GAIN_CORRECTION` (0.045, `sound.c`) was bench-derived
   against the old scheme's output and is carried over unchanged on that
   basis; a wattmeter re-check at low drive is worth doing on first
   power-up, same as this project's own precedent for any change that
   could shift gain (step 2's writeup above), even though it isn't
   treated as a hard blocker here. On-air re-verification against the
   same dial-accuracy and ~40dB image-rejection numbers
   `docs/03_tx_processing_pipeline.md` recorded for the path just
   replaced is the other piece — the explicit live-CW re-confirmation
   checkpoint the original version of this build order never actually
   had (it jumped straight from "still bench-only" to RX/SSB work below
   without ever specifying a step that both wires CW in live *and*
   checks it on air).

   **Dial accuracy confirmed on air.** First real CW transmission on the
   new live pipeline was copied by a remote receiver and reported exactly
   on frequency — direct confirmation that `tx_pipeline.c`'s bin-rotate
   (aimed at `xtal_filter_center` per step 4's derivation) and
   `radio_tx_apply()`'s simplified clk2 math (no more `- CW_PITCH_HZ`
   residual) land the transmitted signal where the dial says, with a real
   receiver on the other end rather than a bench measurement. Pairs with
   step 7's RX-side W1AW confirmation — both directions of this rebuild
   are now dial-accurate on real hardware.

   **Image rejection confirmed on air.** A first attempt tuned a remote
   receiver to dial ∓700Hz (7,019,300 / 7,020,700Hz while transmitting on
   7,020,000) and heard/saw only the wanted signal — but working through
   this pipeline's actual mixer chain shows that's the wrong offset to
   test: the real/-70Hz sidetone splits into a +700Hz and a −700Hz
   spectral component before `zero_sideband()` runs, both of which ride
   the *same* bin-rotate through both real analog mixers, so a leaked
   image (if `zero_sideband()`'s bench-measured ~-70dB suppression isn't
   perfect) stays a fixed 1400Hz from the wanted carrier throughout — not
   the ±700Hz a naive "mirror the CW pitch" guess would predict. Using
   this session's actual `bfo_freq`/`xtal_filter_center`
   (40,035,000/40,012,400Hz, confirmed in `data/hw_settings.ini`), that
   places the image at dial **minus** 1400Hz, ≈7,018,600Hz for this test
   — not symmetric around dial at all, and on the opposite side from
   both of the first attempt's guesses. Retuned there: **nothing audible
   or visible**, exactly the expected result at −70dB below a 5W note
   (roughly ten million times weaker in power) — well below what any
   ordinary receiver would pull out of the band noise, even one tuned
   right to the correct spot. This is a real, targeted on-air
   confirmation of the bench-measured image rejection, not just an
   absence-of-evidence result from testing the wrong frequency the first
   time. Step 5 has no remaining open on-air items.

   **Transmitted power confirmed on a real wattmeter, all nine bands.**
   Key-down at 7.030MHz (40m, `TX_DRIVE`=50, `scale`=0.00112,
   `TX_GAIN_CORRECTION`=0.045 — the same values `tx_power_calibration.md`
   calibrated for the old direct-NCO scheme) reads 4.9W into the
   wattmeter, ±0.2W across repeated key-downs. This directly answers the
   "genuinely still open" question above: `tx_pipeline.c`'s real, keyed
   CW envelope carries the same transmitted power through the new
   pipeline as the old scheme did, not just the same amplitude for a
   steady bench tone — `TX_GAIN_CORRECTION` needs no adjustment for the
   new path. The remaining eight bands were then run with the same,
   unchanged `scale` values (`tx_power_calibration.md` §8 has the full
   table): readings span ~5.0-6.2W, some a bit above the old scheme's
   original 4.7-5.5W bench target but all comfortably inside this
   board's 20+W PA safety rating — accepted as-is, a deliberately
   conservative level for ongoing development rather than something
   needing a fresh bisection.
6. **Done, bench-only.** Wrote a new, parallel module (`src/rx_filter.c`/`.h`)
   implementing §5's RX plan: the same `fft_filter.c` overlap-save engine
   TX uses, with pitch and width as live `rx_filter_retune()` parameters
   instead of a baked-in, offline-designed response. Bench-verified
   against synthetic tones (`src/rx_filter_test.c`,
   `make test-rx-filter && ./test-rx-filter`, same "not part of the
   shipped binary" convention as steps 2/4's harnesses). **`rx_audio.c`
   is completely untouched by this step** — same discipline step 4 used
   for TX: this is a proven, parallel replacement, not a live one; the
   real running binary still demodulates CW with `narrow_filter_coeffs[]`
   exactly as before. Required one genuinely new shared primitive:
   `fft_filter.c` gained `filter_tune_real()` alongside the existing
   `filter_tune()` — a real, symmetric-around-0 bandpass (passes
   `[low,high]` *and* its mirror `[-high,-low]`), needed because this
   filter shapes an already-real signal (rx_audio.c stage 2's demodulated
   audio), whose spectrum is inherently symmetric about 0Hz, unlike TX's
   one-sided SSB/analytic-signal construction. `filter_tune()`'s own
   behavior is unchanged (verified: `test-fft-filter`/`test-tx-pipeline`
   re-run after the refactor, matching pre-refactor numbers to within
   run-to-run `FFTW_MEASURE` plan-selection noise — confirmed by running
   `test-fft-filter` three times back to back and seeing its own deep-
   rejection number (Case C, "with bin-zero") swing between -155.86dB
   and -164.44dB with **zero code changes between runs**, i.e. that
   noise is inherent to `FFTW_MEASURE`'s algorithm-selection at the
   single-precision floating-point floor, not something this refactor
   introduced). Real numbers, at Fs=96000Hz:
   - **Sizing**: unlike TX (`L=1024/M=1025/N=2048`), this filter keeps
     `L=1024` (must match `sound.c`'s `PERIOD_FRAMES`) but uses a much
     longer impulse response, `M=3073` (`N=4096`, chosen to land on a
     power-of-two FFT size) — TX's `M=1025` gives a ~340Hz transition
     (step 2's bench number), fine against a 2700Hz-wide 300-3000Hz
     passband but far too wide relative to a ~300Hz-wide CW passband;
     `M` was scaled up from a Harris-formula estimate targeting ~100Hz,
     then bench-verified below rather than trusted from the formula
     alone.
   - **Passband/real-output check (Case A)**: the tuned pitch (700Hz,
     default width 300Hz) measures **0.00dB** — and critically, the
     pre-`crealf()` complex output's imaginary residual is negligible
     (`imag/real` ratio **1.36e-07**), confirming this filter does *not*
     need `tx_pipeline.c`'s `×2` amplitude-halving correction — expected,
     since `filter_tune_real()`'s mirrored passband keeps both spectral
     halves of a real signal intact, unlike TX's deliberate one-sided
     sideband-zero construction, but checked directly here rather than
     assumed.
   - **Shape (Case B)**: measured **-3dB at ±133.7Hz** (267Hz total,
     close to the ~300Hz design target), **-60dB at ±272.6Hz**, shape
     factor **2.04:1** (-60dB width : -3dB width — not a perfectly
     apples-to-apples comparison with the elliptic's own quoted
     -60dB:-6dB ~1.9:1, but in the same range); rejection at
     pitch+3000Hz (the elliptic's own bench comparison point) measures
     **-86.78dB**.
   - **Live retune (Case C)**: `rx_filter_retune()` from
     (pitch=700, width=300) to (pitch=800, width=150) measurably moves
     the passband at runtime — 700Hz drops from 0.00dB to **-9.05dB**,
     800Hz goes from -0.67dB to **-0.26dB** — stronger verification than
     `tx_pipeline_retune()` ever got (that one has still never been
     called outside its own declaration, per step 4).
   - **Group delay (Case D)**: measured empirically via an impulse-
     response peak (not the `(M-1)/2` formula alone, though it matches
     that prediction exactly) at **16.00ms** — a real, honestly-
     quantified new cost versus the elliptic IIR it's replacing
     (~2.6ms, `rx_audio.c`'s own header) — about 6x more local-monitor
     delay. Likely inconsequential for ordinary CW copy (nothing in
     `cw.c`'s own keying timing depends on this — it only affects the
     receive audio a human ear listens to) but worth noting for anyone
     doing tight QSK/full-breakin operating; not yet judged on air.
   Not yet done: wiring this into `rx_audio.c` for a live cutover and an
   on-air listening comparison against the existing elliptic filter
   (step 7 below) — the explicit split step 4/5 already used for TX,
   since "sounds at least as good on a real signal in real band
   conditions" (§9's skirt-quality check) is a listening-quality
   judgment only the user can make on his own hardware, not something
   this bench harness can settle on synthetic tones alone.
7. **Done; the on-air comparison has since been made** - the FFT
   filter sounded worse than the elliptic on real signals (wideband
   hiss, far-off-centre signals audible), which no bench measurement
   could reproduce, so the elliptic stays the default and both remain
   selectable. Full record:
   [`dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md`](dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md).
   Wired `rx_filter.c` into `rx_audio.c`'s stage 3, keeping the existing
   elliptic filter selectable (not a hard cutover like step 5's TX
   decision — §10 step 6's own build-order text always planned for an
   on-air *comparison*, not an immediate replacement), so the operator
   can A/B the two on real signals in real band conditions before
   `narrow_filter_coeffs[]` is ever removed for good, per §9's
   skirt-quality open question.
   - `rx_audio.c` gained a second stage-3 implementation selector
     (`rx_audio_set_narrow_filter_impl()`/`rx_audio_get_narrow_filter_impl()`,
     `rx_audio.h`) — elliptic stays the default. `rx_audio_process()`
     now buffers stage 2's output across the whole call and runs BOTH
     implementations every time (the elliptic filter per-sample as
     before, the FFT filter once per block via `rx_filter_process_block()`),
     regardless of which one is currently selected — same "keep it warm
     so switching doesn't thump" reasoning the enable/bypass toggle
     already used for the elliptic filter alone, now covering a switch
     *between* implementations too. A block whose size doesn't match
     `RX_FILTER_BLOCK_LEN` (same "should only happen on a genuinely
     abnormal read" reasoning as step 5's TX-side guard) falls back to
     the elliptic output for that one block rather than corrupt the FFT
     filter's overlap-save history — an RX audio dropout has no
     compensating upside the way TX's silence did, so this falls back to
     the *other* filter's output instead of silence.
   - `src/rx_filter.c` moved from bench-only into the real build
     (`Makefile`'s `SRC`/`OBJ`) — `rx_audio.c` now calls it directly, the
     same "step 5"-equivalent moment `tx_pipeline.c`/`fft_filter.c` had.
   - New rigctld command `u`/`U FFTFILT` (`hamlib.c`), alongside the
     existing `u`/`U NARROW`, lets an operator flip the selector remotely
     without a rebuild — wired into `tools/rigctl_panel.py`'s "RX Filter"
     panel as a second checkbox, "Use FFT filter (experimental)",
     unchecked (elliptic) by default, polled/synced the same way the
     existing narrow-filter checkbox already is.
   - New integration smoke test, `src/rx_audio_test.c`
     (`make test-rx-audio && ./test-rx-audio`) — distinct from step 6's
     `rx_filter_test.c` (which never calls `rx_audio.c` at all, and only
     re-verifies `rx_filter.c`'s own DSP correctness): this one exercises
     `rx_audio_process()`'s own new wiring directly (links `rx_audio.c`/
     `vfo.c`/`fft_filter.c`/`rx_filter.c` standalone, no hardware deps -
     same precedent as the other harnesses). Confirmed: a steady
     on-dial-center tone produces comparable output loudness under the
     elliptic filter, the FFT filter, and bypass (RMS ratio FFT:elliptic
     1.05 — a real check, not just "didn't crash"; an order-of-magnitude
     gap here would have meant the selector was reading the wrong buffer
     or missing a scale factor), and the block-size-mismatch fallback
     produces a comparably-loud, finite reading rather than silence or
     garbage. One bug this caught in the test itself, not the code:  the
     first version of this harness fed its synthetic tone directly at
     `CW_PITCH_HZ`, forgetting that stage 2 *adds* `CW_PITCH_HZ` via
     mixing (baseband → audible pitch) — landing the test tone at
     1400Hz, well outside both filters' passbands, and making the two
     implementations' rejection curves (not their passband gain) look
     like a 12x RMS mismatch. Fixed by feeding the synthetic tone at
     0Hz baseband (a station parked exactly on dial center) instead,
     which is what actually produces a `CW_PITCH_HZ` audio tone at the
     output — the same class of "test harness measured the wrong thing"
     bug step 4's writeup already flagged for a different reason,
     caught here by the same discipline of trusting the number over the
     assumption.
   **What's still open**: the on-air listening comparison itself (does
   the FFT filter's ~267Hz -3dB width / 2.04:1 shape / 16ms group delay
   actually sound at least as good for real CW copy as the elliptic's
   ~300Hz / ~1.9:1 / ~2.6ms, per §9's skirt-quality question) is
   fundamentally the operator's own judgment call on real hardware, not
   something any bench harness can settle — `narrow_filter_coeffs[]`
   stays in the tree, and elliptic stays the default, until that
   judgment is made.

   **On-air comparison, first report (2026-09) — and what the bench said
   about it.** The operator's verdict came back negative for the FFT
   implementation: the elliptic "performs as expected," while the FFT
   filter "lets a lot of wideband hiss and far-off-center signals get
   through." That is a falsifiable claim about selectivity, so it was
   measured rather than argued with, in
   [`dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md`](dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md).
   **The reported symptom was not reproduced.** Against the real
   compiled filter at the shipped 700Hz/300Hz design point, the FFT
   implementation measures *narrower* at -3dB (134Hz vs ~160Hz off
   pitch), 30-55dB deeper across the whole stopband (worst case above
   2kHz: -80.2dB vs the elliptic's flat -49.6dB equiripple floor, which
   never improves past ~5kHz), passes 0.8dB *less* stationary broadband
   noise, and sits **13.6dB quieter between CW elements** under a
   realistic keyed-CW-plus-interferer scenario with the real AGC
   applied. Ruled out by direct measurement: stopband leakage,
   overlap-save history corruption from dropped ALSA periods (a dropped
   block every 50 moves a steady tone's output RMS by 0.0011),
   block-size fallback (`RX_FILTER_BLOCK_LEN` and `PERIOD_FRAMES` are
   both 1024, so the FFT path really does run), and passband gain
   difference.
   What the bench *does* show is a large time-domain difference confined
   to keying transitions: 16.0ms group delay against the elliptic's
   2.6ms, and 8.5ms of **pre**-ringing that a causal IIR cannot have at
   all. Note also that `RX_FILTER_IMPULSE_LEN` (3073) is not a chosen
   number — it is `RX_FILTER_N - RX_FILTER_BLOCK_LEN + 1`, i.e. whatever
   the FFT size and ALSA period leave over — and the note tabulates what
   shortening it would cost (at 1025 taps the -3dB width barely moves,
   134Hz→138Hz, while delay and pre-ringing both fall by two thirds;
   what the extra taps actually buy is ultimate skirt steepness).
   Nothing was changed as a result: this remains a measurement-only
   finding, the "hiss" report is still unexplained (impulsive rather
   than stationary band noise interacting with the pre-ringing is the
   leading untested hypothesis), and elliptic stays the default — which
   §5's transition-region result independently supports for CW.

   **Step 7 real-hardware follow-up (first on-air test).** The step 7
   binary was actually run on real sBitx hardware for the first time,
   and receive audio worked — the elliptic filter's own effectiveness
   was reconfirmed. Two things came back from that test:
   - **Startup got noticeably slower than minibitx's**, with specific
     pauses right after `cw_init()`'s "CW straight key ready" printf and
     right after `sound.c`'s "opened hw:0,0 ... playback" printf
     (`maxibitx.c`'s own init sequence — traced line by line to confirm
     this). Root cause: both pauses are `FFTW_MEASURE` plan-creation
     cost — `rx_filter_new()` (inside `rx_audio_init()`, called right
     after the first printf) at the new `RX_FILTER_N=4096`, and
     `tx_pipeline_new()` (inside `sound_thread_start()`, called right
     after the second) at `TX_PIPELINE_N=2048` — the RX cost is newly
     added by step 6/7 and is now paid in addition to TX's pre-existing
     one, on every process start, with no wisdom-file cache to amortize
     either (a pre-existing limitation — see this section's own earlier
     note on wisdom files). Fixed by adding `fft_filter.c`/`.h`'s
     `filter_new_ex(block_len, impulse_len, fftw_flags)` — an additive,
     non-breaking generalization of `filter_new()` (which now just calls
     it with `FFTW_MEASURE`, so every existing caller — `tx_pipeline.c`,
     both bench harnesses — is unaffected) — and switching
     `rx_filter_new()` to call it with `FFTW_ESTIMATE` instead, the same
     "good enough, chosen immediately, no benchmarking search" trade
     `window_filter()`'s own throwaway per-retune plans already made, now
     applied to `rx_filter.c`'s one persistent, per-block-reused plan
     too. Measured on this project's own dev sandbox (a different, and
     probably substantially faster, machine than the user's Pi Zero
     2W — this number is directional, not a Pi measurement):
     `filter_new_ex(FFTW_MEASURE)` at `RX_FILTER_N=4096` took 240.1ms to
     construct; `filter_new_ex(FFTW_ESTIMATE)` took 0.2ms — essentially
     removing that half of the startup delay. Per-block cost (the thing
     that actually has to fit the 96kHz/1024-sample ~10.667ms real-time
     budget, not just startup) was checked, not assumed: both the
     MEASURE-built and ESTIMATE-built plans measured ~0.016ms per
     `filter_forward()`+`filter_inverse()` call on this same sandbox —
     under 0.2% of budget either way, and identical between the two
     (ESTIMATE only changes *which* startup search is skipped, not the
     transform algorithm's own big-O cost at this size). This is real
     margin, not a razor-thin one, so it's very likely the Pi Zero 2W
     (slower per-operation, but not by 500x) still has comfortable
     headroom too — but that is this fix's one open item: it hasn't
     been re-measured on the user's actual board yet, only reasoned
     about from a healthy safety margin measured elsewhere. All four
     bench harnesses (`test-fft-filter`, `test-tx-pipeline`,
     `test-rx-filter`, `test-rx-audio`) were re-run after this change and
     produced identical readings to before (0.00dB passband, 267Hz -3dB
     width, 1.05 RMS ratio, etc.) — `FFTW_ESTIMATE` changes which
     algorithm variant FFTW picks, not the frequency-domain design math,
     so no DSP behavior changed, only construction-time cost.

     **Confirmed on real hardware**: the pause after "CW straight key
     ready" is gone. **But a new symptom showed up that wasn't present
     before**: a few seconds into a run, `sound.c`'s playback ALSA device
     starts flooding `xrun, recovering` messages continuously — the
     audio thread genuinely isn't keeping up with real time (not a
     device or `SCHED_FIFO` problem — the startup log shows no "failed
     to set audio thread to SCHED_FIFO" warning, so the thread IS
     running at real-time priority; `xrun_note()`'s own flood message,
     unchanged since before this project started, already says as much).
     This risk was explicitly named as this fix's "one open item" above
     (the sandbox-measured 0.016ms/block margin isn't a Pi Zero 2W
     measurement) — it just wasn't expected to actually bite. Two
     hypotheses, not yet distinguished:
     (a) `FFTW_ESTIMATE`'s un-benchmarked algorithm choice happens to run
     meaningfully slower per block than `FFTW_MEASURE`'s on this specific
     ARM core — a real, known FFTW characteristic (ESTIMATE trades away
     the benchmarking search, and how much speed that search would have
     bought varies by machine, sometimes a lot on ARM specifically); or
     (b) `rx_audio.c`'s step 7 design (both stage-3 filters, elliptic AND
     FFT, run every single block regardless of which is selected — "keep
     it warm so switching doesn't thump") was already right at this
     board's real-time ceiling even under `FFTW_MEASURE`'s faster plan,
     and simply hadn't been run long enough before now to notice (the
     user's first report only said RX "sounds good", not that it had run
     for an extended session). Two diagnostic additions, both cheap and
     harmless if unused, to tell these apart on the user's own hardware
     rather than guessing further from this sandbox:
     - `rx_filter.c` now reads `MAXIBITX_RX_FILTER_FFTW_MEASURE` at
       startup (any non-empty value forces `FFTW_MEASURE` back, printing
       which one it picked either way) — if setting it makes the xrun
       flood go away, that's (a), and the real permanent fix is a
       wisdom-file cache (still not implemented — lets `FFTW_MEASURE`'s
       faster per-block plan survive without paying its search cost on
       every restart); if the flood persists even with it set, that's
       (b), and the real fix belongs in `rx_audio.c`'s "always run both"
       design instead, not here.
     - `sound.c`'s audio thread now times `sound_process()` itself
       (`clock_gettime()` around the call, no other overhead) and prints
       an avg/max summary every 5 seconds against the 10.667ms/block
       budget — real numbers from the board actually having the problem,
       instead of this sandbox's own (evidently not representative)
       ones.

     **A third data point, from `top` on the running Pi**: CPU sits
     near 100% (one core) for the first few seconds, then drops to a
     sustained ~27% right around when audio becomes audible — and the
     xrun flood still shows up sometime after that. This reframes both
     hypotheses above: a genuinely *sustained* per-block overload
     (either (a) or (b) as stated) should keep CPU pegged near 100% the
     whole time, not settle down to a comfortable 27% before the xruns
     start. The pattern looks more like a one-time startup transient
     (cold FFTW codelets, cold caches, the Pi's `cpufreq` governor
     ramping up from an idle clock speed to full speed under sustained
     load — any of which would show as a real but temporary 100%-CPU
     catch-up burst, not a permanent budget overrun) followed by a
     *separate*, later, likely intermittent stall — something that
     blocks or delays the real-time audio thread for a burst long
     enough to blow through the 4-period (~42.7ms) playback buffer all
     at once, which is exactly what a "flood" of many xruns in the same
     instant looks like, and is fully consistent with a low *average*
     CPU/block-time reading. This is exactly what the block-timing log
     above is positioned to catch: a `max=` reading that spikes far past
     the `avg=` in whichever 5-second window contains the flood would
     confirm an intermittent stall rather than sustained overload, and
     would point the investigation somewhere else entirely (another
     thread's blocking I/O, a lock, a kernel/USB/thermal event) rather
     than at `rx_filter.c`'s own per-block cost.

     **Both diagnostics came back from the Pi, and they're decisive.**
     `sound_process()` timing: avg 1.4-2.7ms, max 2.2-3.3ms, every 5s
     window, budget 10.667ms — comfortable margin, no visible spike, in
     *both* runs. `MAXIBITX_RX_FILTER_FFTW_MEASURE=1` (forcing the old,
     slower startup back): the xrun flood still happened, at
     indistinguishable timing from the `FFTW_ESTIMATE` run. That rules
     out hypothesis (a) outright — this has nothing to do with which
     FFTW plan `rx_filter.c` picks, so the startup-delay fix itself
     (§10 step 7's earlier follow-up entry) stands as correct and
     unrelated to this issue. It also weakens (b): `sound_process()`
     (which is where "run both stage-3 filters every block" actually
     happens) is measurably fine in both runs, with no spike anywhere
     in the windows shown, including ones containing active flooding.
     The missing time has to be somewhere this measurement wasn't
     looking — `sound.c`'s own `snd_pcm_readi()`/`snd_pcm_writei()`
     calls, or unaccounted gaps between them (`cw_poll_key()`, or a
     scheduling delay before this thread got to run again) — so the
     single `sound_process()`-only timing report was widened into a
     four-part one covering all of it: `read`, `process` (unchanged),
     `write` (the whole `if (pcm_playback)` block, buffer-fill plus the
     actual ALSA write), and `period` (top-of-loop to top-of-loop, which
     a healthy system should hold right at 10.667ms — not more, not
     less). Whichever of these is actually where the time goes will show
     up directly in the next run's log instead of needing another guess.
     Current leading suspicion, given the evidence so far points away
     from anything steps 6/7 added: this may be a pre-existing
     characteristic of `sound.c`'s full-duplex ALSA setup (`hw:0,0`
     opened as two independent capture/playback handles, never
     `snd_pcm_link()`-ed) that simply hadn't been run long enough,
     attentively enough, to notice before this session — not a
     regression from the RX filter work at all.

     **The four-part log came back, and it explains the whole thing —
     the flood is a one-time startup transient, not a chronic
     overload.** Only the very first 5-second window (the one starting
     right at "sound: running") shows anything unusual: `write`
     max=20.693ms and `period` max=31.238ms - one single outlier
     iteration. Every window after that, `read` avg~7.9ms/`process`
     avg~2.7ms/`write` avg~0.02ms/`period` avg~10.664ms, rock solid,
     matching the 10.667ms budget almost exactly, with no further
     spikes and no further "xrun, recovering" lines at all for the rest
     of the run. That 20.693ms write figure is itself a clue, not a
     mystery: `usleep(20000)` is exactly `xrun_note()`'s own deliberate
     "flood detected, back off" breather (`sound.c`'s xrun-flood-tracking
     section, unchanged since before this project started) - so that one
     sample IS the code's own backoff firing once, not an unexplained
     stall. The real question was
     always what caused the initial burst of ~10 back-to-back
     underruns that crossed the flood threshold in the first place, and
     the steady, clean timing on every side of it (before any drift
     could accumulate, after settling instantly and staying settled)
     points at a classic ALSA full-duplex cold-start race: `open_pcm()`
     leaves a device PREPARED but not yet RUNNING, and depending on the
     driver's start threshold, playback's hardware clock can begin
     consuming from the ring buffer before software has had a chance to
     keep it filled - especially here, where the very first real
     `snd_pcm_writei()` call only happens after the audio thread is
     created and has already run a full capture-read/process cycle, and
     `tx_pipeline_new()` (its own `FFTW_MEASURE` search, not yet
     switched to `FFTW_ESTIMATE` - see `rx_filter_new()`'s own comment
     on that trade) adds further delay before that first write can
     happen at all. **Fix**: `sound_thread_start()` now primes
     `pcm_playback`'s ring buffer with a full buffer's worth (4 periods)
     of silence via `snd_pcm_writei()` immediately after opening it,
     before the audio thread (or `tx_pipeline_new()`) ever runs - the
     standard "never let a double-buffered consumer find its buffer
     empty" fix, independent of exactly how much startup delay happens
     before the first real write. This is a genuinely different fix
     from anything in steps 6/7's own code (`rx_filter.c`/`rx_audio.c`
     are untouched here) - it happens to have been *found* by this RX
     work only because step 7 was the first time anyone ran maxibitx's
     RX audio path attentively enough, for long enough, to notice a
     transient that (per the timing log) has nothing to do with RX
     filtering at all. Rebuilt and regression-tested clean (all four
     bench harnesses, zero warnings); not yet re-confirmed on the user's
     Pi that the flood is actually gone, since this sandbox can't
     reproduce ALSA hardware timing at all - that's the one remaining
     open item.

     **Priming alone didn't fix it either - the real cause was the gap
     it was primed into.** Same exact signature on real hardware even
     with the primed buffer in place: burst of ~10 xruns right at
     "sound: running", then rock-solid `read`/`process`/`write`/`period`
     readings for the rest of the run (the one `write` max=20.448ms in
     the first window is - again - `xrun_note()`'s own `usleep(20000)`
     backoff firing once, not a new stall). That ruled the priming fix
     out cleanly, and pointed straight at the one thing this fix
     deliberately left alone: `sound_thread_start()` calls
     `tx_pipeline_new()` - which was still using `FFTW_MEASURE` - *after*
     opening and priming `pcm_playback`, but *before* creating the audio
     thread that actually keeps it fed. That ordering means the primed
     buffer sits completely unattended for the entire length of
     `tx_pipeline_new()`'s own `FFTW_MEASURE` search - on this sandbox,
     186.7ms for `TX_PIPELINE_N=2048` (measured the same way as
     `rx_filter_new()`'s own construction-time check) - which is nearly
     4.4x longer than the primed buffer's own ~42.7ms of silence. The
     buffer drains and starts underrunning well before the audio thread
     that's supposed to refill it ever gets to run, reproducing the exact
     same startup burst regardless of how much was primed into it. This
     also explains why forcing `rx_filter_new()` back to `FFTW_MEASURE`
     earlier made no difference: RX's own plan search happens in
     `rx_audio_init()`, well before `sound_thread_start()` is even
     called, so it was never part of this particular gap at all - a
     different, real fix (removing RX's own share of the startup delay)
     for a different problem, not a red herring, just not this one.

     **Fix, two parts:**
     - `tx_pipeline_new()` now makes the exact same trade
       `rx_filter_new()` made: `filter_new_ex(..., FFTW_ESTIMATE)`
       instead of `filter_new()`'s `FFTW_MEASURE`, overridable via
       `MAXIBITX_TX_PIPELINE_FFTW_MEASURE` (same diagnostic pattern as
       RX's own env var). Measured the same way as RX's: construction
       time 186.7ms → 0.0ms on this sandbox, per-block cost identical
       between the two (~0.007ms, against `TX_PIPELINE_N=2048`'s share of
       the same 10.667ms/block budget) - not yet re-measured on the
       user's actual Pi, same open item as RX's own version of this
       trade.
     - `sound_thread_start()` now calls `tx_pipeline_new()` *before*
       priming `pcm_playback`, and does the priming immediately before
       `pthread_create()` instead of immediately after opening the
       device - closing the gap between "buffer gets filled" and "the
       thread that keeps it filled starts running" down to essentially
       nothing, rather than trying to out-buffer whatever that gap's
       length happens to be. This is the more robust half of the fix:
       even if some future step adds another real setup cost between
       opening playback and starting the audio thread, priming
       immediately before `pthread_create()` keeps this race closed
       regardless, where the first attempt's placement (prime, then pay
       an unrelated setup cost, then start the thread) could reopen it
       again.
     All four bench harnesses re-run clean after both changes
     (`test-tx-pipeline`'s own numbers - 0.00dB passband, -70.05dB image
     rejection - unchanged, confirming `FFTW_ESTIMATE` didn't touch TX's
     frequency-domain math any more than it touched RX's).

     **Confirmed fixed on the user's real hardware.** No more xruns at
     all - the `loop timing` report for the whole run holds steady at
     read~7.56ms/process~3.08ms/write~0.017ms/period~10.663ms, every 5s
     window, no anomalies anywhere. This closes out the entire xrun-flood
     investigation: `rx_audio.c`/`rx_filter.c` were never the cause (both
     A/B tests ruled that out cleanly); the real bug was a start-up
     sequencing race in `sound.c`/`tx_pipeline.c` that step 7's RX work
     just happened to be the first thing to run long enough, attentively
     enough, to expose. With the flood gone, the periodic `loop timing`
     printout was switched from always-on to opt-in
     (`MAXIBITX_LOOP_TIMING=1`) - it did its diagnostic job, and a normal
     run has no reason to print a status block every 5 seconds forever;
     the underlying tracking (a few `clock_gettime()` calls per block)
     stays unconditional and effectively free, so the knob is there again
     if a future mystery needs it. The two `MAXIBITX_*_FFTW_MEASURE`
     env-var overrides (`rx_filter.c`/`tx_pipeline.c`) stay as
     permanent, harmless diagnostic knobs for the same reason.
   - **"Use FFT filter" produced no noticeable audible effect.** Not a
     code bug as far as this can be verified without the user's own
     console log: `rx_audio_test.c`'s Case B independently confirms the
     selector really does switch which buffer feeds stage 4 (a real,
     measured 1.05 RMS ratio between the two, not a "didn't crash"
     check), and both filters really do have different, live-measured
     shapes (step 6: FFT ~267Hz -3dB width, 2.04:1 shape factor, 16ms
     group delay; elliptic: ~300Hz, ~1.9:1, ~2.6ms). The likely
     explanation is that this is a genuinely subtle DSP difference, not
     a missing one — both are already narrow ~300Hz CW filters with a
     similar shape factor, so switching *between* them doesn't produce
     the dramatic, easy-to-hear change that switching the narrow filter
     *off* (bypass) does; the user's own report ("the narrow cw filter
     is still very effective") is consistent with this — the narrowing
     itself clearly works, just not distinguishably differently between
     the two implementations, at their current default widths, to this
     operator's ear. Ruling in/out a real wiring bug instead (the FFTFILT
     toggle silently not reaching `rx_audio.c`, or the block-size-
     mismatch fallback silently always engaging) needs information only
     available from the user's own running console log: whether toggling
     the checkbox prints `hamlib.c`'s `"rigctl: U FFTFILT %d -> ..."`
     confirmation line, and whether `rx_audio.c`'s own
     `"rx_audio: block size ... falling back to the elliptic filter"`
     warning ever appears (it should not, in normal operation — the audio
     thread always calls in fixed `RX_FILTER_BLOCK_LEN`-sized blocks).

     **Confirmed from the user's real console log, both toggling
     correctly.** Every FFTFILT toggle shows both `hamlib.c`'s
     `"rigctl: U FFTFILT %d -> stage-3 implementation %s"` confirmation
     and the follow-up `"u FFTFILT -> fft"`/`"-> elliptic"` readback -
     the selector reaches `rx_audio.c` every time. The block-size-
     mismatch warning never appears once in the whole log - rules that
     hypothesis out too. So this is not a wiring bug.

     **A second, independent measurement in the same log settles the
     "is it doing anything at all" question**: rigctld's `l STRENGTH`
     (the S-meter reading) is driven by `rx_audio.c`'s `meter_env`, which
     is fed by `fabs(narrowed)` - the *exact same* `narrowed` value that
     `sample = narrowed * gain * rx_volume` also uses for the actual
     audio output (`rx_audio.c`'s own comment on "two envelopes, two
     jobs" - the meter's only job is to observe what's already reaching
     the speaker, nothing more). With `NARROW` on, `STRENGTH` reads
     -45 to -48dB regardless of which stage-3 implementation is
     selected - both far below the ~-24 to -28dB the same log shows with
     `NARROW` off. That means the FFT filter genuinely IS reducing total
     signal+noise energy by an amount statistically indistinguishable
     from the elliptic filter's - not a gain bug, not "doing nothing":
     the very code path that also drives the speaker confirms real,
     comparable attenuation either way.

     So the real question becomes why comparable *measured* attenuation
     produces such different *perceived* results - "very apparent" for
     elliptic, "difficult to hear, if anything" for FFT. The most likely
     explanation, grounded in what's already measured rather than a new
     guess: the elliptic filter is a resonant 8-pole IIR with a real
     0.5dB in-band ripple (its own design point, `rx_audio.c`'s header),
     while the FFT filter is deliberately flat and ripple-free by
     construction (bench-confirmed exactly 0.00dB passband, no ripple,
     step 6). A resonant ripple/peak sitting right at the CW pitch can
     make the wanted tone perceptually "pop" out against the reduced
     noise floor - a real psychoacoustic emphasis effect - independent
     of how much total noise energy is actually removed. `meter_env`'s
     slow envelope follower (the same attack/release time constants as
     the AGC's own, tuned for CW keying, not for capturing fine spectral
     texture) also can't distinguish "smoothly reduced hiss" from "hiss
     with a similar average level but different in-band brightness" -
     human hearing is far more sensitive to that kind of texture than an
     RMS-style meter is. Both filters narrowing similarly *in total
     energy* is fully consistent with them sounding very differently
     "narrow" to a human ear. This is a genuine, on-air listening
     finding - not a bug to fix - and it directly answers this build
     order step's original open question (§10 step 6/7: "does the FFT
     filter sound at least as good for real CW copy"): on this operator's
     ear, on this band, elliptic's resonant character reads as more
     effective even though the FFT filter is doing comparable
     quantitative work, so elliptic staying the default is the right
     call unless/until that changes. `narrow_filter_coeffs[]` and the
     `RX_NARROW_FILTER_FFT` selector both stay in the tree either way -
     the FFT path remains available (and is verified genuinely
     effective) for anyone who prefers its flatter, ripple-free
     character, or for a future step that might narrow its width further
     to make the effect more assertive.
   - **Follow-up: the FFT filter's actual measured shape, checked
     directly rather than assumed.** The user's own description of the
     FFT filter ("sounds like very wideband noise... little effect on
     the hiss") was specific enough to warrant checking the psychoacoustic
     hypothesis above against the filters' real frequency responses,
     rather than resting on it. A small standalone tool (not part of the
     maxibitx tree) read `rx_filter.c`'s live `fir_coeff` array directly
     - the exact per-bin gain `filter_forward()` multiplies every block
     by, at the shipped default 700Hz/300Hz tuning - and evaluated
     `narrow_filter_coeffs[]`'s 4-section biquad cascade analytically at
     the same frequencies, both normalized to 0dB at their own measured
     passband peak. Result: the FFT filter is **not** the wider or
     weaker of the two. Its -3dB width is 265Hz vs. elliptic's 325Hz, its
     -40dB width 495Hz vs. 575Hz, and its stopband keeps falling smoothly
     the further out you go, reaching -85dB by 4kHz. The elliptic
     filter's stopband, by contrast, is a genuine ripple: two narrow,
     very deep nulls (its design's transmission zeros, near 260Hz and
     1790Hz) but sitting back up around only -50 to -60dB almost
     everywhere else - exactly its spec'd "50dB stopband," never much
     better, even 3kHz away from the passband. So the FFT filter isn't
     failing to narrow the band or leaking more noise through by design
     - if anything the measured data says the opposite. This *strengthens*
     the resonance-vs-flat-response explanation above rather than
     replacing it: with the FFT filter measurably at least as selective,
     the most likely account for "little effect on the hiss" is still
     that the elliptic filter's in-band ripple gives the CW tone a
     resonant lift right where the ear is listening, making its noise
     reduction sound obvious, while the FFT filter's flatter cut removes
     comparable (here, more) energy with no such emphasis to draw the
     ear's attention. Elliptic stays the default on the same basis as
     before.
   - **RX dial accuracy, confirmed on air:** the user reports receiving
     W1AW (ARRL HQ's own station, a well-known reference signal hams use
     for exactly this kind of check) at 7.0475 MHz and finding it exactly
     on frequency. That's a real confirmation that the RX chain's
     frequency translation — `vfo.c`'s baseband math plus stage 2's
     mixing to `CW_PITCH_HZ`, all upstream of both stage-3 filters this
     step touched — hasn't introduced any offset error carrying over from
     minibitx. Distinct from step 5's still-open TX-side dial-accuracy
     item above (that one needs someone *else* to confirm where maxibitx
     itself transmits, not what it receives), but a meaningful data point
     in its own right, and good independent evidence that nothing in
     steps 6/7's RX changes disturbed tuning.
8. **Done, confirmed on air.** Wired real
   mic-audio TX for USB/LSB into `cw.c`/`sound.c`, reusing the exact
   same shared `tx_pipeline.c` instance CW already uses (step 5) rather
   than a second one — `tx_pipeline_process_block()`'s `sideband`
   argument only affects the bin-zero step, and `filter_tune()`'s
   300-3000Hz passband has no sideband-dependent term, so CW/USB/LSB
   genuinely share one instance, exactly as step 4's writeup anticipated
   (`tx_pipeline_retune()` still goes unused — LSB doesn't need a
   mirrored passband after all, since the filter runs on real baseband
   audio *before* the bin-zero step, not on an already-shifted signal).
   - This also folds in what used to be a separate, later step
     ("physical PTT input, once CAT-only testing is done") much
     earlier than originally sequenced here — and for a different
     reason than planned: it turns out no new GPIO is needed at all.
     Cross-checking real sbitx's own `sbitx_gtk.c` (`afarhan/sbitx`,
     fetched directly to verify hardware wiring the operator asked
     about, rather than trusting an initial, garbled recollection of
     pin numbers that didn't survive a self-consistency check) found
     `PTT` defined as wiringPi pin 7 — which, translated through the
     same wiringPi→BCM table `docs/01_hardware_init_and_control.md`'s
     own migration table already uses, is BCM4/physical pin 7: the
     exact same electrical line as maxibitx's existing `CW_KEY`. Real
     sbitx reads this identical line as a straight key in CW mode and
     as a mic PTT switch in voice modes (`sbitx_gtk.c`'s own
     `key_poll()`/main-loop `tx_on(TX_PTT)`/`tx_off()` split) —
     confirming the operator's mic PTT switch and CW straight key
     genuinely share one physical contact on this hardware, not two
     separate ones ("Farhan sometimes demonstrates operating CW with
     his thumb on the mic PTT switch"). So sensing mic PTT needed no
     new hardware pin, just `cw.c`'s `cw_poll_key()` interpreting the
     same closure differently depending on `radio_get_mode()`
     (radio.h) — see that function's own comment for this cross-check.
   - `cw_poll_key()` (`cw.c`): in `RADIO_MODE_CW`, unchanged (hang-timer
     semi break-in, as always). In `RADIO_MODE_USB`/`RADIO_MODE_LSB`,
     the same key line is read as an immediate PTT switch instead —
     `radio_set_tx()` asserted the instant it closes, released the
     instant it opens, no hang timer (a voice transmission has no
     inter-element gap to bridge the way CW's dits/dahs do).
     `RADIO_MODE_DIGITAL` still ignores this line entirely — correctly:
     that mode has no physical key/mic PTT to sense in the first place,
     since WSJT-X keys the rig over CAT instead (see below and step 9).
   - `sound.c`'s TX audio-generation block now branches on
     `radio_get_mode()`: CW keeps pulling `cw_get_sample()`'s tone and
     `TX_PIPELINE_KEEP_UPPER`, unchanged; USB/LSB instead pull real mic
     audio (`mic_buf` — already captured every block since minibitx,
     but previously discarded, `(void)input_mic;`) scaled by a new
     `MIC_TX_INPUT_SCALE` constant, and select
     `TX_PIPELINE_KEEP_UPPER`/`TX_PIPELINE_KEEP_LOWER` by mode. The
     local monitor channel (L, never reaches the PA) now carries
     whichever signal is actually driving TX that block — the CW
     sidetone pitch, or a monitor copy of the operator's own mic audio
     — instead of always being the CW tone.
   - The WM8731's 'Mic' capture gain (`setup_audio_codec()`),
     previously left at 0 (muted) since nothing consumed it, is now set
     to a new `MIC_CAPTURE_GAIN_PERCENT` (50) — a plain starting guess,
     not bench-confirmed, the same way `RX_CAPTURE_GAIN_PERCENT`
     started before real listening refined it. Unlike
     `RX_CAPTURE_GAIN_PERCENT`, 'Mic' hasn't even been bench-confirmed
     yet to be a real graduated gain control rather than a switch — the
     same gotcha `RX_LINE_INPUT_ON`'s comment already documents for a
     different control on this same codec — worth an
     `amixer -c 0 sget 'Mic'` check before trusting this number.
   **What's genuinely still open, not yet bench- or air-verified:**
   `MIC_TX_INPUT_SCALE`'s and `MIC_CAPTURE_GAIN_PERCENT`'s real-world
   levels (speech has a very different amplitude/clipping story than a
   steady CW tone or envelope — no wattmeter check has been done
   against a real keyed voice envelope, unlike CW's own step 5/§8
   re-check); actual on-air sideband rejection for a real voice signal
   (step 4/5's -70dB number is a CW-tone measurement only); and whether
   `MIC_CAPTURE_GAIN_PERCENT`'s ALSA control even behaves as a
   graduated gain. A first on-air SSB test is what would answer all of
   these — nothing here should be assumed correct until one happens.
   - **First on-air SSB test: no measurable power out, real bug found
     and fixed.** Keying the mic in USB produced no wattmeter reading
     and nothing on a remote receiver - a real, complete TX failure, not
     a level/calibration shortfall. (What DID appear - a voice-shaped
     trace on `tools/rigctl_panel.py`'s spectrum display while talking -
     turned out to be a red herring, not evidence anything downstream of
     the mic was working: that display is fed by `iq_stream.c`'s RX I/Q
     path, `rx_buf`/L, not `mic_buf`/R or anything from `tx_pipeline.c`
     - a completely separate signal, already flagged as not a valid
     pre-PA tap when this was first looked at as a TX-monitoring tool
     - see this same section's step-5-era TX-spectrum-tool discussion.)
     Root cause: `sound_set_rx_capture()` (`sound.c`), called by
     `radio_tx_apply()` at the very start of every TX burst to protect
     the RX chain from relay/PA-harmonic bleed
     (`rx_gain_and_level_calibration.md` §8), zeroed the WM8731's whole
     `'Capture'` ALSA element via `sound_mixer()`'s `*_all()` call -
     which sets BOTH stereo channels identically. That was harmless
     right up until this step, when `'Capture'` stopped being an
     RX-only control: `'Input Mux'` is bench-confirmed locked to
     `'Line'` (not `'Mic'`), which strongly suggests this board's real
     microphone is wired onto the codec's Line-In-RIGHT pin rather than
     through the codec's own separate internal mic preamp/mux path -
     meaning `mic_buf`/R rides the exact same `'Capture'` gain stage as
     `rx_buf`/L, not the separate `'Mic'` element `MIC_CAPTURE_GAIN_
     PERCENT` was added to control (step 8's earlier writeup above
     already flagged `'Mic'` as possibly inert for exactly this reason -
     this on-air result is consistent with that). So every TX burst was
     zeroing the mic's real analog level the instant it started, before
     `tx_pipeline_process_block()` ever saw anything but near-silence -
     a complete, silent kill of the entire USB/LSB signal chain,
     regardless of `MIC_TX_INPUT_SCALE`/`MIC_CAPTURE_GAIN_PERCENT`.
     Fixed with a new per-channel capture-volume helper
     (`sound_mixer_capture_channel()`, mirroring the existing
     per-channel `sound_mixer_channel()` pattern "Master" already needed
     for the same L-vs-R-serve-different-purposes reason) -
     `sound_set_rx_capture()` now touches only the LEFT (RX) channel of
     `'Capture'`, leaving RIGHT (mic) alone through every TX/RX
     transition in every mode. Depends on this ALSA element actually
     supporting independent per-channel capture volume rather than a
     single shared/ganged register underneath the stereo abstraction -
     unconfirmed until tested; `sound_mixer_capture_channel()` logs a
     clear warning if a per-channel write is ever rejected, which would
     mean a different fix (e.g. skipping the mute entirely in USB/LSB
     rather than trying to split it by channel) is needed instead. Also
     added a Mode selector to `tools/rigctl_panel.py` (CW/USB/LSB/
     DIGITAL radio buttons, reading/writing rigctld's `m`/`M`) - direct
     motivation was exactly this test: with no in-panel way to see or
     change mode, a mode/PTT-line mismatch was one more silent failure
     mode indistinguishable from this real bug until ruled out by hand.
   - **Re-test after that fix: mic audio now audible on the local
     monitor speaker, but still no measurable power out.** A genuinely
     different symptom from before - this confirms the capture-mute fix
     above worked (`mic_buf` is carrying real signal again, and
     `sound.c`'s mode branching is correctly routing it to the local
     monitor channel), and narrows the remaining gap to somewhere
     between real mic level and what actually reaches the exciter.
     `MIC_TX_INPUT_SCALE` (`sound.c`) was always flagged as a guess
     ("maps full-scale straight to 1.0... NOT yet checked against real
     speech") - a real mic, even at `RX_CAPTURE_GAIN_PERCENT`'s existing
     70% analog gain, plausibly peaks at a small fraction of int32
     full-scale for ordinary speaking volume, well below `cw_get_sample()`'s
     near-unity CW tone - easily a >10x amplitude gap, i.e. >100x in
     power, which a modest wattmeter could plausibly read as "nothing"
     while a local speaker amplifier makes the exact same signal
     perfectly audible (an amplifier and a wattmeter have very different
     sensitivity floors). Rather than re-guess a single compile-time
     constant and require an edit/rebuild/restart per trial, split
     `MIC_TX_INPUT_SCALE` (a fixed, purely mechanical int32->float unit
     conversion) from a new `mic_tx_gain` (`sound.c`/`sound.h`) - a live,
     runtime-adjustable multiplier on top of it, starting at 1.0, the
     same "separate the unit conversion from the operator-adjustable
     gain" split `rx_audio.c`'s `rx_volume`/AGC already use. Reachable
     remotely via a new rigctld extension, `l`/`L MICGAIN` (`hamlib.c`,
     raw multiplier value, not a 0.0-1.0 percent like `AF` - there's no
     natural ceiling here the way there is for volume) and a new "Mic
     Gain (TX, USB/LSB)" slider in `tools/rigctl_panel.py`, right under
     Volume - so the next several bisection trials against a real
     wattmeter reading can happen live, mid-session, the way `AF`
     volume already can, rather than needing `MIC_TX_INPUT_SCALE`
     re-guessed and the whole binary rebuilt/restarted each time. Not
     yet re-tested on air - the next step is simply keying up in
     USB/LSB and raising this slider while watching the wattmeter.
   - **Third on-air data point: LSB measured a hard 0W while USB (same
     mic, same slider settings) put out full rated power.** This ruled
     out the gain-staging hypothesis above as the cause - a level
     problem would affect both sidebands the same way, not one of them
     completely - and pointed instead at something sideband-specific in
     `tx_pipeline.c`'s FFT construction. Investigating found not one but
     two real, previously-latent bugs, both specific to
     `TX_PIPELINE_KEEP_LOWER` (LSB), which no existing bench test had
     ever actually exercised - `tx_pipeline_test.c`'s Cases A-C only
     ever ran `TX_PIPELINE_KEEP_UPPER`; Case B's own symmetric test
     filter (a bespoke `filter_tune(f, -3000, 3000, ...)`, not
     `tx_pipeline_new()`'s real passband) happened to sidestep the first
     bug entirely, so LSB's code path had shipped from step 4 onward
     numerically unverified.
     1. **The filter itself was silently discarding everything LSB
        needed, before `zero_sideband()` ever ran.** `tx_pipeline_new()`
        tuned its shared filter with plain `filter_tune()` - and
        `fft_filter.h`'s own header comment on that function is explicit
        that its passband is one-sided: "the *other* half of the
        spectrum is deliberately unwanted image content." That's exactly
        right for a caller that only ever wants
        `TX_PIPELINE_KEEP_UPPER`, but this pipeline is shared - a real,
        symmetric input tone has energy at both +700Hz and -700Hz, and
        `zero_sideband(TX_PIPELINE_KEEP_LOWER)` is supposed to keep the
        -700Hz half and let the explicit rotate place it. With plain
        `filter_tune()`'s one-sided +300..+3000Hz-only passband, the
        -300..-3000Hz content LSB needs was already zero by the time
        `filter_forward()` finished - `zero_sideband()` had nothing left
        to keep. Measured directly (a throwaway RMS check comparing
        `TX_PIPELINE_KEEP_UPPER`'s and `TX_PIPELINE_KEEP_LOWER`'s raw
        output energy for the identical input tone): 0.707 RMS for
        upper, 0.0000024 RMS for lower - not a mis-placed signal, no
        signal at all, which matches "hard 0W" far better than any
        gain-staging shortfall could. Fixed by switching
        `tx_pipeline_new()`/`tx_pipeline_retune()` to
        `filter_tune_real()` - the same symmetric, mirrored-passband
        entry point `rx_filter.c` already uses for its own "genuinely
        real signal" reason (`fft_filter.h`'s own comment on the two
        functions) - so the filter now preserves *both* sidebands'
        content and leaves the actual sideband selection entirely to
        the explicit `zero_sideband()` step, the way
        `docs/ARCHITECTURE.md` §5's original design always intended.
     2. **The shared IF bin-rotate (`TX_IF_SHIFT_BINS`) was derived only
        for the kept-upper case, then reused unmirrored for LSB.**
        `TX_IF_SHIFT_HZ`'s derivation (`tx_pipeline.h`) rotates the kept
        POSITIVE-frequency half up onto `xtal_filter_center`;
        `TX_PIPELINE_KEEP_LOWER` keeps the NEGATIVE-frequency half
        instead, and applying that same rotation to it lands the result
        at a different, uncentered frequency - not necessarily hard
        0W by itself (more likely a de-tuned, attenuated signal off the
        crystal filter's passband center), but wrong regardless. Fixed
        with a mirrored `TX_IF_SHIFT_HZ_LSB`/`TX_IF_SHIFT_BINS_LSB`
        (`bfo_freq - xtal_filter_center + CW_PITCH_HZ` - the sign on
        `CW_PITCH_HZ` flips relative to the upper-case formula, giving
        23300Hz -> 497 bins), and `tx_pipeline_process_block()` now
        selects between `TX_IF_SHIFT_BINS`/`_LSB` (and the matching
        phase-continuity `flip` parity, which depends on which shift
        was actually applied) based on the block's own `sideband`
        argument instead of hardcoding the upper-case constant
        everywhere.
     Both fixes were needed together - fixing only the rotate constant
     without also fixing the filter's passband still measured -146dB
     (i.e. still nothing, since `zero_sideband()` had nothing to
     rotate); fixing only the filter without the mirrored rotate would
     have produced a real but off-center, de-tuned signal instead.
     Verified with a new `tx_pipeline_test.c` Case D, added specifically
     because Cases A-C never touched `TX_PIPELINE_KEEP_LOWER`: with both
     fixes in place it reads 0.00dB at LSB's own predicted placement
     (22596.875Hz) and -78.22dB measured at the frequency the old,
     unmirrored-shift bug would have actually produced (21190.625Hz,
     `target_image` from Case B's own math) - confirming this isn't just
     *a* signal somewhere, but the right one, and specifically not the
     old wrong one. Not yet re-tested on air - this is a bench-only
     proof; the next step is re-trying LSB on 40m against the wattmeter
     to confirm it now matches USB's already-working power output.
   - **On-air re-test confirms the fix: LSB and USB now put out
     comparable power at the same `mic_tx_gain` setting.** This closes
     out the bench-only caveat above - the `filter_tune_real()` +
     `TX_IF_SHIFT_BINS_LSB` fix is now on-air verified, not just
     bench-proven, and step 8's mode-branching TX path is confirmed
     working symmetrically for both sidebands, not just USB. Also gives
     the first real data point for `mic_tx_gain` itself: the correct
     operating point was found at the low end of its 0-64 range, under
     ~5 - i.e. much closer to `sound.c`'s existing default of 1.0 than
     to its max, consistent with the "a real mic peaks far below
     `cw_get_sample()`'s near-unity CW tone" reasoning from the
     gain-staging writeup above. Genuinely still open: an exact,
     bisected optimal value (rather than just "somewhere under ~5"),
     and whether 1.0 is close enough to leave as the compiled default or
     worth nudging once a tighter number is in hand - neither blocks
     step 8 being done, both are just further calibration precision, the
     same category as the still-open ALC/power-calibration item (§9)
     below.
   - **Sideband sense (not just power) confirmed correct on air, closing
     out the one remaining doubt about the LSB fix above.** Every check
     so far (Case D's bench numbers, the on-air power match) confirmed
     LSB and USB are self-consistent with each other - correctly
     mirrored, not colliding - but none of them independently proved
     which physical sideband either one actually lands on once the
     signal passes through `bfo_freq`/`xtal_filter_center`'s real mixer
     chain, since `radio.c` has no mode-dependent branching in that
     chain at all (identical `clk1`/`clk2` formulas regardless of mode)
     and `maxibitx`'s own RX path doesn't yet discriminate sidebands
     either - there was no built-in ground truth to check against, only
     inference from step 5's CW/USB image-rejection test (which used the
     same `TX_PIPELINE_KEEP_UPPER` convention USB shares, and confirmed
     *that* construction lands above the dial, not inverted). Resolved
     with a receiver-label-independent test: a steady whistled tone,
     watched on a remote receiver's waterfall rather than listened to
     through its own mode demodulator, so the result depends only on
     which side of the dial frequency the energy actually falls on.
     Result: LSB's tone appeared *below* the dial frequency, exactly the
     true-lower-sideband behavior `TX_IF_SHIFT_BINS_LSB`'s derivation
     predicted, pairing with USB's already-established above-dial
     placement. Both sidebands are now independently on-air confirmed
     landing on their correct physical side of the dial - not just
     correct relative to each other, which is the distinction this check
     specifically existed to rule out. No code change needed; this was a
     real, live risk (double-conversion superhet designs are a classic
     place for exactly this kind of inversion to hide) that turned out
     not to be a bug.
9. **Done, confirmed on air (2026-09-23 FT8 contact).** WSJT-X TX
   audio bridge: replaced `usb_gadget.c`'s UAC2 gadget end to end, from
   raw baseband I/Q (for an external SDR-console app to demodulate
   itself) to genuinely bidirectional, already-demodulated 16-bit/48kHz
   real PCM audio - closing the actual gap behind step 4/8's `DIGITAL`
   placeholder, and superseding §5's original "reaches input_mic over
   the mic/line-in path" plan for that mode entirely. Prompted by the
   operator asking, in plain terms, how WSJT-X was ever supposed to get
   its own generated tone back to the exciter - tracing the existing
   I/Q gadget's actual data (`sound.c`'s old `uac_push_iq(out_i, out_q)`
   call, fed straight from the VFO-mixed `i_samples`/`q_samples`, never
   touched by `rx_audio.c`'s real demodulator) confirmed it never
   carried anything WSJT-X could decode without a separate SDR-console
   app doing real demodulation in between - not the architecture the
   operator actually wanted.
   - **Why real audio, not I/Q, and why a dedicated USB path rather than
     the mic input:** WSJT-X (and any similar digital-mode app) wants an
     ordinary sound-card-style device to decode from, not raw I/Q it has
     no built-in way to demodulate; real demodulation already happens
     on-target (`rx_audio.c`), so the gadget now carries that same
     signal directly instead of asking a host-side app to redo the job.
     Genuinely bidirectional now, unlike the I/Q version (whose
     playback/host-to-device direction was declared in the USB
     descriptor but never used): WSJT-X's own generated TX tone arrives
     over this same gadget's capture-side PCM and rides into the shared
     `tx_pipeline.c` instance step 8 already built for CW/USB/LSB.
   - **RX tap point - post-AGC, pre-`rx_volume`:** `rx_audio_process()`
     (`rx_audio.c`/`.h`) gained a new `uac_out` output parameter, tapped
     one stage earlier than `out[]`'s own codec-bound samples - after
     stage 4's AGC makeup gain (so a remote decoder still gets signal
     conditioning that keeps its input in a decodable range regardless
     of band conditions, the same thing stage 1/3's filtering already
     does for it) but before `out[]`'s own `* rx_volume` multiply (so
     WSJT-X's decoded level doesn't silently change every time the
     operator touches their own listening-volume knob) - the same
     "line out"/ACC-jack independence a real rig's front-panel volume
     already has from its own fixed-level rear connector. Deliberately
     NOT independent of the AGC too - AGC is signal-conditioning, not an
     operator loudness preference, so a remote decoder wants it exactly
     as much as the local speaker does.
   - **New module, `upsample48k.c`/`.h`:** the direction `decim48k.c`
     never needed - 48kHz (the gadget's TX/capture-side rate) back up to
     96kHz (`tx_pipeline.c`'s native rate). Deliberately reuses
     `decim48k.c`'s own 25-tap coefficient table rather than deriving a
     new filter from scratch: a real anti-aliasing filter for decimation
     and a real anti-imaging filter for interpolation are the same
     prototype lowpass when the rate-change factor matches (here, M=2/
     L=2), differing only in the zero-stuffing insertion point and a
     ×2 makeup-gain correction the interpolation direction needs (zero-
     stuffing halves a signal's average energy). Bench-verified against
     synthetic tones (`src/upsample48k_test.c`,
     `make test-upsample48k && ./test-upsample48k`, same "not part of
     the shipped binary" convention as every other DSP module's own
     harness in this doc) - real numbers: a steady input settles to
     within ~2% of unity passband gain (0.978-0.982 measured, vs. 1.0
     ideal), a 3000Hz tone reappears at 1.0019 (essentially unity)
     amplitude once upsampled, and the zero-stuffing image at 45000Hz
     (48000 - 3000) measures **55.7dB down** - well past the harness's
     own 20dB bar and consistent with the same coefficient table's
     already-bench-proven decimation-direction rejection.
   - **Compile-time scale, as asked for:** `usb_gadget.c`'s new
     `UAC_RX_AUDIO_SCALE` (`32767.0 / 500000000.0`, referencing
     `rx_audio.c`'s own `AGC_TARGET_AMPLITUDE`) is the one knob that
     maps the DSP's natural post-AGC amplitude range onto 16-bit PCM's
     ±32767 - retune that, not any caller, if the level WSJT-X actually
     sees on a real host ever needs adjusting; nothing downstream of
     `uac_writer_thread()` needs to change. The reverse (host-to-device)
     direction needed no equivalent knob - unpacking a 16-bit PCM sample
     back to a normalized double is a fixed, unambiguous conversion, not
     an amplitude judgment call the way mapping onto the gadget's own
     outbound range was.
   - **Threading/queueing, both directions:** `usb_gadget.c` now runs two
     independent lock-free SPSC ring buffers (the same producer-writes-
     head/consumer-writes-tail split `hpsdr_p1.c`'s I/Q queue already
     established, doubled rather than restructured) instead of one
     paired I/Q queue - `uac_writer_thread()` (RX/outbound, unchanged in
     spirit from the old writer, just packing mono 16-bit PCM instead of
     paired 24-bit I/Q) and a new `uac_reader_thread()` (TX/inbound,
     mirroring the writer's backoff/retry/transition-logging pattern in
     reverse, the sole caller of the blocking `snd_pcm_readi()` on the
     gadget's capture-side PCM substream). Both keep `sound.c`'s
     real-time audio thread fully isolated from a stalled or absent USB
     host in either direction, for the same reason step-1-era
     `hpsdr_p1.c` needed this split in the first place.
   - **`sound.c` wiring:** `sound_process()`'s old per-block
     `decim48k_apply()`-on-raw-I/Q loop (feeding `uac_push_iq()`) is
     gone entirely, replaced by decimating `rx_audio_process()`'s new
     `uac_out` tap and feeding `uac_push_audio_rx()` instead - one rail,
     not two, since real audio has no I/Q pairing to preserve.
     `audio_loop()`'s TX branch gained a third `tx_mode` case alongside
     CW's tone and USB/LSB's mic audio: `RADIO_MODE_DIGITAL` pulls from
     `uac_pull_audio_tx()`, upsamples via `upsample48k_apply()`, and
     always selects `TX_PIPELINE_KEEP_UPPER` (FT8/digital-mode
     convention: always transmitted as USB regardless of band). The
     branch's own outer gate also had to change - `cw_tx_active()` alone
     (cw.c's key/mic-PTT-driven flag) is never true in `DIGITAL`, since
     that mode's PTT is CAT-only and was already working unconditionally
     before this step (`hamlib.c`'s `T`, `usb_gadget.c`'s own `TX`/`RX`/
     `TQ` handlers all call `radio_set_tx()` regardless of mode) - so the
     gate is now `cw_tx_active() || (in_tx && radio_get_mode() ==
     RADIO_MODE_DIGITAL)`, and any shortfall in what the host actually
     sent is silence-filled by feeding zero through the (still-running,
     history-preserving) upsampler rather than skipping it, the same
     "keep it warm across a gap" reasoning `rx_audio.c`'s stage-3 filters
     already use while bypassed.
   - **Regression check:** full rebuild (`make clean && make`) clean
     under `-Wall -Wextra`; `test-fft-filter`, `test-tx-pipeline`,
     `test-rx-filter`, and `test-rx-audio` (the latter's call site
     updated for `rx_audio_process()`'s new `uac_out` parameter) all
     still pass with numbers matching their own steps' original bench
     results - this step touched `rx_audio.c`'s signature but not its
     DSP, and touched `sound.c`'s RX-side gadget feed but not
     `hpsdr_p1.c`/`iq_stream.c`'s own I/Q consumption, which is
     unchanged and still gets native 96kHz I/Q exactly as before.
   - **What's genuinely still open, not yet bench- or air-verified:**
     nothing has actually been plugged into a real WSJT-X session yet -
     `UAC_RX_AUDIO_SCALE`'s real-world level (does WSJT-X's own decoder
     actually see a comfortably-decodable signal, or does it need
     retuning the same way `MIC_TX_INPUT_SCALE`/`mic_tx_gain` did for
     step 8's voice path), whether WSJT-X's own generated tone survives
     the round trip through `uac_reader_thread()`/`upsample48k_apply()`/
     `tx_pipeline.c` cleanly enough to actually decode on the other end,
     and real wattmeter/ALC calibration for `DIGITAL`'s own TX power (§9
     below, unchanged from before this step - a level problem here is a
     different, still-open question from whether the audio arrives at
     all).
   - **Follow-up: on-air report of ~10x fewer FT8 decodes than a raw-I/Q
     path - chased to a real bug and fixed.** With audio and rig control
     both confirmed (steps 11/12), the operator compared this gadget's
     WSJT-X decode count directly against SparkSDR consuming raw I/Q
     over `hpsdr_p1` (`04_remote_control_and_iq_output.md`) into its own
     copy of WSJT-X: SparkSDR's path sees roughly 10x more FT8 signals.
     Chased on the bench in
     [`rx_uac_out_digital_mode_bandwidth.md`](dsp_design_notes/rx_uac_out_digital_mode_bandwidth.md).
     A first pass measured `uac_out`'s actual passband by sweeping a
     synthetic complex tone across dial offsets (feeding
     `rx_audio_process()` the same way `rx_audio_test.c` does), and
     pointed at stage 3's narrow filter defaulting ON (nothing ties
     `narrow_filter_enabled` to `radio_get_mode() ==
     RADIO_MODE_DIGITAL`) as the leading suspect - squeezing `uac_out`
     to ~200Hz instead of FT8's ~2.7kHz. The operator then confirmed
     both filters were already off during the original comparison,
     ruling that out, and asked directly whether stage 1 could use one
     design for both CW and digital modes. It already can - CW's own
     selectivity lives in stage 3, not stage 1, so there's no real
     conflict, and the bench sweep's own OFF-state numbers already
     showed stage 1 passing a clean ~3kHz one-sided band, close to what
     FT8 needs. Chasing that question down instead surfaced the real
     bug: `usb_gadget.c`'s `UAC_RX_AUDIO_SCALE` mapped `rx_audio.c`'s
     `AGC_TARGET_AMPLITUDE` directly onto 16-bit PCM full scale with
     zero headroom - so with stage 3 off (required for FT8), a single
     lone steady tone alone already clipped 27% of the time, worsening
     with more simultaneous tones present (measured to +6.4dB over the
     old full-scale reference at 60 tones) - real FT8 band conditions,
     and a far better-quantified explanation for a 10x deficit than any
     filter shape, since clipping sprays intermodulation splatter across
     the whole sub-band and degrades every decode on the band at once,
     not just ones outside some passband. **Fixed:** `UAC_RX_AUDIO_SCALE`
     now carries a 15dB headroom margin (`UAC_RX_AUDIO_HEADROOM`),
     isolated entirely to `usb_gadget.c` - `rx_audio.c`'s AGC and local
     CW listening (`out[]`) are untouched. Re-measured: the single-tone
     27% clip rate and the 20-tone 1.86% clip rate both drop to 0.00%,
     with 8.6dB of margin still spare at 60 simultaneous tones. Full
     rebuild clean under `-Wall -Wextra`; every existing DSP test
     harness (`test-rx-audio`, `test-fft-filter`, `test-rx-filter`,
     `test-upsample48k`, `test-tx-pipeline`) still passes unchanged -
     none link `usb_gadget.c`, so this confirms the DSP chain itself
     wasn't touched. Code-complete; on-air re-confirmation of the
     original SparkSDR comparison is still outstanding - see the linked
     note's §9 for what's still open.
   - **Follow-up: the actual root cause - a CW-only demod receiving the
     wrong sideband. Fixed.** The clipping fix above was real but not
     the main problem. A simultaneous on-air A/B (20m, 14.074.000,
     `DIGITAL`) had SparkSDR on `hpsdr_p1.c` I/Q decoding ~30 stations
     per interval while WSJT-X on this gadget's audio decoded one or
     two - both fed the *identical* `sound.c` I/Q, which put the fault
     squarely inside `rx_audio.c`. `rx_audio.c` was a CW monitor in
     every mode: stage 2 always added a 700Hz `CW_PITCH_HZ` offset, and
     stage 1 always kept positive baseband - which, because maxibitx's
     raw I/Q is spectrally inverted (a station `+d` above dial arrives
     at `-d`; `tools/rigctl_panel.py` had documented and corrected this
     for its own display all along), meant the *lower* sideband. Real
     FT8 above dial only leaked through stage 1's -40..-75dB stopband,
     folded around 700Hz; FT8 below dial arrived mirrored and
     undecodable. **Fixed:** `rx_audio_set_demod()` (`RX_DEMOD_CW`/
     `USB`/`LSB`), driven from `radio_set_mode()`, with `DIGITAL` →
     USB. USB/LSB pick their sideband with one input conjugation
     (`RX_IQ_SPECTRUM_INVERTED`) and demodulate with no BFO offset, so
     audio == `|RF - dial|`. CW is bit-identical to before. The
     correction is deliberately not in `sound.c`, which would also flip
     SparkSDR's working I/Q. **On-air confirmed (2026-09-21):** a +100Hz
     dial step moves signals left in the audio WSJT-X waterfall (correct
     USB sense, so the constant is right), and WSJT-X on the gadget's
     audio decodes on par with SparkSDR in a simultaneous A/B (~40 each
     in one 20m interval, SNRs typically within 1dB - was ~30 vs. 1-2
     before). The original ~10x deficit is closed. Full record, including a misdesigned test that
     briefly got this hypothesis wrongly dismissed:
     [`rx_uac_out_digital_mode_bandwidth.md`](dsp_design_notes/rx_uac_out_digital_mode_bandwidth.md)
     §10.

10. Power/ALC calibration for voice, per §9. Planned in
    [`dsp_design_notes/tx_test_tones_and_alc.md`](dsp_design_notes/tx_test_tones_and_alc.md).
    Built and bench-tested so far: the TX test-tone generator
    (`tone_gen.c`, rigctld `U TONE`), the carrier-placement fix it
    measured, and the peak limiter with its `RFPOWER`/`ALC` controls.
    Still to do: the two-tone power and IMD measurements that set this
    board's honest rated output, and a wattmeter check of real speech.

11. **Done, confirmed on air (2026-09-23 FT8 contact).** Simplified
    step 9's UAC2 gadget audio from stereo-duplicated to genuinely mono,
    both directions, prompted by the operator's own real-world bring-up:
    initial Windows/WSJT-X testing of step 9's build got no further than
    driver-level enumeration succeeding (confirmed via Device Manager's
    "Devices by connection" view - both a "Microphone" and "Speakers"
    child endpoint correctly bound under the "sBitx Audio" composite
    device) - WSJT-X itself never successfully decoded or transmitted
    through it. Rather than keep debugging with step 9's stereo-
    duplication hedge still in place, the hedge itself was removed as the
    simplification most likely to help and, independently, the more
    correct design regardless of whether it turns out to be the actual
    fix.
    - **What the hedge was and why it was never load-bearing:** step 9's
      `uac_gadget_create()` set `c_chmask`/`p_chmask` to `"3"` (2 channels,
      both wire channels present) purely defensively - "hedging for host/
      app compatibility with devices that assume a stereo interface," per
      the code's own comment - not because anything in this project's own
      signal chain is stereo. `rx_audio.c`'s `uac_out` tap and
      `tx_pipeline.c`'s TX input were always a single mono rail;
      `uac_writer_thread()` was writing the identical sample onto both
      wire channels, and `uac_reader_thread()` was reading both back and
      averaging them down to the one mono value `upsample48k_apply()`
      actually wanted. Real ham-radio digital-mode USB audio interfaces
      (SignaLink USB, RigBlaster, and similar) are universally mono, both
      directions - mono is the normal, expected case for this exact
      application, not an edge case that needs a stereo hedge.
    - **Changed:** `usb_gadget.h`'s configfs doc comment and
      `usb_gadget.c`'s `uac_gadget_create()` (`c_chmask`/`p_chmask` from
      `"3"` to `"1"` - one channel, ch0 only), `UAC_CHANNELS` (2 to 1),
      `uac_writer_thread()`'s packing loop (writes one 16-bit sample per
      frame instead of duplicating it onto `slot[0]`/`slot[1]` and
      `slot[2]`/`slot[3]`), and `uac_reader_thread()`'s unpacking loop
      (reads one `int16_t` directly instead of reading and averaging a
      `left`/`right` pair). Nothing upstream or downstream of the two ring
      buffers changed - `decim48k_apply()`, `upsample48k_apply()`,
      `rx_audio.c`'s `uac_out` tap, and `tx_pipeline.c`'s TX input are all
      unchanged, since this only touches how a sample is packed onto or
      off of the USB wire, not the DSP itself.
    - **Regression check:** full rebuild (`make clean && make`) clean
      under `-Wall -Wextra`; `test-fft-filter`, `test-tx-pipeline`,
      `test-rx-filter`, `test-rx-audio`, and `test-upsample48k` all still
      pass with numbers matching their own steps' original bench results
      - expected, since none of those harnesses touch `usb_gadget.c` at
      all (it isn't linked into any of them - it needs real configfs/ALSA
      gadget hardware none of the harnesses provide), so this is
      confirmation the DSP chain feeding the gadget is untouched, not a
      test of the gadget change itself.
    - **What's genuinely still open, not yet bench- or air-verified:**
      whether this actually fixes WSJT-X decode/encode. Step 9's own
      failure mode was never root-caused - no dmesg/ALSA error was ever
      captured showing *why* WSJT-X didn't work with the stereo-
      duplicated build, so this change is the most standards-aligned
      simplification available (matching real hardware precedent and
      removing a hedge that was never necessary), not a confirmed fix for
      a diagnosed bug. Needs the same real end-to-end WSJT-X test step 9
      itself never got - if WSJT-X still doesn't work against a genuinely
      mono gadget, the channel count wasn't the problem and the real
      cause is still unfound.
    - **Follow-up: USB serial number bumped `0000001` -> `0000002`, as a
      deliberate one-time cache break.** Bench work on the real Windows
      11 host turned up the specific mechanism behind "WSJT-X and
      Audacity both refuse to open this device": the gadget advertises
      exactly ONE audio format and no alternates (`c_srate`/`p_srate`
      48000, `c_ssize`/`p_ssize` 2, `c_chmask`/`p_chmask` 1 - see
      `uac_gadget_create()`), which is far more rigid than a typical
      commercial USB audio device. Windows' legacy wave mapper
      ("Microsoft Sound Mapper - Input") performs automatic sample-rate
      and channel conversion between what an app asks for and what the
      device really supports; opening a specific device *by name* does
      not, and fails outright on any mismatch. That asymmetry was
      directly observed: inside one Audacity instance, the mapper path
      streamed real audio that tracked maxibitx's own volume control
      live, while a direct open of the same device returned PortAudio
      -9999 ("Unanticipated Host Error") - which also rules out the
      usual -9999 causes (mic privacy, exclusive-mode lock, device busy),
      since all of those would have broken the mapper path too.
    - **Why the serial number is implicated at all:** Windows caches an
      audio endpoint's believed format keyed on VID/PID/serial, and this
      gadget deliberately holds all three fixed so a host sees one stable
      device across replugs (and the ACM function keeps its COM port
      number). That stability is exactly what lets a host go on believing
      this device is stereo after step 11 made it mono - and no amount of
      unplugging/replugging clears it, because nothing in the identity
      changed. Bumping the serial forces one clean break: the host
      enumerates an entirely new device and builds its endpoint state
      fresh from the current descriptors. Deliberately a fixed new value,
      NOT randomized per run - a serial that changed on every start would
      leave a trail of ghost device instances and hand FLRig a different
      COM port on every restart.
    - **Known, accepted side effect:** because the host sees a new device,
      Windows assigns the CAT/ACM function a NEW COM port number. FLRig
      (and anything else pointed at the old port) has to be repointed
      once, after which the new port is as stable as the old one was.
    - **Still unverified at the time of writing:** whether the bump
      actually clears the symptom. If it does, the cached-stale-format
      theory is confirmed; if a freshly-enumerated device still refuses
      direct opens, the cache was never the problem and the remaining
      suspect is simply the app-side format request (Audacity's project
      rate defaults to 44100 and its recording channels to 2; WSJT-X's
      Settings -> Audio input channel selector must be on "Mono"), which
      is an operator-facing setup matter for
      `10_external_digital_modes_wsjtx.md` (since folded into `06_api.md`) rather than a code defect.
    - **Confirmed on the bench: the serial bump worked.** Audacity opens
      "Microphone (Source/Sink)" by name, and **WSJT-X is decoding**.
      That settles the diagnosis above: Windows had been holding cached
      endpoint format state against the old fixed VID/PID/serial, and
      replugging could never clear it. Step 11's own "not yet
      bench-verified" caveat is closed for the RX/audio half.

12. **Done, confirmed on air (2026-09-23 FT8 contact).** Fixed the
    Kenwood `IF` reply's field layout, which was silently breaking
    Hamlib-based rig control. With audio working (step 11), WSJT-X moved
    on to its next complaint: "Rig Control Error" when configuring rig
    control, against a CAT surface that FLRig had been driving happily
    for weeks.
    - **Root cause, and why FLRig never noticed:** WSJT-X drives rig
      control through Hamlib, and Hamlib's `ts480.c` wires
      `.get_vfo = kenwood_get_vfo_if`, so VFO, PTT and split all come
      from parsing the `IF;` reply at FIXED character offsets.
      `kenwood.c` defaults `if_len` to 37 for any backend that doesn't
      override it (ts480.c doesn't), where `if_len` counts the reply
      including `"IF"` but excluding `';'` - so the wire reply must be
      exactly 38 bytes. Ours was 39, and because its RIT field was 5
      chars where the real layout is 6 (sign + 5 digits), RX/TX and the
      mode digit both sat one position late. A length mismatch is a hard
      `-RIG_EPROTO` after the port's retries, not something Hamlib
      shrugs off. FLRig never cared because its parser is far more
      forgiving, and `rig_open()` itself had always succeeded - Hamlib's
      `kenwood_open()` only hard-fails when `ID;` goes unanswered, and
      ours has always answered `ID020;`. That is exactly why CAT looked
      completely healthy from FLRig (frequency and volume control both
      worked, clean attach/detach) while WSJT-X refused to connect: the
      two clients exercise entirely different parts of the same surface.
      The `IF` comment in `usb_gadget.c` had flagged this field layout as
      an unverified reconstruction since it was written; it was wrong.
    - **Verified, not assumed:** the corrected format string is pulled
      back out of `usb_gadget.c` programmatically and exercised across
      six cases (RX/TX, both RIT extremes at `RIT_MAX_HZ` = ±9999, three
      bands, three modes), asserting reply length 37 excluding `';'` and
      the PTT/mode/VFO digits landing on offsets 28/29/30 - the literal
      indices Hamlib's `kenwood_get_ptt()`, `kenwood_get_mode()` and
      `kenwood_get_vfo_if()` read. All six pass.
    - **`RADIO_MODE_DIGITAL` now reports as `'2'` (USB), closing the
      digit-9 question this project had been carrying as an open flag.**
      Two independent sources agree the old `'9'` was wrong: QRP Labs'
      QMX CAT manual documents digit 9 as "FSK Reverse", and Hamlib's
      default `kenwood_mode_table` (which is what a TS-480 gets, since
      ts480.c defines no table of its own) maps 9 to RTTY-REVERSE. That
      table has no DATA/PKT digit anywhere in the 1-9 range an `IF`
      response can express - a real TS-480 simply has no data mode - so
      `'9'` was making WSJT-X display the rig as RTTY-R. `'2'` is the
      truthful answer: FT8 *is* upper sideband, and "run the rig in USB
      and let the digital-mode app own the audio" is how a TS-480 is
      operated for data.
    - **The one judgment call, flagged deliberately:** because DIGITAL
      now reports as USB, a host that reads the mode back and writes it
      out again - which WSJT-X does routinely - would walk the radio out
      of DIGITAL into plain USB, switching the TX source from
      `uac_pull_audio_tx()` to the mic and killing transmit. The MD set
      handler therefore treats an incoming USB request while already in
      DIGITAL as the no-op it was almost certainly meant to be. Any
      other mode request (CW, LSB) still applies normally, so this
      cannot strand the radio in DIGITAL. The trade accepted here is
      that DIGITAL becomes locally-selectable only, not reachable or
      distinguishable over CAT - which is the same position a real
      TS-480 is in, and is worth revisiting if a future client needs to
      command it.
    - **`PS` and `AI` added:** Hamlib's `kenwood_open()` queries both and
      tolerates a timeout on either, so neither is required for
      correctness - but each unanswered query burns the port's full
      retry budget on every open, for nothing. `PS;` answers `PS1;`
      (maxibitx is trivially "on" whenever it is answering CAT at all)
      and `AI;` answers `AI0;` (this surface never pushes unsolicited
      status). Sets for both are accepted and ignored rather than
      honored - there is no software power switch to throw, and
      auto-reporting isn't implemented.
    - **Regression check:** full rebuild clean under `-Wall -Wextra`;
      `test-fft-filter`, `test-tx-pipeline`, `test-rx-filter`,
      `test-rx-audio`, `test-upsample48k` all pass. As in step 11, none
      of them link `usb_gadget.c`, so they confirm the DSP chain is
      untouched rather than testing the CAT change - that is what the
      six-case `IF` harness above is for.
    - **Still open:** whether WSJT-X's Test CAT now passes end to end,
      and whether PTT over CAT keys the radio correctly from WSJT-X (as
      opposed to from FLRig, which was already working). Also untested:
      whether WSJT-X's split operation works, since Hamlib reaches split
      through `FR`/`FT`/`SP`, none of which this surface implements yet -
      split should be left off in WSJT-X until it does.
    - **Follow-up: on-air confirmed, but not the way this step
      expected.** WSJT-X's rig control kept failing even after the `IF`
      fix above, with the same symptom on both sides: WSJT-X's own CAT
      Settings refused to save, and FLRig's frequency changes never
      reached WSJT-X. The cause was not this surface at all - WSJT-X's
      own Hamlib rig type had been pointed directly at the same COM port
      FLRig already held open, and a serial port is exclusive-access on
      Windows. Neither app fails loudly when this happens; both just
      look uncommunicative. The fix was entirely on the Windows/app side:
      point WSJT-X's Rig dropdown at its dedicated `Flrig` entry (an
      XML-RPC relay to FLRig, not a second serial connection) instead of
      at a Kenwood/Hamlib serial type, so only FLRig ever opens the port.
      Once reconfigured that way, frequency control and audio both work
      end to end on real Windows 11 + WSJT-X hardware - the first
      complete on-air confirmation of this gadget's whole `DIGITAL`-mode
      bridge (step 9's audio, step 11's mono conversion, and this step's
      `IF` fix, all three exercised together for the first time).
      **Follow-up, on-air confirmed: WSJT-X's own rig control also works
      pointed directly at this gadget's serial Kenwood CAT, with FLRig
      entirely out of the loop.** So the `IF` fix above was the whole
      story for direct CAT after all - the FLRig-relay workaround was
      necessary only because of the unrelated port-contention bug
      immediately above, not because WSJT-X's own Hamlib client needed
      anything different from what FLRig gets. Two independent Hamlib
      clients (FLRig, WSJT-X) now both drive this surface correctly.
      **Still not confirmed:** `hamlib.c`'s separate rigctld surface (TCP
      4532) has not been tried as WSJT-X's `NET rigctl` rig type at all -
      untested either with or without FLRig in the picture, since direct
      serial CAT already covers the need. See
      `10_external_digital_modes_wsjtx.md` (since folded into `06_api.md`) for the operator-facing setup
      steps and the two other things confirmed on the same bench pass
      (the gadget enumerates as "Microphone (Source/Sink)"/"Speakers
      (Source/Sink)", never as "sBitx Audio," by name; and it advertises
      exactly one audio format with no alternates, which is why opening
      it by name can fail even when Windows' generic Sound Mapper path
      to the same device works fine).
