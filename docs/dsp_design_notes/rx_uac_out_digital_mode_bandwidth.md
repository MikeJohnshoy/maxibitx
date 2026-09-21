# RX Audio Bandwidth Reaching WSJT-X (`uac_out`) vs. a Raw-I/Q Path

Status: **implemented and on-air confirmed (§10, 2026-09-21)** - the
inversion constant's sign was checked directly, and WSJT-X on the
gadget's audio now shows many more signals. A full decode-count A/B
against SparkSDR hasn't been recorded yet. Read §10 first: it holds the actual root cause, found
from a simultaneous on-air A/B against SparkSDR, and it **corrects**
two conclusions earlier sections reached. `rx_audio.c` was a CW-only
demodulator in every mode, and because maxibitx's raw I/Q is spectrally
inverted, it was receiving the *lower* sideband with a +700Hz pitch
offset whenever WSJT-X was connected - real FT8 above dial only leaked
through stage 1's stopband. Fixed with a mode-aware demod (§10). §7's
claim that stage 1 "already works for everything" and §8's claim that
clipping was "the real bug" / the dominant cause were both wrong: §8's
clipping was real and its fix stands, but it was a secondary issue.
§§1-9 are kept as written, as the record of how this was chased.

## 1. Why this note exists

On-air report, after `10_external_digital_modes_wsjtx.md`'s audio and
rig-control setup were both already confirmed working
(`ARCHITECTURE.md` §10 steps 11/12): *"I have an SparkSDR using IQ
data via hpsdr_p1, using wsjtx for decodes, and it sees 10x more ft8
signals than usb_gadget using wsjtx for decodes."*

Both paths end in the same WSJT-X decoder; they differ only in what
feeds it. SparkSDR gets raw baseband I/Q over `hpsdr_p1.c`
(`04_remote_control_and_iq_output.md`) and does its own demodulation
and windowing, free to hand WSJT-X as wide an audio window as it
likes. The UAC2 gadget instead hands WSJT-X `rx_audio.c`'s `uac_out`
tap directly - whatever comes out the far end of the same 4-stage
local-CW-monitor chain a human listening for CW on headphones gets
(`rx_audio_demod_design.md`), on the explicit reasoning recorded when
that tap was built (`ARCHITECTURE.md` §10 step 9): *"Deliberately NOT
independent of the AGC too - AGC is signal-conditioning, not an
operator loudness preference, so a remote decoder wants it exactly as
much as the local speaker does."* That reasoning was written with one
CW signal in mind. FT8 is a different animal - dozens of simultaneous
tones spread across roughly a 2.7kHz audio window - so it was worth
finding out, with real numbers, whether "wants it exactly as much as
the local speaker does" still holds for stage 3's ~300Hz single-signal
selectivity filter specifically.

## 2. Two candidate causes, identified by reading the code first

- **`narrow_filter_enabled` defaults ON** (`rx_audio.c`, file scope:
  `static int narrow_filter_enabled = 1;`), and nothing anywhere ties
  it to `radio_get_mode() == RADIO_MODE_DIGITAL` - confirmed by an
  exhaustive grep across every `src/*.c` and `src/interfaces/*.c`. If
  an operator switches from listening to CW into running WSJT-X
  without separately disabling this, `uac_out` is stage 3's filtered
  output, not stage 1/2's wider signal.
- **Even with stage 3 disabled**, stage 1's image-reject complex FIR
  (327 taps, real-lowpass prototype passband edge 1500Hz/stopband edge
  1900Hz, modulated up by 1500Hz to become one-sided) and stage 2's
  fixed mix up to `CW_PITCH_HZ` (700Hz, `cw.h`) were both designed
  around one CW tone landing somewhere near dial center, not around
  preserving a wide, symmetric window for a wideband digital mode.

Both were plausible from reading the code alone; the point of this
note is to stop guessing and measure `uac_out` directly.

## 3. Measurement setup

A synthetic sweep harness links exactly the modules `rx_audio_test.c`
itself links (`rx_audio.c`, `vfo.c`, `fft_filter.c`, `rx_filter.c`) -
not `radio.c`, which `rx_audio_process()` has no dependency on at all
(confirmed by grep: no `radio_`-prefixed symbol appears anywhere in
`rx_audio.c`). For each of a range of dial offsets from -2000Hz to
+3500Hz, the harness feeds `rx_audio_process()` a single complex tone
`i[n] = cos(2*pi*f*n/Fs)`, `q[n] = sin(2*pi*f*n/Fs)` - a pure one-sided
complex exponential at frequency `f` relative to dial center, the same
representation a real signal sitting `f` Hz away from where the VFO is
tuned would produce in baseband I/Q. 60 blocks of 2048 samples (~1.28s)
run first so the AGC's envelope (`AGC_ATTACK_MS`/`AGC_RELEASE_MS`) is
fully settled before one more block's `uac_out` is measured (RMS over
its second half, well clear of any block-boundary transient). Repeated
across both states of `narrow_filter_enabled`. Every offset's RMS is
reported in dB relative to whichever offset came out loudest in that
same run, i.e. this measures the *shape* of the passband reaching
WSJT-X, not its absolute level (`UAC_RX_AUDIO_SCALE` is a separate,
already-flagged open question, `ARCHITECTURE.md` §10 step 9).

Kept as ad hoc bench code, not committed to the tree - the same
convention `rx_narrow_filter_fft_vs_elliptic.md`'s measurements
followed (a throwaway harness, findings recorded here in prose, no new
permanent test case added to `rx_audio_test.c`).

**Two mistakes caught before trusting any number, worth flagging for
whoever builds the next one of these:** the first build attempt
speculatively linked `radio.c` too, which pulled in a chain of
hardware-only symbols (`si5351bx_setfreq`, `radio_hw_set_ptt`, and
others) that don't resolve without linking `radio_hw.c`/`si5351v2.c`/
`sound.c` as well - dropped it once grep confirmed `rx_audio_process()`
never needed it. Separately, the first build that *did* link cleanly
reported exactly 0.0 at every single offset, both filter states - not
small, exactly zero. That traced to the harness never calling
`vfo_init_phase_table()` before `rx_audio_init()`. `rx_audio_test.c`
itself calls it first (`vfo.h`'s own doc comment: "Call once at
startup, after vfo_init_phase_table()"); without it, `vfo_read_iq()`'s
lookup table (`vfo.c`'s file-scope `phase_table[]`) is all zeros, so
stage 2's mixer (`audio = fi*c - fq*s`) silently multiplies by zero
every sample regardless of input. An all-exact-zero result across an
entire sweep was the tell that this was a harness bug, not a real
measurement, and it was fixed before any number below was trusted.

## 4. Results

Dial offset is relative to where the VFO is tuned (0 = a signal
exactly at dial center). Because stage 2 always mixes up by the fixed
`CW_PITCH_HZ` (700Hz), a signal at dial offset `X` reaches WSJT-X as an
audio tone at `700 + X` Hz - useful for translating these numbers
against WSJT-X's own audio window later.

**Narrow (stage 3) filter ON - the default state, nothing auto-disables
it for `DIGITAL` mode:**

| offset (Hz) | uac_out (dB, rel. peak) |
|---:|---:|
| -300 | -70.1 |
| -200 | -31.3 |
| -100 | -4.7 |
| -75 | -3.6 |
| **-50** | **-2.9** |
| 0 | -1.8 |
| 75 | -0.2 |
| **150** | **0.0 (peak)** |
| 200 | -11.9 |
| 300 | -34.5 |
| 700 | -52.9 |
| 1500 | -58.3 |
| 3000 | -53.3 |

-3dB points: roughly **-50Hz to +150Hz** - a **~200Hz-wide** passband,
matching a single-CW-tone filter (the elliptic's own design point is a
~300Hz -3dB width centered on `CW_PITCH_HZ`; this measured width is the
same design seen from the dial-offset side, and the small numeric
difference from the nominal 300Hz is unsurprising given the elliptic's
own passband ripple). Everything else - -50 to -60dB out at 700-3000Hz
- is effectively gone.

**Narrow filter OFF:**

| offset (Hz) | uac_out (dB, rel. peak) |
|---:|---:|
| -400 | -41.1 |
| -300 | -19.4 |
| -200 | -10.2 |
| -100 | -4.8 |
| **-25** | **-2.3** |
| 0 | -1.7 |
| 200 | -0.1 |
| 1500 | 0.0 (peak) |
| 2200 | -0.0 (peak) |
| 2800 | -0.1 |
| **3000** | **-1.7** |
| 3050 | -3.1 |
| 3200 | -10.3 |
| 3500 | -41.3 |

-3dB points: roughly **-25Hz to +3000Hz** - a **~3025Hz-wide**
passband, but sharply asymmetric around dial center: it falls off fast
on the low side (already -10dB by 200Hz below center, -41dB by 400Hz
below) while staying essentially flat (equiripple, consistent with
stage 1's remez design) out to ~3000Hz above center before rolling off
toward the stopband by ~3200-3500Hz. This is stage 1's own image-reject
filter shape (`rx_audio_demod_design.md` §7), not stage 3 - it's
present whether or not stage 3 is enabled.

## 5. Interpretation against what WSJT-X actually wants

WSJT-X's FT8 sub-band conventionally spans roughly 200-2900Hz of audio
(~2.7kHz), with real stations' tones spread across that whole range.
Using `audio = 700 + offset`: audio 200Hz needs dial offset -500Hz;
audio 2900Hz needs dial offset +2200Hz. So a conventionally-tuned FT8
window corresponds to roughly **-500Hz to +2200Hz** of dial offset.

With the narrow filter left ON (its default, and nothing disables it
for `DIGITAL` mode), essentially none of that range survives except a
~200Hz sliver near offset 0 - only signals landing within about 150Hz
of dial center get through at all. That is a bandwidth reduction on
the order of 15x relative to the ~2.7kHz FT8 wants, in the same ballpark
as - and a fully sufficient explanation for - the reported ~10x decode
deficit, with no code change needed to explain it: it is simply
whatever state the filter happened to be left in.

With the narrow filter OFF, the picture is much better but not perfect:
the passband (-25 to +3000Hz offset) covers almost all of the wanted
-500 to +2200Hz range on its upper two-thirds, but the low end of a
conventionally-tuned FT8 window (roughly -500 to -100Hz offset, i.e.
audio 200-600Hz) sits in territory that is already down 5-40dB and
falling fast - real signals in that part of the sub-band would still
be substantially suppressed, though nowhere near as catastrophically
as under the first effect. This second effect is a smaller, structural
mismatch specific to a wideband digital-mode consumer (it's
`rx_audio.c` stage 1's own image-reject design, built around one CW
tone, showing up as an asymmetric window when asked to pass a much
wider band) - real, measured, but secondary to whether stage 3 was
engaged at all.

## 6. What this does and doesn't establish

The leading, best-supported explanation for the reported 10x gap is
that the narrow (stage 3) filter was left in its default ON state
while running WSJT-X - there is no code path that disables it for
`DIGITAL` mode, and nothing on screen would remind an operator moving
from CW listening to FT8 to turn it off. That alone measures out to
roughly the right order of magnitude, and is checkable/fixable purely
operator-side today, no code change needed
(`rx_audio_set_narrow_filter(0)`, or `tools/rigctl_panel.py`'s own
control for it).

What this note does NOT establish: whether the operator's own
SparkSDR-vs-usb_gadget comparison already had the narrow filter
disabled (if it did, the measured stage-1/2 asymmetry above becomes the
primary remaining suspect instead of a secondary one), and whether
either effect, once addressed, actually closes the reported gap on
air - both are one real A/B test away rather than a bench-only
question at this point.

## 7. New information: both filters were already off

The operator confirmed, after reading §§1-6, that *both* filters
(stage 3's narrow selectivity filter, and its FFT/elliptic
implementation choice) were already off during the original
SparkSDR-vs-usb_gadget comparison - ruling out §6's leading hypothesis
entirely. That leaves the smaller, secondary effect from §5/§6 (stage
1's own passband asymmetry) as the only *frequency-response* candidate
left, and prompted a direct question: **can stage 1 use one filter
design that works for both CW and digital modes**, rather than
maintaining (or inventing) separate designs?

The answer turned out to be yes, but not for the reason the question
assumed - and chasing it down surfaced a real bug that has nothing to
do with stage 1's shape at all.

**Why stage 1 was never really in conflict between the two modes:**
CW's own fine selectivity is stage 3's job, not stage 1's - stage 1
exists purely to pick one sideband of the raw baseband I/Q and reject
the other (`rx_audio_demod_design.md` §7.5's "which side is wanted").
Both CW and FT8 want the *identical* selection (upper sideband,
matching the universal FT8 convention already documented in
`10_external_digital_modes_wsjtx.md` §2) - there is no real design
tradeoff between them here, because CW's narrowing happens downstream,
in a stage FT8 already needs disabled. And §4's own OFF-state
measurement already showed stage 1 passing a clean, flat, correctly
one-sided ~3000Hz-wide band above dial center - not far at all from
what FT8's ~2.7kHz sub-band needs, once §5's assumption about where
real FT8 traffic sits relative to dial center is set aside (a
correctly-tuned FT8 dial, per convention, parks *below* all wanted
traffic, putting it entirely on stage 1's already-passed, positive-
offset side - the §5 concern about content needing to come from
*below* dial center may not reflect how FT8 is normally operated at
all). So: one stage 1 filter, unchanged, is already the right answer
for both modes' *frequency response*. It was never necessary, and
would likely be counterproductive, to widen it toward the rejected
sideband - that side carries other stations' real, independent
traffic, not a self-image, and letting it through would reintroduce
genuine interference for both CW and digital use.

## 8. The real bug: `uac_out` clips at the 16-bit PCM stage

Ruling out stage 1's shape raised an obvious question: if the
passband was already fine, what *does* explain a 10x decode deficit?
Re-reading `usb_gadget.c`'s own scaling comment for the answer -
`UAC_RX_AUDIO_SCALE` maps `rx_audio.c`'s `AGC_TARGET_AMPLITUDE`
(500000000.0) directly onto 16-bit PCM full scale (32767), with no
margin at all - and checking what that means once stage 3 is off
(the confirmed, required condition for FT8) turned up real clipping.

**Mechanism:** `uac_out = narrowed * gain`, where `gain =
AGC_TARGET_AMPLITUDE / agc_env` and `agc_env` tracks the *raw input's*
own magnitude - so `gain` is whatever it takes to bring the raw input
up to the AGC's target, regardless of the input's real-world scale.
With stage 3 bypassed, `narrowed` is stage 1+2's output, whose own
passband gain at a signal's frequency is close to unity but not
exactly - stage 1's equiripple design (`rx_audio_demod_design.md` §7)
has passband ripple that measures 0.3-0.7dB *above* unity at some
frequencies. Multiplying that by `gain` (calibrated to land exactly
on `AGC_TARGET_AMPLITUDE`) means a single steady tone, alone, with
nothing else present, already overshoots the exact level
`UAC_RX_AUDIO_SCALE` maps to full scale.

**Measured (extending §3's harness with the same modules, no
`radio.c`):** a lone tone at a flat-passband frequency (dial offset
1500Hz, narrow filter off) drives `uac_out` to +0.58dB over the old,
headroom-free full-scale reference - **27.2% of samples clipped**
after 16-bit packing, from one single steady signal with nothing else
on the band at all. Adding more simultaneous tones (a synthetic
"busy band" - N equal-power tones spread across 100-2900Hz offset,
their combined RMS held constant so the AGC settles to the same
overall envelope regardless of N) makes it worse in the way ordinary
multi-tone crest factor predicts - independent tones' peaks
occasionally align:

| simultaneous tones | peak level vs. old full-scale reference |
|---:|---:|
| 1 | +0.58dB |
| 2 | +0.96dB |
| 5 | +1.85dB |
| 10 | +2.08dB |
| 20 | +3.34dB |
| 40 | +5.11dB |
| 60 | +6.43dB |

(Each row is the worst peak seen over a ~4.3s measurement window per
tone count - a real 12.64s FT8 transmission would have more chances
for independent tones' peaks to align, so these likely understate the
true worst case somewhat.) Real FT8 bands routinely have several to
dozens of simultaneous decodable signals, so this is squarely a real
operating condition, not an edge case - and every one of those
clipping events is a hard 16-bit saturation, which sprays broadband
intermodulation splatter across the *entire* sub-band right when
WSJT-X is trying to pull dozens of much weaker signals out of the
noise. This is a far more direct, better-quantified explanation for a
10x decode deficit than either of §2's original candidates - it
degrades every decode on the band simultaneously, not just the ones
outside some filter's passband.

**Root cause:** `AGC_TARGET_AMPLITUDE`/`UAC_RX_AUDIO_SCALE` were
seemingly never bench-checked with stage 3 off - `ARCHITECTURE.md`
§10 step 9 (which added `uac_out`) predates stage 3 gaining a
disable switch at all in any digital-mode context, and every prior
bench/on-air check of level (`rx_audio_demod_design.md`'s own
history) was done for CW listening, where stage 3's own attenuation
outside its ~300Hz passband happens to supply exactly the headroom
this calibration silently assumed would always be there.

**Fix, implemented:** `usb_gadget.c`'s `UAC_RX_AUDIO_SCALE` now
includes a fixed 15dB headroom margin (`UAC_RX_AUDIO_HEADROOM =
5.6234133`, i.e. `10^(15/20)`) below the old, headroom-free reference
point - a compile-time-only change, nothing in `rx_audio.c` or the
AGC itself was touched, so local CW listening (`out[]`) is completely
unaffected. Re-measured with the new scale: the single-tone case's
27.2% clip rate drops to **0.00%**, and the 20-simultaneous-tone case's
1.86% also drops to **0.00%**, with **8.6dB of margin still spare at
60 simultaneous tones** in the same bench sweep - a comfortable
margin for real band conditions, still leaving `uac_out` well within
16-bit PCM's ~96dB dynamic range. Full rebuild (`make clean && make`)
is clean under `-Wall -Wextra`; `test-rx-audio`,
`test-fft-filter`, `test-rx-filter`, `test-upsample48k`, and
`test-tx-pipeline` all still pass with unchanged numbers - none of
those harnesses link `usb_gadget.c` at all (same reasoning
`ARCHITECTURE.md` §10 step 11's regression check already gives), so
this confirms the DSP chain is untouched, not a test of this change
itself.

## 9. What's not decided yet

- **On-air re-test of the original SparkSDR-vs-usb_gadget comparison**
  with this fix in place - not yet done. §8's bench numbers make a
  strong case, but the reported 10x gap hasn't yet been re-measured on
  real air with real signals.
- Whether **15dB is the right amount of headroom**, versus more or
  less - chosen from the measured 40/60-tone crest-factor trend plus a
  safety margin, not tuned against a real, very busy band. Easy to
  revisit: `UAC_RX_AUDIO_HEADROOM` is the one knob, isolated to
  `usb_gadget.c`, same as `UAC_RX_AUDIO_SCALE` always was.
  `rx_audio.c`'s `out[]`/local-speaker path was deliberately left
  alone rather than also adding headroom there - it has never been
  reported as clipping, and stage 3's own narrow passband (CW's normal
  operating condition) already keeps it far from this problem.
- §5/§6's stage-1 asymmetry finding **still stands as a real,
  measured, secondary effect** (real signals landing very close to or
  slightly below dial center are still attenuated) - just no longer
  believed to be a major contributor to the reported 10x gap now that
  clipping is quantified. Not worth acting on unless the on-air
  re-test above still shows a gap after §8's fix.
- §7's "one stage 1 filter already works for everything" conclusion
  rests partly on an assumption about standard FT8 dial-tuning
  convention (dial parked below all wanted traffic) - worth confirming
  that's actually how the operator runs WSJT-X, since if the dial is
  instead parked mid-band, some real traffic would legitimately need
  the currently-rejected sideband, and stage 1 genuinely would need
  reconsidering rather than being cleared.

## 10. The actual root cause: a CW demod receiving the wrong sideband

**What finally isolated it.** The operator ran both paths off the same
maxibitx at the same moment on 20m (dial 14.074.000, `DIGITAL`, narrow
filter off, 2026-09-21): SparkSDR consuming I/Q over `hpsdr_p1.c` into
its own WSJT-X, and a second WSJT-X on the UAC2 gadget's audio.
SparkSDR decoded ~30 stations every interval; the audio WSJT-X decoded
one or two, all at 122-253Hz of audio, with SNRs that disagreed wildly
with SparkSDR's for the same stations. Its waterfall had activity
crammed into ~0-1500Hz, while SparkSDR showed the traffic where it
belongs, ~200-2000Hz above dial. Both consumers are fed the *identical*
`i_samples`/`q_samples` from `sound.c` - so the RF front end, ADC,
mixing and I/Q were all proven good, and the fault had to be entirely
inside `rx_audio.c`.

**The two defects.** `rx_audio.c` was designed as a local CW monitor
and never became mode-aware on the RX side:

- Stage 2 always mixed stage 1's output up by `CW_PITCH_HZ` (700Hz).
  Every station reached WSJT-X 700Hz higher than `RF - dial`, the
  relationship WSJT-X (and any SSB app) assumes.
- Stage 1 always keeps *positive* baseband frequencies (§4's sweep,
  which fed an idealized `i=cos, q=+sin` tone). But maxibitx's raw
  baseband I/Q is spectrally inverted: a station `+d` Hz above dial
  arrives at `-d`. `tools/rigctl_panel.py` had documented this all
  along and conjugates its spectrum for exactly this reason. The
  derivation: `sound.c` multiplies the real ~24kHz IF by
  `e^{+j2pi*RX_IF_FREQ_HZ*t}` (`vfo_read_iq()` returns I=cos, Q=+sin),
  mapping IF frequency `f` to baseband `24000 - f`; the analog chain's
  two high-side conversions (clk2 = dial + `xtal_filter_center`, clk1 =
  `xtal_filter_center + RX_IF_FREQ_HZ`) put the IF at `24000 + d`, so
  baseband = `-d`. So "positive baseband" was the band *below* dial -
  the lower sideband.

**What that predicts, and why it matches everything seen.** Re-running
§3's harness with the hardware's inverted orientation (`i=cos, q=-sin`
for a station at `+d`), `DIGITAL` mode, pre-fix code:

| station vs. dial | audio Hz | level | |
|---:|---:|---:|---|
| -1500 (below) | 2200 | 0.0dB | full strength, **mirrored** |
| -200 (below) | 900 | -0.1dB | full strength, mirrored |
| +200 (above) | 500 | -10.4dB | mirrored |
| +800 (above) | 100 | -75.1dB | folded around 700Hz |
| +1000 (above) | 300 | -44.5dB | upright, buried |
| +1500 (above) | 800 | -41.2dB | upright, buried |
| +2000 (above) | 1300 | -42.4dB | upright, buried |

Mirrored FT8 cannot decode (its tone order and Costas sync arrays run
backwards). The real traffic, above dial, only reaches WSJT-X 40-75dB
down through stage 1's stopband, folded into roughly 0-1800Hz - the
compressed low-frequency waterfall and low-audio-frequency decodes
seen at 14.074. The stopband's own ripple (-40 to -75dB, varying with
frequency) is why SNRs disagreed so badly with SparkSDR's: each
station's attenuation depended on exactly where it sat. The same model
explains the earlier 40m observations too. Dial 7.077 put the 7.074-7.077
FT8 cluster *below* dial - on the passed side, at full strength, but
mirrored: a bright, busy waterfall with few decodes. Dial 7.074 put the
traffic above dial, on the rejected side: nearly empty. Dial 7.071 put
it 3-6kHz above dial, out past everything: nothing.

**A reasoning error worth recording.** Before the SparkSDR A/B, a
sideband inversion was proposed and then wrongly dismissed. The test
offered to check it - retune to 7.071, "the mirror image of 7.077" -
was misdesigned: under the inversion hypothesis, the passed side is
*below* dial, so moving the dial down to 7.071 could never have brought
the traffic back. The null result at 7.071 was consistent with the
inversion all along, not evidence against it. The lesson generalizes:
before running a discriminating test, write down what *each*
hypothesis predicts for it, not only the one being tested.

**The fix.** `rx_audio.c` now has a demodulator selector,
`rx_audio_set_demod()` (`RX_DEMOD_CW`/`USB`/`LSB`, `rx_audio.h`),
driven from `radio.c`'s `radio_set_mode()` - the one place every mode
change (rigctld `M`, Kenwood `MD`, the control panel) already funnels
through, so `rx_audio.c` itself still has no `radio.h` dependency.
`DIGITAL` demodulates as USB, matching the TX side's
`TX_PIPELINE_KEEP_UPPER`. For USB/LSB, sideband selection is a single
conjugation of the input before stage 1 (stage 1 always keeps positive
baseband, so the choice is just whether to flip the input into it),
governed by one named constant, `RX_IQ_SPECTRUM_INVERTED` (1), and
stage 2 takes the real part directly with no BFO offset. The
correction lives in `rx_audio.c`, not in `sound.c`'s mixer, on
purpose: fixing the inversion at the source would silently flip every
other I/Q consumer too, including SparkSDR over `hpsdr_p1.c`, which
works as-is. **CW is untouched:** it is never conjugated and still
mixes to `CW_PITCH_HZ`, and a 200-block run (narrow filter on, then
off; two tones plus noise) produces a bit-identical hash of both
`out[]` and `uac_out` against the previous build.

Re-measured, same inverted-hardware model, `DIGITAL`/USB, narrow
filter off:

| station vs. dial | audio Hz | level |
|---:|---:|---:|
| +100 | 100 | -0.2dB |
| +300 | 300 | -0.7dB |
| +800 | 800 | 0.0dB |
| +1500 | 1500 | 0.0dB |
| +2500 | 2500 | -1.7dB |
| +3000 | 3000 | -1.7dB |
| +3300 | 3300 | -19.5dB |
| -300 (below) | 300 | -19.5dB |
| -800 (below) | 800 | -75.1dB |
| -1500 (below) | 1500 | -41.2dB |

Every station above dial now lands at exactly `RF - dial` Hz of audio,
upright, flat within 2dB from 100 to 3000Hz - WSJT-X's whole FT8 window
- with the lower sideband 40-75dB down. `LSB` is the exact mirror. Full
rebuild clean under `-Wall -Wextra`; `test-rx-audio`,
`test-fft-filter`, `test-rx-filter`, `test-upsample48k` and
`test-tx-pipeline` all pass unchanged.

**Side effects on `USB`/`LSB` voice.** Both modes received through the
same CW demod before this - voice 700Hz too high, and the same
sideband regardless of which mode was selected. They now get real SSB
demodulation. Nobody has listened to it yet.

**What's still open.**

- **On-air confirmation - first result in (2026-09-21).** After
  deploying this, the operator reports WSJT-X on the gadget's audio
  "now seeing many more signals." Still worth recording: the full
  simultaneous SparkSDR A/B, after
  deploying this, is the test: WSJT-X on audio should now decode close
  to what SparkSDR does, report the same audio frequency for each
  station SparkSDR shows `f` Hz above dial, and report SNRs within a
  few dB of SparkSDR's.
- **`RX_IQ_SPECTRUM_INVERTED` = 1 - confirmed on air (2026-09-21).**
  In `DIGITAL`, stepping the dial +100Hz moved signals in the audio
  WSJT-X waterfall to the *left*, which is the correct USB behavior.
  That's a direct measurement, not just the model: the constant is
  right for this board.
- **Stage 3 in USB/LSB.** The narrow filter is still centered on
  `CW_PITCH_HZ` in audio terms. In USB/LSB/`DIGITAL` it would pass only
  a ~300Hz window around 700Hz of audio, not a CW signal at dial center.
  Leave it off for those modes, as before. Making it mode-aware was
  declined for now.
- **TX frequency agreement.** With RX now honoring `audio = RF - dial`,
  WSJT-X's TX tone at audio `f` needs to land at `dial + f` on air.
  That's `tx_pipeline.c`'s job, unchanged here. Worth checking in the
  FT8 TX test the operator has deliberately deferred.
- **CW's own sideband.** CW still keeps positive baseband, which (given
  the inversion) means a station *below* dial is heard at a higher
  pitch - effectively CW-reverse. It's unchanged here, since CW wasn't
  part of this problem and on-air CW copy is already confirmed. Worth a
  deliberate decision some day, not a silent change.
