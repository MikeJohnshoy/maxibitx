# maxibitx — architecture and design rationale

Status: §10 steps 1-3 done. Step 1: this tree is minibitx's `src/`,
`docs/`, `data/`, and `tools/` carried over unchanged (only
`src/maxibitx.c`'s filename and the Makefile's output binary name
changed), confirmed building and running on real hardware. Step 2: the
shared FFT overlap-save filter (`src/fft_filter.c`/`.h`) is ported and
bench-verified against synthetic tones (`src/fft_filter_test.c`). Step
3: mode is a real, single-owner value now (`radio_set_mode()`/
`radio_get_mode()`, `radio.c`), agreed on by both control surfaces — see
§10 for the actual measured/verified detail on both. Nothing past that
has started: `cw.c`'s separate TX carrier and `rx_audio.c`'s fixed
narrow filter are still exactly as minibitx shipped them, and neither
the shared filter nor the real mode state is wired into an actual audio
decision yet (steps 4-5). This document is both the
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
  - DIGITAL (placeholder, v1): same source as SSB, unchanged - an
         external digital-mode app (WSJT-X on a host PC first) already
         generates its own audio and just needs it to reach input_mic
         over the existing mic/line-in path; no new i_sample branch
         needed for this alone. Kept as a distinct mode value (not
         reused MODE_USB) purely so ALC/power calibration and CAT mode
         reporting can differ later - see §8, §9. Onboard generation/
         decoding (an actual FT8 encoder/decoder running on-target)
         stays deferred; this placeholder is only about naming the
         mode, not building that.
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
same block size, same Kaiser-window construction — operating on the
already-demodulated audio coming out of stage 2. Two live parameters
instead of zero: pitch (where stage 2 already mixes to, unchanged) and
width (now a real `filter_tune()` passband argument instead of a
baked-in 300Hz). No coefficient regeneration, no rebuild, for either.

This directly resolves the original "how do we handle `CW_PITCH_HZ`
changing" question from earlier in this project: on RX, changing pitch
now just means re-tuning stage 2's existing mixing oscillator *and*
re-calling `filter_tune()` with the new center — both cheap, both
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

**Not done yet:** `radio_tx_apply()`/`sound.c`'s audio thread reading
that real mode to decide which `i_sample` source feeds the shared TX
pipeline this block (the keyer's envelope output for CW, mic audio for
USB/LSB, and for `DIGITAL` the same mic/line-in audio as SSB, sourced
from a host PC's digital-mode app instead of a human voice) and which
sideband-zero branch applies — the same single mode check `tx_process()`
already uses for these decisions. That's steps 4/5, once the shared
pipeline itself is wired into `cw.c`/`rx_audio.c`; mode being real is
the precondition for that, not that work itself.

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
  item, since this is exactly that kind of new per-block compute.
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
     constant added later (step 7) calibrates real analog/mixer gain,
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
   Not yet done: wiring this into the real pipeline (steps 3-5 below),
   or a wisdom-file cache for `filter_new()`'s `FFTW_MEASURE` plans
   (see `fft_filter.c`'s comment on why that's deferred, not skipped).
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
4. Migrate CW TX onto the shared pipeline (still bench-only), re-verifying
   dial accuracy and image rejection against the same on-air numbers
   `docs/03_tx_processing_pipeline.md` already recorded for the old
   direct-to-DAC path, plus the envelope/block-boundary check from §9 —
   before ever touching SSB on the air.
5. Migrate `rx_audio.c`'s stage 3 onto the same shared pipeline (§5),
   with pitch/width as live parameters — bench-verified against
   synthetic signals first (same `test_rx_audio.c`-style methodology),
   then an on-air listening comparison against the existing elliptic
   filter per §9's skirt-quality check, before removing
   `narrow_filter_coeffs[]` for good.
6. First on-air SSB TX test, CAT-triggered PTT, one band, conservative
   drive level — measure actual sideband rejection before touching
   power calibration at all.
7. Power/ALC calibration for voice, per §9.
8. Physical PTT input (if wanted) once CAT-only testing is done.
