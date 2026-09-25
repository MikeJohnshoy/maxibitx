# Stage 3: the FFT narrow filter vs. the elliptic, measured

Status: §2-§10 are measurement only, written before any code changed.
They record what was actually measured after an on-air report that the
two stage-3 implementations "sound very different," and specifically that
the reported symptom was **not reproduced** by bench measurement: the FFT
filter's frequency response is correct, and better than the elliptic's by
every steady-state measure taken here. What the bench does show is a
real, large time-domain difference confined to keying transitions.

§11 and §12 are the parts that changed code, and they are two different
answers to the same problem. §11 fixes the FFT filter's time domain with a
minimum-phase realization that costs none of its magnitude response. §12
sidesteps it, by giving the elliptic filter the pitch and width control
that the FFT path existed to provide - twelve pre-designed sets, switched
at runtime, keeping the elliptic's attack. Both are in the tree. Read §4
for the problem, §11 and §12 for the two answers.

## 1. Why this note exists

`rx_audio.c`'s stage 3 has two implementations, selectable at runtime
via `rx_audio_set_narrow_filter_impl()` (`u`/`U FFTFILT` over rigctld):
the fixed 8-pole elliptic IIR designed in
[`rx_audio_demod_design.md`](rx_audio_demod_design.md) §8.2, and the
shared FFT overlap-save filter (`rx_filter.c`, `ARCHITECTURE.md` §10
steps 6/7). Both run every block regardless of which is selected, so
switching doesn't thump.

The on-air report (2026-09): the elliptic "performs as expected," while
the FFT filter "lets a lot of wideband hiss and far-off-center signals
get through." That is a specific, falsifiable claim about the FFT
filter's selectivity, and the first thing to do with it is measure it
rather than reason about it.

Everything below was measured against the **real compiled
`rx_filter.c`/`fft_filter.c`**, with the elliptic's
`narrow_filter_coeffs[]` copied verbatim out of `rx_audio.c`, both at
the shipped design point (700Hz pitch, 300Hz width, Fs=96kHz).

## 2. Frequency response: the FFT filter is not leakier - it is much tighter

Swept tones, each filter's output normalized to its own 700Hz passband
response, so this is selectivity only and not a gain comparison:

| tone | FFT | elliptic |
|---|---|---|
| 700 Hz | 0.00 dB | 0.00 dB |
| 800 Hz | -0.67 dB | -0.07 dB |
| 900 Hz | -18.02 dB | -12.19 dB |
| 1000 Hz | -66.27 dB | -34.04 dB |
| 1200 Hz | -71.41 dB | -49.78 dB |
| 1500 Hz | -76.03 dB | -55.79 dB |
| 2000 Hz | -80.19 dB | -62.78 dB |
| 3000 Hz | -84.71 dB | -52.93 dB |
| 5000 Hz | -89.61 dB | -50.58 dB |
| 8000 Hz | -93.80 dB | -49.93 dB |
| 12000 Hz | -97.22 dB | -49.72 dB |
| 20000 Hz | -101.31 dB | -49.62 dB |
| 30000 Hz | -103.99 dB | -49.58 dB |
| 47000 Hz | -105.60 dB | -49.57 dB |

Worst-case stopband anywhere above 2kHz: **FFT -80.2dB, elliptic
-49.6dB.**

The elliptic's equiripple stopband floor is real and it is flat: past
about 5kHz it sits at -49.6dB and stays there all the way to Nyquist,
which is exactly what an equiripple design does - it trades a guaranteed
floor for never getting any better than that floor. The FFT filter keeps
descending, reaching -105dB by 47kHz. The FFT filter is also *narrower*
at the top: -3dB at 134Hz off pitch versus the elliptic's ~160Hz.

So the reported symptom - far-off-center signals getting through the FFT
filter - is not what the filter does. It rejects them 30-55dB **better**
than the elliptic does.

## 3. Broadband noise

Stationary white noise through both, same input:

```
input noise RMS            0.577469
FFT filter output RMS      0.042505   (-22.7 dB vs input)
elliptic   output RMS      0.046795   (-21.8 dB vs input)
```

The FFT filter passes **0.8dB less** broadband noise - an equivalent
noise bandwidth ratio of 0.83x. Consistent with §2: it is the narrower
filter with the deeper stopband, so it must pass less noise, and it
does.

## 4. Time domain: this is where they genuinely differ

Impulse response, measured by feeding a single unit impulse and tracking
the output envelope (threshold: 1% of peak, i.e. -40dB):

| | FFT | elliptic |
|---|---|---|
| group delay | 16.0 ms | ~2.6 ms |
| ringing *after* the peak | 8.6 ms | 27.3 ms |
| ringing *before* the peak | **8.5 ms** | **none** |

Two things worth noting, one of them counterintuitive.

The counterintuitive one: the elliptic actually rings **longer** after
the fact - 27.3ms against the FFT's 8.6ms. Total ring energy is not
where the FFT filter loses.

The one that matters: the FFT filter is a linear-phase FIR, so its
impulse response is symmetric, and roughly 8.5ms of it arrives
**before** the transient that caused it. The elliptic is a causal IIR
and has exactly zero pre-ringing, by construction. Pre-ringing is
perceptually distinctive in a way that post-ringing is not - the ear
expects a sound to decay, not to anticipate itself.

The group delay difference (16.0ms vs 2.6ms) matters for a second
reason, in §7.

## 5. Keyed CW: where the penalty actually lands

Scenario: 20 WPM keyed CW (60ms elements) at 700Hz, plus a noise floor,
plus a strong interferer 5kHz off-frequency at 33x the wanted signal's
amplitude - i.e. deep in both filters' stopbands. Raw-input amplitudes
chosen at the scale `rx_audio.c` actually sees. The full `rx_audio.c`
AGC (`AGC_TARGET_AMPLITUDE`, 5ms attack, 300ms release, `AGC_MAX_GAIN`)
is applied exactly as the shipped code does it, from the raw undelayed
input.

Measuring **only samples well inside a key-up or key-down state** (a
guard band excludes everything within ~20ms of a transition):

```
gap energy   FFT 8.79e+04   elliptic 4.22e+05   -> FFT -13.6 dB
signal/gap   FFT 33.7 dB    elliptic 19.6 dB    -> FFT +14.1 dB
```

The FFT filter is **13.6dB quieter between elements** and gives **14.1dB
better signal-to-gap ratio**. Repeating the same run with each filter's
own group delay removed changes this by 0.2dB, so it is not a
delay-alignment effect.

The penalty is therefore not in the steady state at all. It is confined
entirely to the transition regions - the ~25ms around every key edge
where the FFT filter's 16ms of delay and 8.5ms of pre-ringing smear the
element boundary. At 20 WPM those regions are a meaningful fraction of
the total airtime, so the keying is genuinely softened; but the noise
floor between elements is lower, not higher.

## 6. A measurement error worth recording

The first version of the keyed-CW test above had **no guard band** and
scored every sample by the *input's* key state. It reported the FFT
filter putting +4.5dB *more* energy into the gaps and coming out 5.3dB
worse on signal-to-gap ratio - the opposite of §5's result, and
apparently a clean confirmation of the on-air report.

That number was an artifact of the harness. With 60ms elements and a
13.4ms group-delay difference, roughly a fifth of each nominal "gap"
window contained the delayed tail of the previous element for the FFT
path and did not for the elliptic. The test was measuring its own
windowing misalignment, not the filter.

Recorded here deliberately, because the wrong number was briefly
believed and reported before the guard-band version was run: a
transition-region artifact and a real selectivity difference look
identical if the measurement window straddles the transitions, and this
specific mistake is easy to repeat with any block-based filter that has
a large group delay.

## 7. The AGC tap point, and why an off-frequency signal is audible anyway

Independent of which stage-3 filter is selected: `rx_audio_process()`
computes its AGC envelope from `sqrt(i*i + q*q)` on the **raw input** -
before stage 1, before stage 3. That was a deliberate fix
(`rx_audio_demod_design.md` §8.8) for a real problem, and the reasoning
behind it still holds.

The consequence, which that note's §8.8 already anticipates as "AGC
desense from an unrelated signal," is that a strong station several kHz
away ducks the receiver's gain **even though stage 3 removes it
completely**. The operator hears their wanted signal and the noise floor
move in sympathy with a station they cannot hear. That is a plausible
mechanism for the "far-off-center signals get through" half of the
on-air report, and it is not a filter defect - §2 shows both filters
reject those signals, the FFT one far harder.

It applies to both implementations equally. What is *not* equal is that
with the FFT filter selected, the ducking is computed from an undelayed
envelope and applied to audio delayed by 16ms - a **13.4ms mismatch**
against the elliptic's 2.6ms. Whether that decorrelation is enough to
turn a fused, unnoticed gain movement into a separately audible artifact
is a hypothesis this note does not test, and it would need a listening
test rather than a bench one.

## 8. What this note does not explain

The "wideband hiss" half of the on-air report is **not reproduced**.
Every steady-state measurement taken here says the FFT filter delivers a
lower noise floor than the elliptic: 0.8dB less on stationary white
noise (§3), 13.6dB less between CW elements under realistic conditions
(§5), and a stopband 30-55dB deeper across the band (§2).

Ruled out as causes, each by direct measurement:

- **Filter selectivity.** §2. The FFT filter is narrower at -3dB and
  deeper everywhere past 900Hz.
- **Broadband noise leakage through the stopband.** §3.
- **Overlap-save history corruption from dropped blocks.** Simulating a
  dropped ALSA period once every 50 blocks changed a steady 700Hz tone's
  output RMS from 0.7074 to 0.7063 - the overlap-save engine re-converges
  within one block, so an xrun flood is not a candidate.
- **Block-size fallback.** `RX_FILTER_BLOCK_LEN` is 1024 and `sound.c`'s
  `PERIOD_FRAMES` is 1024, so the FFT path genuinely runs in the live
  radio rather than silently falling back to the elliptic.
- **Passband gain difference.** Both land within 5% of each other on a
  tuned tone (`test-rx-audio` Case B, ratio 1.05).

What remains untested, and is the most plausible surviving explanation
for a *perceived* noise increase: the 8.5ms of pre-ringing (§4) acting
on **impulsive** band noise - static crashes, QRN, key clicks from other
stations - rather than on the stationary noise the bench fed it. A long
symmetric FIR smears an impulse into a ~17ms two-sided wash; a train of
such impulses could plausibly be heard as continuous hiss where the
elliptic renders them as discrete clicks. Testing that needs an
impulsive-noise source in the harness, or a recording off the antenna,
neither of which exists yet.

## 9. If shortening the impulse turns out to be the answer

`RX_FILTER_IMPULSE_LEN` is **not a chosen number**. It falls out of
`RX_FILTER_N - RX_FILTER_BLOCK_LEN + 1` = 4096 - 1024 + 1 = 3073, i.e.
it is whatever the FFT size and the ALSA period size leave over. Nothing
about CW selectivity picked it.

Measured tradeoff at the same 700Hz/300Hz design point (pre-ring
measured as in §4; -60dB column blank where the skirt never reaches
-60dB within 600Hz of pitch):

| taps | N | -3dB | -60dB | group delay | pre-ring |
|---|---|---|---|---|---|
| 3073 (ships today) | 4096 | 134 Hz | 273 Hz | 16.0 ms | 8.5 ms |
| 2049 | 3072 | 128 Hz | 327 Hz | 10.7 ms | 5.6 ms |
| 1025 | 2048 | 138 Hz | 508 Hz | 5.3 ms | 2.7 ms |
| 513 | 1536 | 203 Hz | — | 2.7 ms | 2.4 ms |
| 257 | 1280 | 418 Hz | — | 1.3 ms | 0.6 ms |
| *elliptic, for reference* | | *~160 Hz* | *~275 Hz* | *2.6 ms* | *none* |

The -3dB width barely moves between 3073 and 1025 taps (134Hz to
138Hz) while group delay falls by two thirds and pre-ringing by
two thirds. What is actually being bought with those extra 2048 taps is
ultimate skirt steepness: -60dB at 273Hz instead of 508Hz. 2049 taps is
the closest match to the elliptic's skirt (327Hz vs 275Hz) at a third
less delay.

The deeper point: this is a 300Hz filter running at 96kHz - a 0.3%
fractional bandwidth, which is *why* it needs thousands of taps. A
receiver that does narrow CW filtering at 8-12kHz needs roughly
`3073 / 8` taps for the same shape, with proportionally less delay and
pre-ringing. `decim48k.c` already exists and already does the 96k->48k
half of that. Decimating stage 3 is the architecturally correct fix and
the one that makes the tap count stop being an accident; shortening the
impulse is the cheap one that can be tried in an afternoon.

## 10. What's not decided

This section was written before §11. Where §11 settles something, it says
so; the rest still stands.

- **Nothing here has been acted on.** No code changed. The FFT
  implementation remains available and the elliptic remains the default
  (`narrow_filter_impl = RX_NARROW_FILTER_ELLIPTIC`), which given §5's
  transition-region finding is a defensible default for CW regardless of
  how the rest of this resolves.
- **The "wideband hiss" report is still unexplained** (§8). The
  impulsive-noise hypothesis is the one worth testing next, and it needs
  a harness that feeds impulsive rather than stationary noise.
- **Whether the 13.4ms AGC/audio delay mismatch is audible** (§7) is
  untested. Delaying the AGC's control signal to match the selected
  filter's group delay is small and self-contained, and would settle it.
- **Which of §9's directions to take**, if any - shorten the impulse,
  decimate stage 3, or leave the FFT path as the sharp-but-laggy option
  and keep the elliptic as the CW default.
- **Whether any of this matters for SSB or digital modes**, where stage
  3's 300Hz width is wrong anyway and the filter is normally bypassed.
  Every measurement here is a CW-selectivity measurement.

## 11. Minimum phase: the same magnitude without the delay

§9's three options all pay for the time domain with something: taps,
selectivity, or a decimation stage that doesn't exist yet. There is a
fourth that pays with none of them, and it is what the tree now does.

### Why it works

§4's group delay is not a property of the filter's selectivity. It is a
property of how `filter_tune_ex()` realizes it. That function designs a
brick-wall response, inverse-transforms it, and centres the resulting
impulse at tap M/2 - a symmetric impulse, which is linear phase, which
means every frequency is delayed by exactly (M-1)/2 = 1536 samples =
16.0ms. The symmetry is also why half the impulse arrives before its own
peak.

A given magnitude response does not require that realization. Reflecting
every zero that lies outside the unit circle to its mirror image inside
changes the phase and leaves |H| untouched - the reflection is the one
rearrangement with that property - and the result is the *minimum*-phase
filter with that magnitude: the one that concentrates its energy as early
as possible. Same passband, same skirts, same stopband; a decaying
impulse starting at tap 0 instead of a symmetric one centred at tap 1536.

`filter_min_phase()` (`fft_filter.c`) does it by cepstral factorization
rather than by finding roots: take the floored log of |H|, inverse-
transform to the real cepstrum, zero the anticausal half and double the
causal half, transform back, exponentiate. The fold only touches the odd
(phase) part of the log-spectrum, which is why the magnitude survives it.

### The one thing that can go wrong, and what it cost to fix

A windowed FIR is zero past tap M-1 by construction. A minimum-phase
impulse decays rather than ending, and overlap-save's discard region is
exactly M-1 samples - so a tail reaching past that point does not get
discarded, it wraps into the output. Measured: the tail carried -58dB of
the impulse's energy. Inaudible in level, but it lands as an artifact
repeating at the block rate rather than as a smooth error in the
response, which is the worse of the two failure modes.

Truncating at M cures the wrap and costs stopband depth instead, because
a hard cut leaks. Fading the tail out first with a raised cosine costs
almost nothing. Measured against the linear-phase filter it is supposed
to match, worst-case deviation anywhere in the swept range:

| fade length | worst deviation | where |
|---|---|---|
| no fade, hard cut | 5.21 dB | +1400 Hz |
| last 128 taps | 4.13 dB | +300 Hz |
| last 256 taps | 2.13 dB | +400 Hz |
| last 1024 taps (M/3, ships) | **0.49 dB** | +300 Hz |
| last 2048 taps | 2.24 dB | +300 Hz |

`MIN_PHASE_FADE_DIVISOR` is 3 for that reason. The remaining 0.49dB sits
at a stopband null 300Hz off pitch, where -66.3dB became -66.8dB; in the
passband the two filters agree within 0.06dB. Energy past tap M-1 after
the fade is at the float noise floor, -142dB, the same as the windowed
FIR's.

`MIN_PHASE_FLOOR_DB` is -100dB. The log needs a floor because true zeros
occur, and the floor sets how fast the cepstrum decays; sweeping it showed
everything below -80dB produces an identical filter, because the response's
own stopband never goes deeper than about -86dB. -100dB is inside the
region where the choice stops mattering.

### What it measures

All three filters at the shipped design point, from `rx_filter_test.c`
Case E and the same bench harness as §2-§5:

| | linear phase | minimum phase | elliptic |
|---|---|---|---|
| -3dB off pitch | 134 Hz | 134 Hz | ~160 Hz |
| stopband past 2kHz | -83.6 dB | -83.6 dB | -49.6 dB |
| white noise, equal gain at pitch | -1.41 dB | -1.47 dB | reference |
| group delay | 16.0 ms | **4.5 ms** | 3.6 ms * |
| keyed element, key-down to -3dB of settled | 20.3 ms | **8.7 ms** | 7.2 ms |
| impulse energy past tap M-1 | -142.5 dB | -142.0 dB | n/a |
| gain at pitch | 0.00 dB | +0.04 dB | reference |

\* The elliptic's group delay reads 3.6ms here, against the ~2.6ms
`rx_audio.c`'s own header quotes and §4 above repeats. Both are that
filter: this column is the impulse response's peak, measured the same way
for all three so the comparison is apples to apples, while 2.6ms is the
design-time figure at the passband centre. The 1ms is the metric, not a
correction to either number.

The keyed-element column is the one worth reading twice. At +10ms after
key-down the linear-phase filter's envelope is 56.7dB below its settled
level - the element has barely started - while the minimum-phase one is
within 0.8dB and the elliptic within 0.7dB. That gap is what the ear was
reporting, and it closes without giving up a single dB of the
selectivity §2 measured.

### What it costs

Group delay is no longer constant across the passband; that is what
"minimum phase" means. For a ~300Hz filter carrying a single CW tone this
is close to free - there is one frequency of interest in there - and for
SSB it would not be, which is part of why the switch exists. The other
part is that every step 6 bench number in `ARCHITECTURE.md` §10 was
measured against the linear-phase realization, and losing the ability to
reproduce them would be a bad trade for a default.

### What it changes elsewhere in this note

- **§4's pre-ringing is gone.** A minimum-phase filter is causal and
  front-loaded; nothing arrives before the transient that caused it. §8's
  surviving hypothesis for the "wideband hiss" report - 8.5ms of symmetric
  pre-ring smearing impulsive band noise into a continuous wash - has no
  mechanism left in this realization. That makes §11 the cheapest
  available test of §8's open question: if the hiss goes away with
  `U MINPHASE 1`, the hypothesis was right.
- **§7's AGC/audio delay mismatch shrinks from 13.4ms to 1.9ms.** The
  envelope is still computed from the undelayed input, but the audio it
  now controls is 4.5ms late rather than 16.0ms late - close enough to the
  elliptic's 2.6ms that the mismatch is no longer a meaningful difference
  between the two implementations.
- **§9's options are not foreclosed.** Decimating stage 3 remains the
  architecturally right answer to a 0.3% fractional bandwidth, and
  shortening the impulse remains cheap. Minimum phase is orthogonal to
  both: it would apply to any tap count, and a shorter impulse would still
  benefit from it.

### How to use it

`rx_audio_set_narrow_filter_min_phase()`, reachable over rigctld as
`u MINPHASE` / `U MINPHASE <0|1>` alongside the existing
`U FFTFILT`. On by default, so selecting the FFT implementation gets the
realization that sounds right; `U MINPHASE 0` returns the linear-phase one
for an A/B. The switch re-designs the response, so it runs on the
interface thread, not the audio thread, and one block can straddle the
change - an audible edge when the operator flips a switch, which is the
same non-atomic retune `rx_filter_retune()` has always had.

Still not decided by any of this: whether the FFT implementation should
become the *default* stage 3. That needs on-air listening against the
elliptic, not more bench time.

## 12. The other answer: a bank of twelve elliptic filters

§11 made the FFT filter good enough to use. This section is about not
needing it to be, because the question that started all of this was never
"which filter measures better" - it was "can the operator change pitch and
width." The FFT filter answers that with continuous control at a
time-domain cost. Twelve pre-designed elliptic filters answer it with
quantized control at no cost at all.

Elliptic coefficients can't be designed at runtime, which is why stage 3
originally carried exactly one filter. But nothing requires it to carry
only one. Six pitches (500 to 1000 Hz in 100 Hz steps) times four widths
(150/300/450/600 Hz) is 24 coefficient sets, 3.8 KB of rodata, one of which
is active at a time - so the CPU cost is identical to the single fixed
filter it replaces, and strictly lower than running the FFT filter's
4096-point transform every block. Adding rungs costs only rodata, which is
why the range is set by what an operator would use rather than by what the
arithmetic can afford.

### Where the design came from

The shipped filter's design call survived only as a comment, so the first
step was recovering it. Matching coefficients against
`scipy.signal.ellip()` output identified it exactly - 4th order, 0.5dB
ripple, 50dB stopband, band edges at `pitch ± width/2`, `fs=96000`,
reproducing all twenty of the shipped coefficients to `0.0e+00`. The bank's
700Hz/300Hz entry is therefore bit-for-bit the filter the radio already
ran, and `tools/gen_narrow_filters.py` asserts that on every run rather
than trusting it.

That script is the design: it generates all twelve sets, measures each one,
refuses to emit anything if any set fails a check, and writes
`src/narrow_filter_bank.h`. Twelve sets is 240 coefficients, well past
where a transcription error would be caught by eye - and an elliptic
section's poles sit close enough to the unit circle that a wrong digit
means an unstable filter rather than a slightly wrong one. `make
check-filters` verifies the header is current.

### What the twelve measure

| width | pole radius | −3 dB | at +150 / +300 Hz | attack | FFT min-phase attack |
|---|---|---|---|---|---|
| 150 Hz | 0.99933 | 162 Hz | −38.5 / −52.2 dB | 8.8 ms | 11.6 ms |
| 300 Hz | 0.99883 | 324 Hz | −0.5 / −34.5 dB | 5.2 ms | 8.8 ms |
| 450 Hz | 0.99849 | 486 Hz | −0.5 / −11.5 dB | 3.9 ms | — |
| 600 Hz | 0.99831 | 648 Hz | −0.4 / −0.5 dB | 3.3 ms | 6.7 ms |

Figures at 700 Hz; the 600 and 800 Hz rows differ by hundredths. Every set
is numerically comfortable - worst pole radius 0.99933, and after 500 ms of
silence following excitation the residual is 1e-14 or smaller, so no limit
cycles and no creeping instability. The ladder could extend to 100 Hz
(radius 0.99954, 12.4 ms attack, stable even with float32 coefficients);
50 Hz is where it stops being worth it, at 23 ms attack and audible
ringing half a second later.

The elliptic beats the minimum-phase FFT filter on attack at every width,
which is the whole point. What it gives up is the floor: an equiripple
stopband sits at exactly −50 dB from about 3 kHz to Nyquist and never
improves, where the FFT filter keeps descending to −105 dB. With a 300 Hz
filter and an interferer 300 Hz off, the FFT path is 32 dB better. So the
two implementations now divide honestly: the bank is the everyday filter,
and the FFT path is for digging a weak signal out from under a close strong
one.

### Switching between them is free

This was the expected obstacle. Swapping twenty coefficients under a
steady tone, keeping the biquad history:

```
300 -> 150 Hz   +1.99 dB overshoot, settles in  9.8 ms
300 -> 600 Hz   +0.90 dB overshoot, settles in  1.7 ms
150 -> 600 Hz   +1.28 dB overshoot, settles in  3.4 ms
```

Two dB and ten milliseconds, so no crossfade, no double-running, no ramp -
just overwrite the coefficients. Zeroing the history instead measures the
same within a dB and settles *slower* when widening, besides throwing away
a filter's worth of signal every time the operator touches the control, so
the history is kept. Measured again through the whole pipeline
(`rx_audio_test.c` Case H, where the AGC is also in circuit) the worst
swap peaks 0.46 dB above the settled level.

Like `rx_filter_retune()`, the swap isn't atomic against the audio thread:
one block can be computed with a half-updated coefficient set. At 2 dB that
is an audible edge when the operator turns a knob, not a defect that
appears on its own.

### The pitch axis needs the BFO, and this is the part that isn't obvious

Selecting a different *filter* center does not change the pitch the
operator hears. The demodulated tone is placed by `rx_audio.c`'s software
BFO, and if only the filter moves, the tone stays put and the filter
detunes off it. The size of that mistake depends on the width: at 150 Hz a
700 Hz tone through the 600 Hz-centered filter is attenuated **38.5 dB**,
while at 600 Hz wide it is attenuated **0.4 dB**. A pitch control behaving
that way - "does nothing, or kills the audio, depending on the width knob"
- would be worse than no pitch control.

So `rx_audio_set_narrow_pitch()` moves both: the bank entry and
`vfo_start(&bfo, pitch, 0)`. `rx_audio_test.c` Case G is the regression
guard - a station on dial center stays equally loud at every pitch, which
is only true if the BFO followed.

Two things worth being precise about. This is the *software audio-frequency*
BFO, not `hw_settings.ini`'s `bfo_freq` (the ~22.6 kHz hardware
crystal-filter BFO that `tx_pipeline.c` derives its IF placement from) -
moving it does not affect where the radio transmits. And `vfo_start()`
quantizes to a 65536-entry phase table, so the rungs land at
599.5/698.7/799.8 Hz rather than exactly; against a 150 Hz passband that is
a fraction of a percent, and it is the same quantization the BFO has always
had.

### What this leaves open

- ~~**The TX sidetone doesn't follow the pitch.**~~ **Fixed - see §13.** It
  turned out to matter more than "cosmetic": a sidetone that disagrees with
  the received pitch makes zero-beating by ear transmit off frequency by the
  difference.
- **Whether these are the right rungs** is an on-air question. 150 and
  300 Hz are the CW widths; 450 and 600 are for comfortable listening. A
  100 Hz width rung is available if wanted. The pitch range stops at 500 Hz
  at the bottom for a measured reason - see §14.
- **Which implementation should be the default** is still undecided, and
  the bank makes the comparison fairer rather than settling it: both
  implementations now track the same pitch and width, so switching compares
  two filters aimed at the same passband instead of two different ones.
- **`M <mode> <passband>`'s passband argument is still cosmetic.** Snapping
  it to the nearest width rung would give stock Hamlib clients real filter
  control with no extensions, and would retire the `CWWIDTH` extension.

## 13. The sidetone has to move too, or you transmit off frequency

§12 shipped the pitch control with a gap it described as separable: `cw.c`
kept generating the sidetone at `CW_PITCH_HZ` while the receiver moved. On
air that gap is not cosmetic, and the reason is worth stating plainly.

The pitch is the beat note that means *he is on my dial*. A station exactly
on the dial is heard at the pitch; one *d* Hz above is heard at pitch + *d*;
and a key-down lands on the dial, because `tx_pipeline.c`'s CW rotation is
`(bfo_freq − xtal_filter_center) − CW_PITCH_HZ` — the tone and the shift
cancel. So tuning a station until it beats at the selected pitch puts you
exactly on his frequency.

Now leave the sidetone at 700 and select 600. The operator does the natural
thing and tunes until the received note matches the sidetone they know — 700
Hz. But heard = pitch + *d*, so 700 = 600 + *d* means the station is 100 Hz
**above** the dial, while the transmission goes out **on** the dial: 100 Hz
low. Inside a 500 Hz filter he still hears you, so nothing announces the
error; you just get answered less often than you should be.

The fix is to move all four things that encode the pitch, together:

| what | where | why it has to move |
|---|---|---|
| RX BFO | `rx_audio.c` | the tone you hear |
| narrow filter | the bank, §12 | centered on that tone |
| keyed tone | `cw.c` | the sidetone you zero-beat against |
| CW IF shift | `tx_pipeline.c` | so the carrier stays on the dial |

`radio_set_cw_pitch()` is the only function that does all four, and it is
what `L CWPITCH` now calls. `radio.c` is the right owner — it already
coordinates across modules for `radio_set_mode()` — and the alternative,
`rx_audio.c` reaching into `cw.c` and `sound.c`, would have put transmit
behavior behind a receive-side call.

Two details the implementation turns on. The request is snapped by the RX
side *first*, and that snapped value is what feeds `cw.c` — handing the
sidetone the unsnapped request is precisely how the two would silently
disagree again. And `sound_update_cw_if_placement()` reads `cw_get_pitch()`
rather than taking a pitch argument, so the shift cannot be derived from a
tone the radio isn't generating; the ordering (tone, then shift) follows from
that.

It refuses mid-transmission, returning the current pitch unchanged. The four
updates aren't atomic, and a key-down straddling them would transmit the old
tone against the new shift — briefly off frequency by the pitch change.
Waiting for key-up costs nothing, since nobody adjusts pitch while sending.

### The carrier moves by a few Hz, and always did

`tx_pipeline_test.c` Case H measures where the carrier actually lands at
each pitch, reading `cw_shift_bins` back from the pipeline rather than
recomputing what it ought to be:

| pitch | shift applied | carrier | off the dial | level |
|---|---|---|---|---|
| 500 Hz | 22078 Hz | 22578.12 Hz | −21.88 Hz | −0.41 dB |
| 600 Hz | 21984 Hz | 22584.38 Hz | −15.62 Hz | −0.04 dB |
| 700 Hz | 21891 Hz | 22590.62 Hz | −9.38 Hz | −0.00 dB |
| 800 Hz | 21797 Hz | 22596.88 Hz | −3.12 Hz | −0.00 dB |
| 900 Hz | 21703 Hz | 22603.12 Hz | +3.12 Hz | −0.00 dB |
| 1000 Hz | 21609 Hz | 22609.38 Hz | +9.38 Hz | −0.00 dB |

The ideal is 22600 Hz at every pitch. It can't be hit exactly, because the
rotation is a whole number of 46.875 Hz bins and each pitch rounds
differently — so the carrier sits within half a bin of the dial. Case H
bounds the individual offset at half a bin, and the *spread* between pitches
at a full bin, because two pitches can round in opposite directions; more
than that in either case would be a rounding bug rather than quantization.

**The 9.38 Hz at 700 Hz is not new.** `TX_IF_SHIFT_CW_BINS` has always
rounded 21900 Hz to 467 bins = 21890.625 Hz, so every CW transmission this
radio has ever made was about 9 Hz low of its dial reading. That is the
first hard number for ARCHITECTURE.md §10 step 5's still-open "where does
maxibitx actually transmit" question — nine Hz is far too small to have been
noticed on air, and far too large to be nothing if anyone ever measures it.

If that ever needs fixing, the clean answer is not a finer rotate but the
reverse of the current arithmetic: choose the *tone* so the shift lands on
an exact bin. At 700 Hz nominal, 467 bins wants a tone of
22600 − 21890.625 = 709.375 Hz, which is inaudibly different from 700 and
puts the carrier exactly on the dial. The filter bank wouldn't care — a 9 Hz
offset inside a 150 Hz passband is nothing — and `vfo_start()`'s own 1.46 Hz
quantization would then be the only residual. Not done, not needed at CW
accuracies, but it is the shape of the fix.

## 14. Extending the bank to 500-1000 Hz, and what that cost

Three pitches was a starting point, not a design: most rigs offer a CW
pitch range on the order of 400-1000 Hz. The bank now runs 500 to 1000 Hz
in 100 Hz steps against the same four widths — 24 sets, 3.8 KB of rodata,
still one filter active at a time, so still no CPU cost over the single
fixed filter this stage started with.

Extending it was editing two lists in `tools/gen_narrow_filters.py` and
rerunning it, which is the whole reason the generator exists. All 24 sets
pass the same checks, and they are strikingly uniform: every width lands on
the same measured −3 dB figure at every pitch (162/324/486/648 Hz), the
stopband floor is −50.0 to −50.2 dB throughout, attack runs 3.2 to 9.2 ms,
and the worst pole radius is 0.99937 at 500 Hz / 150 Hz — no worse than the
0.99933 the original three already carried. Nothing about the wider range
stresses the arithmetic.

### Two things the extension taught

**A bound in the test was wrong.** Case H asserted that the carrier's
*spread* across pitches stayed inside half a bin. That passed while the bank
held 600/700/800 Hz, whose roundings happen to cluster within 12.5 Hz. At
500 to 1000 Hz the spread is 31.25 Hz and the assertion failed — correctly
reporting that the bound, not the code, was wrong: each pitch rounds
independently to its own nearest bin, one possibly up while another rounds
down, so two pitches can legitimately sit a full bin apart. The individual
offset bound of half a bin was right and is unchanged. Worth recording
because it is the good case: a test that failed on being asked a harder
question, rather than a test that silently kept passing.

**500 Hz is the floor, and it is the TX pipeline's floor, not the bank's.**
Case H's level column shows the CW carrier at −0.41 dB at 500 Hz against
−0.00 dB from 700 Hz up. That is not the narrow filter — it is
`tx_pipeline.c`'s own 300-3000 Hz passband, whose Kaiser transition is
roughly 340 Hz wide at M=1025, so a 500 Hz tone still sits on the lower
skirt. Four tenths of a dB of transmit power is nothing, but the roll-off
steepens below 500, and a 400 Hz pitch would be measurably down. Extending
lower would therefore mean widening the TX passband's lower edge, not adding
bank entries — a separate change with its own consequences for what else
that edge is keeping out.

Also worth noting for anyone reading the carrier table: the extreme pitches
have the largest dial offsets (−21.9 Hz at 500 Hz), since the quantization
residual grows with distance from wherever the bin grid happens to align.
Still far inside CW practice, and still the same argument for the
tone-chosen-to-land-on-a-bin fix in §13 if it ever matters.
