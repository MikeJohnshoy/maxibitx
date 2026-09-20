# RX Audio Bandwidth Reaching WSJT-X (`uac_out`) vs. a Raw-I/Q Path

Status: measurement only, no code changed - the reported symptom's
leading cause is identified and bench-quantified, but not yet on-air
confirmed, and no fix (operator-side or code) has been applied or
requested yet.

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

## 7. What's not decided yet

- Whether to fix this operator-side only (document "turn stage 3 off
  before running WSJT-X" in `10_external_digital_modes_wsjtx.md`, same
  as any other digital-mode setup step) or in code - e.g. having the
  `uac_out` tap bypass stage 3 unconditionally, regardless of the local
  `narrow_filter_enabled` setting used for CW listening, treating a
  remote/digital-mode consumer as categorically different from a human
  on headphones. That would directly revisit the reasoning quoted in
  §1 from `ARCHITECTURE.md` §10 step 9.
- Whether stage 1/2's own passband asymmetry (§4/§5) is worth
  addressing at all once the (much larger) stage-3 effect is ruled out
  or fixed - and if so, whether that means widening/recentering stage
  1's design specifically for the `uac_out` consumer, or leaving CW
  listening's own filter alone and giving `DIGITAL` mode a separate,
  wider stage-1 design point entirely.
- An on-air re-test of the SparkSDR-vs-usb_gadget comparison with the
  narrow filter confirmed OFF, to see how much of the reported 10x gap
  actually closes - not yet done.
