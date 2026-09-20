# Stage 3: the FFT narrow filter vs. the elliptic, measured

Status: measurement only - no code changed. This note exists to record
what was actually measured after an on-air report that the two stage-3
implementations "sound very different," and specifically to record that
the reported symptom was **not reproduced** by bench measurement. The
FFT filter's frequency response is correct, and better than the
elliptic's by every steady-state measure taken here. What the bench does
show is a real, large time-domain difference confined to keying
transitions. No design decision is recorded here yet - see §9/§11 for
the options and what would have to be decided to pick one.

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
