# CW noise reduction: a design study

Status: **proposed — a study, nothing built.** §14–§16 are a
recommendation, an order of work and the decisions wanted. No code until
those are taken.

Written to answer, before any code exists, the questions asked about noise
reduction (NR) for the local CW audio:

- Do voice and CW need separate NR algorithms? CW comes first.
- How much benefit can be expected on CW? The sbitx's NR "did not seem very
  good".
- Is NR hard to place in maxibitx's own demodulator? Should it go after
  the narrow filter, after the AGC, or both?

Every number here is measured by the tools in
[`tools/nr_study/`](../../tools/nr_study/), derived from the code with the
derivation shown, or cited. Predictions are labelled as predictions. The
bench is a model, not the radio: §5 says what it leaves out.

## 1. The answer in brief

**Expect comfort, not copy.** NR makes the gaps between elements quieter.
It does not make a weak signal readable that wasn't already. At −9 dB SNR
(in 2500 Hz, the WSJT-X convention), the best NR variants make the gaps
12–13 dB quieter, at a cost of about 5 dB of the signal itself. Not one
spectral NR variant decoded more dit slots than the filter alone. Below
about −12 dB, the spectral variants remove the signal almost as fast as
the noise.

**The copy tool is the narrow filter, and it is already built.** Going
from 300 Hz to 150 Hz is the largest copy improvement measured. At −9 dB
it cuts slot errors from 7.8% to 3.2% at 20 WPM, and from 13.2% to 7.0% at
35 WPM. No NR variant matched that at 35 WPM. The one NR that matched it
at 20 WPM, the line enhancer, did so by acting as an adaptive narrow
filter (§6).

**The best comfort comes from the two together.** The 150 Hz filter plus a
CW-tuned spectral NR gives 11 dB of mark-to-gap contrast at −9 dB. Stage 3
alone gives 3 dB. Copy stays within a point of the 150 Hz filter's at 20 WPM.

**Voice and CW use the same algorithm with different constants, not
different algorithms.** Voice settings smooth the gain slowly and cut
deep. On CW they cost copy: 11.3% errors at −9 dB against 7.6% for CW
settings, and 21% against 17% at 35 WPM. They also soften every element.
One implementation can take a CW and a voice parameter set. CW comes
first.

**Placement is not hard, but NR can't go inside stage 3's FFT.** Per-bin
gains applied inside the overlap-save frames break overlap-save: 4–5% of
each frame's response wraps around (−13 dB time aliasing, measured). It
would also work only with the FFT filter. NR belongs in its own small
stage:

- **Where:** between stage 3 and the AGC's gain multiply, at 6 kHz.
- **"After the filter":** yes.
- **"After the AGC":** not quite. The AGC's envelope is already measured
  from the raw I/Q and doesn't change. The gain is applied after NR, and
  delayed by NR's latency.
- **Leaves unchanged:** the USB audio feed (`uac_out`), the S-meter and
  DIGITAL mode.
- **Stage 3 out:** NR is held off too, which keeps the 6 kHz stage cheap
  (§9).

**Cost when on:** about 23 ms of extra receive latency for the spectral
NR, and about 4% of stage 1's arithmetic. **Cost when off:** nothing.

**The sbitx's NR, measured:** each mode cuts the signal nearly as much as
the noise.

- **DSP mode:** gaps 6.5 dB quieter, signal 4.7 dB quieter, against the
  sbitx's own filter at −9 dB. Errors rise from 5.5% to 9.0%.
- **ANR mode:** gaps 5.8 dB quieter, signal 5.3 dB quieter, so it mostly
  turns the volume down. It also amplitude-modulates every 10.7 ms block
  by 2.3 dB through a coding slip (§8).
- **Why:** the noise estimate is updated twice a second and includes the
  signal, and the gains are applied inside the filter's FFT. That fits the
  report.

**Recommendation (§14):** don't build anything until you have listened to
the WAVs. If they are worth having, build a CW-tuned spectral NR (MMSE-LSA
gain, minimum-statistics noise, 128-point frames at 6 kHz) as its own
stage, off by default, used with the 150 Hz filter for weak signals.

The LMS line enhancer measured better on single-signal copy at 20 WPM,
with half the latency. It is not recommended first for two reasons:

- It costs a weaker second station in the passband real copy (0.8% → 5.0%
  errors, §7). That is the ordinary state of a CW band.
- Its settings were found on this bench.

## 2. What maxibitx has

`rx_audio_process()` (`src/rx_audio.c`) handles one 1024-sample block
(10.7 ms) at 96 kHz:

1. **Stage 1:** a 327-tap complex bandpass that keeps 0 to +3 kHz of
   baseband and selects the sideband.
2. **Stage 2:** the demodulator. CW and CWR mix up to the pitch with the
   software BFO.
3. **Stage 3:** the narrow filter. It is either the FFT filter
   (`rx_filter.c`) or the elliptic bank (`narrow_filter_bank.h`), and both
   run every block.
   - **FFT filter:** overlap-save, N = 4096, L = 1024, M = 3073 taps, with
     23.4 Hz bins. It is minimum phase by default, with 4.5 ms group delay
     (`rx_narrow_filter_fft_vs_elliptic.md` §11).
   - **Elliptic bank:** 24 eight-pole filters, six pitches by four widths:
     150, 300, 450 and 600 Hz.
   - **When it is out:** stage 3 is bypassed when the operator turns it off,
     and in DIGITAL mode (`rx_uac_out_digital_mode_bandwidth.md` §11).
4. **Stage 4:** the AGC gain, then the volume.
   - **The envelope** comes from the **raw input I/Q** magnitude: 5 ms
     attack, 300 ms release. No stage's selectivity can leak into the gain
     (`rx_audio_demod_design.md` §8.8).
   - **`uac_out`,** WSJT-X's whole audio feed over USB, is taken here:
     after stage 3 and after the gain, before the volume.
   - **The S-meter** reads stage 3's output.

Two consequences matter for NR:

- **The AGC gain leads the audio.** It is computed from the raw input but
  applied to audio that stage 3 has delayed by its group delay.
  `rx_narrow_filter_fft_vs_elliptic.md` §15 measured what that costs, an
  element's onset flattened by 1–1.5 dB. It also tested the fix, delaying
  the gain to match, in a scratch patch that was never committed. Any NR
  that adds delay makes this lead larger (§9).
- **Stage 3 has only part of a frame free.** Its overlap-save frames hold
  a 3073-tap filter in a 4096-point FFT. That leaves room for a gain that
  varies smoothly across frequency, but not for one that varies sharply
  bin by bin (§4).

## 3. What NR can and cannot do for CW

A CW signal is one tone, keyed. For copying it, the best linear receiver is
a filter as narrow as the keying allows. A 20 WPM dit lasts 60 ms, so its
spectrum is roughly 20 Hz wide. The 300 Hz filter is about fifteen times
wider than that, and each halving takes out 3 dB of noise and almost none
of the signal. The 150 Hz filter's measured cost is 1.3 dB of mark level
at −9 dB, against 3.5 dB less noise. Narrowing is therefore the first and
best tool, and it is why `narrow150` keeps winning below.

Spectral NR is a different kind of thing: a gain per frequency bin per
frame, set from each bin's estimated SNR. It can do two things:

- Turn down bins that hold only noise. This is an adaptive narrowing, the
  same effect as above, but chosen on the fly.
- Turn down whole frames that hold only noise, which gates the gaps.

What it cannot do is separate signal from noise inside the bin the signal
is in. Gating the gaps quiets the background, which is less tiring to
listen to. It does little for an ear that is already telling mark from
space by energy. At low SNR the gain decision itself becomes noisy:

- Marks get turned down with the gaps (the "mark dB" column below).
- Lone noise bins get through as short tones. This is "musical noise", the
  characteristic artifact of spectral NR.

An adaptive line enhancer (LMS) works another way. It predicts each sample
from samples 8 ms older. A steady tone is predictable and noise is not, so
the prediction keeps the tone. The predictor converges to a narrow filter
centred on whatever tone is there. That makes it an adaptive narrow filter,
which suits one signal and suits two less well (§7).

## 4. The candidates

| | where | how |
|---|---|---|
| **In-filter** | inside stage 3's 4096-point frames | per-bin gain on the filtered spectrum before the inverse FFT. Adds no latency. Breaks overlap-save (below). FFT filter only. |
| **Spectral, own stage** | after stage 3, decimated by 16 to 6 kHz | its own short-time FFT (sqrt-Hann, 75% overlap, weighted overlap-add), then interpolation back to 96 kHz. Works with either stage 3, or with none. |
| **LMS line enhancer** | after stage 3, at 6 kHz | 64-tap leaky NLMS predictor, decorrelation delay 48 samples (8 ms), μ = 0.01, leakage 0.1. |
| **sbitx DSP / ANR** | inside its own receive FFT | modelled line for line from v5.401 (§8). |

The spectral variants share one gain machine, `SpectralNR` in the bench:

- **Noise estimate:** minimum statistics (Martin). Each bin's smoothed
  power is tracked over a 1.5 s window in eight sub-windows. The minimum
  is scaled by a bias, found numerically: 2.61 in the filter's frames,
  2.59 in the short frames.
- **Gain:** the MMSE log-spectral-amplitude gain (Ephraim–Malah), with a
  decision-directed a-priori SNR and a gain floor.
- **Bins:** only those in the passband plus 150 Hz either side are
  processed. The rest get the floor. With stage 3 in, that changes
  nothing, because stage 3 has already taken them down by 60 dB or more.

The settings, chosen by the usual reasoning and then measured:

| setting | voice-style (`in_voice`) | CW (`in_cw`, `stft*`) |
|---|---|---|
| decision-directed α | 0.98 (smooth, few musical tones, slow) | 0.80 (follows a 60 ms dit) |
| gain floor | −20 dB | −15 dB |
| frame, hop | in-filter: 4096 at 96 kHz, 1024 (42.7 ms, 10.7 ms) | `stft_cw`: 256/64 at 6 kHz (42.7 ms, 10.7 ms, 23.4 Hz bins); `stft128`: 128/32 (21.3 ms, 5.3 ms, 46.9 Hz bins) |

**Why in-filter breaks overlap-save.** Overlap-save is exact only while
the response it applies fits in M = N − L + 1 taps. The filter already uses
all 3073 of them. A gain that differs bin by bin has a long impulse
response of its own. The filter times the gain then runs past M, and the
excess wraps around into the samples kept. The bench measures the energy
outside M taps, frame by frame:

| variant | median | 95th percentile |
|---|---|---|
| `in_voice` | −12.4 dB | −7.4 dB |
| `in_cw` | −13.4 dB | −9.0 dB |
| `in_cw_sm` (gains smoothed over 5 bins) | −30.2 dB | −24.4 dB |

In `in_cw`, one frame in twenty puts more than 12% of its response in the
wrong place, heard as smearing and a faint warble. Smoothing the gains
across frequency mostly fixes the aliasing, but a smoother gain is a less
selective one. It costs 7.8 dB of mark level at −9 dB and the most copy of
any CW variant (§6). On top of this, the in-filter route works only with
the FFT filter, and couples NR to `rx_filter.c`'s block size. It is ruled
out.

## 5. The bench

`tools/nr_study/cw_nr_bench.py` generates keyed CW and adds white noise:

- **Text:** "CQ POTA DE KB2ML KB2ML K", at 20 WPM (and 35), with 5 ms
  edges.
- **Pitch:** 700 Hz.
- **Length:** 24 s at 96 kHz.
- **Noise levels:** −21 to 0 dB SNR in 2500 Hz.

Stage 3 is modelled as `rx_filter.c` builds it, but linear phase: overlap-save,
N = 4096, L = 1024, a Kaiser β 5 bandpass of 3073 taps. Each variant then
runs as §4 describes. The offline resamplers are zero-phase, so live
latency is derived in §10, not measured here. Every result is the mean of
four noise draws (two at 35 WPM), and the range of the four is printed.

What it reports, per SNR:

- **errors:** dit-slot decisions, each slot's energy against the single
  best threshold for that output. It is a generous stand-in for copy (the
  threshold and the timing are known), applied the same way to every
  variant. Each run has about 400 slots, so a difference of a point or two
  is within the noise. The printed ranges show how much.
- **contrast:** mean mark-slot energy over mean gap-slot energy, in dB.
  This is how much quieter the gaps are than the elements.
- **gap dB / mark dB:** gap and mark energy against stage 3 alone. These
  are the noise removed and the signal lost.
- **onset:** at +10 dB, the time for an element to reach −3 dB of its
  settled level, ms.

What it leaves out: QRN (impulse noise), QSB, chirp and drift, a human ear,
and the AGC. White noise is the case spectral NR is best at, so these
results are, if anything, kind to it. Two signals are checked separately
(§7).

## 6. Results: one signal

Slot errors, 20 WPM, mean of four noise draws:

| SNR (2500 Hz) | −21 | −18 | −15 | −12 | −9 | −6 | 0 |
|---|---|---|---|---|---|---|---|
| none (stage 3, 300 Hz) | 42.0 | 42.5 | 32.3 | 19.9 | 7.8 | 0.8 | 0.0 |
| **narrow150** (no NR) | 40.0 | 38.3 | 26.3 | **13.9** | **3.2** | 0.4 | 0.0 |
| in_voice | 41.0 | 41.5 | 32.9 | 21.5 | 11.3 | 5.4 | 0.6 |
| in_cw | 40.4 | 40.4 | 31.9 | 20.3 | 7.6 | 2.3 | 0.0 |
| in_cw_sm | 41.4 | 41.3 | 34.5 | 24.4 | 13.5 | 3.6 | 0.0 |
| stft_cw (256) | 39.8 | 41.6 | 31.6 | 21.7 | 9.3 | 2.8 | 1.0 |
| stft128 | 41.8 | 41.3 | 33.1 | 21.6 | 9.3 | 1.4 | 0.0 |
| n150+stft (150 Hz, then stft128) | 40.7 | 37.9 | 27.3 | 15.0 | 4.2 | 0.5 | 0.0 |
| lms | 41.8 | 39.7 | 27.5 | 14.2 | 4.3 | 1.3 | 0.4 |
| n150+lms | 39.8 | 37.3 | 26.8 | 13.3 | 4.1 | 1.6 | 0.4 |

And what each does to the sound at −9 dB:

| variant | contrast | gaps | marks | onset at +10 dB |
|---|---|---|---|---|
| none | 3.2 dB | — | — | 1.0 ms |
| narrow150 | 5.4 dB | −3.5 dB | −1.3 dB | 1.7 ms |
| in_voice | 11.2 dB | −15.2 dB | −7.2 dB | 3.5 ms |
| in_cw | 7.2 dB | −8.1 dB | −4.2 dB | 1.8 ms |
| in_cw_sm | 4.5 dB | −9.1 dB | −7.8 dB | 0.6 ms |
| stft_cw | 6.9 dB | −9.0 dB | −5.3 dB | 2.2 ms |
| stft128 | 7.3 dB | −8.8 dB | −4.7 dB | 1.6 ms |
| **n150+stft** | **11.2 dB** | −13.1 dB | −5.2 dB | 2.0 ms |
| lms | 10.7 dB | −12.6 dB | −5.1 dB | 2.4 ms |
| n150+lms | 11.2 dB | −12.8 dB | −4.9 dB | 2.4 ms |

At 35 WPM (two draws), slot errors:

| SNR | −12 | −9 | −6 |
|---|---|---|---|
| none | 26.1 | 13.2 | 2.6 |
| **narrow150** | **18.5** | **7.0** | **1.1** |
| in_voice | 27.9 | 21.4 | 15.4 |
| in_cw | 26.8 | 17.0 | 6.0 |
| stft128 | 27.4 | 14.4 | 3.2 |
| n150+stft | 19.6 | 9.2 | 1.3 |
| lms | 22.0 | 9.8 | 2.6 |
| n150+lms | 20.0 | 9.7 | 3.3 |

What the tables say:

1. **The 150 Hz filter is the copy tool.** In every copyable row, at both
   speeds, it is the best entry or within the noise of the best, and it
   costs almost nothing in signal.
2. **Spectral NR alone does not improve copy.** At 20 WPM `stft128` sits a
   point or two behind stage 3 alone, within the noise of the measure. At
   35 WPM it is clearly behind. What it buys is 4–7 dB more contrast.
3. **The LMS line enhancer is the one NR that improves copy.** At 20 WPM it
   matches the 150 Hz filter. It does so by narrowing: it converges to a
   filter tens of Hz wide around the tone. At 35 WPM, where the elements
   are short, it falls behind the 150 Hz filter.
   - It is also the least consistent variant. Its mark loss swings from
     −2.7 dB to −6.0 dB between adjacent SNRs, as the predictor
     re-converges after every gap.
   - Its leakage setting was chosen on this bench, from four values tried
     on this signal. Without leakage the weights drift where the narrow
     input gives them nothing to hold onto, and the output grew by 8–17 dB
     at some SNRs.
4. **Voice settings are wrong for CW.** `in_voice` gives the deepest gaps
   and costs the most copy: at 35 WPM its errors at −6 dB are 15.4%, six
   times stage 3 alone. Its onset is twice as slow (3.5 ms; 6.3 ms at 35
   WPM), because α = 0.98 makes the gain lag a rising element.
5. **Spectral NR stops paying below about −12 dB.** At −15 dB and lower,
   its mark loss approaches its gap loss, contrast rises by only 1–3 dB,
   and its errors match stage 3 alone. The noise estimator and the gain
   can no longer tell a mark from a noise peak. The variants that still
   improve copy down there (`narrow150`, `lms` and the two `n150` pairs)
   do it by narrowing.
   - `lms` keeps 7–9 dB of contrast at −15 dB and below, but its errors
     don't follow. That contrast comes from occasional loud marks, not
     reliably louder ones.

## 7. Results: two signals in the passband

`tools/nr_study/cw_nr_two_signals.py` puts two stations inside the 300 Hz
filter:

- **Station A:** at the 700 Hz pitch, −3 dB SNR.
- **Station B:** 100 Hz above it, sending different text, 0, 6 and 12 dB
  below A.

It scores B through a 60 Hz analysis filter, which stands in for an ear
picking B out. The table gives B's slot errors and B's level against stage
3 alone:

| B below A | none | lms | stft128 |
|---|---|---|---|
| 0 dB | 0.0% | 1.0%, +1.3 dB | 0.0%, −1.3 dB |
| 6 dB | 0.8% | **5.0%**, −0.2 dB | 0.8%, −3.5 dB |
| 12 dB | 19.0% | **25.3%**, −6.8 dB | 20.4%, −6.5 dB |

**The line enhancer costs the weaker station real copy.** One predictor
serves both tones. A's energy dominates the prediction error, so the
weights fit A first and B gets what is left. The result is B six times
worse at 6 dB down.

**The spectral NR leaves B's copy where it was.** Each bin is judged on its
own, so A's presence does not change B's bins. It does turn B down a few
dB, as it turns down any weak signal.

In a pileup or a busy band this is the deciding difference. A spectral NR
behaves like quieter noise. A line enhancer behaves like a filter that
locks onto the loudest tone.

## 8. The sbitx's NR, read and measured

The sbitx's receive path (`rx_process()` in v5.401's `src/sbitx.c`, the
`54bugfixes` reference used throughout these notes) is one FFT:
overlap-save, N = 2048, L = 1024, at 96 kHz, with 46.9 Hz bins. Its
1025-tap filter fills M = N − L + 1 exactly, so there are no spare taps.
Its two NR modes, credited in the source to W2JON and W4WHL, both apply
per-bin gains inside that FFT.

**DSP, read from the code:**

- **Noise estimate:** updated once every `noise_update_interval` = 50
  blocks (0.53 s). Each bin's magnitude goes through a one-pole filter
  that rises 5% and falls 25% per update.
  - The estimate is an average of everything in the bin, signal included,
    not a noise-only statistic.
  - Rising 5% per half-second, it takes tens of seconds to settle after a
    change.
- **Gain:** a sigmoid in the magnitude SNR, centred at 0.5. In a noise-only
  bin (SNR ≈ 1) it subtracts 92% of the mean noise magnitude, with a floor
  at 10% of it. Bins that happen to sit above the mean survive. This is the
  classic recipe for musical noise.
- **Smoothing:** the code's comment says "bin-to-bin", but the code blends
  each bin with *its own previous frame* (0.9/0.1). That is time smoothing,
  not frequency smoothing.

**ANR, read from the code:** a relaxed Wiener gain from time-smoothed
magnitudes against that same noise estimate, with a 0.2 floor. Then
"bin smoothing": each complex bin becomes 0.8 of itself plus 0.1 of each
neighbour.

A convolution across bins is a multiplication in time. This one multiplies
every frame by 0.8 + 0.2 cos(2πn/N). The half of the frame overlap-save
keeps runs from 0.6 up to 1.0, so every 10.7 ms block is ramped by up to
4.4 dB. The bench measures **2.3 dB of block-rate (93.75 Hz) amplitude
modulation on a steady tone** after the filter, and none with ANR off.

**Measured**, at the same 300 Hz width, with the noise estimate settled
(40 s of noise run through first), against the sbitx's own filter alone:

| at −9 dB, 20 WPM | errors | contrast | gaps | marks |
|---|---|---|---|---|
| sbitx filter only | 5.5% | 3.7 dB | — | — |
| + DSP | 9.0% | 5.5 dB | −6.5 dB | −4.7 dB |
| + ANR | 6.9% | 4.2 dB | −5.8 dB | −5.3 dB |

DSP buys 1.8 dB of contrast for a rise in errors. ANR buys 0.5 dB, which
amounts to a volume cut with a 94 Hz flutter on it. The `stft128` and
`n150+stft` proposed here buy 4 and 8 dB. "Did not seem very good" is the
right description.

The sbitx filter on its own copies a little better than the bench's stage 3
(5.5% against 7.8%). Its 1025 taps ring for a third as long as 3073. That
is a lead for the narrow-filter notes, and not a reason to change this
study's conclusions: every NR comparison above is against its own baseline.

## 9. Where it goes in `rx_audio.c`

```
raw I/Q ─┬─► stage 1 ─► stage 2 ─► stage 3 ─┬────────────────────► × gain ─────────► uac_out
         │                                  │                                        (unchanged)
         │                                  ├─► S-meter (unchanged)
         │                                  │
         │                                  └─► NR (6 kHz) ──────► × gain(t − D) ─► × volume ─► speaker
         │                                                              ▲
         └─► AGC envelope ─► gain ─────────► delay line, D = NR latency ┘
```

**After stage 3, before the gain multiply.** Not after the gain, for a
reason specific to this AGC. Its envelope is the raw wideband I/Q, so a
strong station anywhere in the 96 kHz span pumps the gain at its keying
rate. That is by design, "AGC desense", as with a front-end AGC.

If NR came after the gain, its noise floor would rise and fall with the
gain by tens of dB. A minimum-statistics tracker would follow the ducked
floor, and treat the un-ducked noise as signal. Before the gain, the noise
floor NR sees is steady. The MMSE gain depends only on ratios, so the
absolute level there doesn't matter.

**The AGC's envelope does not move.** It still reads the raw I/Q, so NR
can't change what the AGC does. That keeps the frequency independence
`rx_audio_demod_design.md` §8.8 chose it for.

**The gain is delayed by NR's latency, D.** Without that, the gain would
lead the audio by stage 3's group delay plus about 23 ms. Every element's
onset would be fully ducked before it is heard, and an interferer's duck
would arrive 23 ms early. The delay line is one ring buffer of `double`,
about 2200 entries.

The fix §15 of the narrow-filter note tested and left uncommitted is the
same buffer with D extended by stage 3's group delay. Building NR makes
that a one-constant change (decision §16.3).

**What stays exactly as it is:**

- **`uac_out`** keeps `narrowed × gain`, undelayed and un-NR'd. The
  programs on it run their own detectors, and in DIGITAL it is WSJT-X's
  whole feed. The bench shows nothing a decoder would gain, and an NR's
  gating would hand it a noise floor that jumps.
- **The S-meter** keeps reading stage 3's output.
- **DIGITAL mode** inhibits NR, as it inhibits stage 3. USB and LSB stay
  off until voice settings exist (§12).

**Either stage 3 works.** NR reads `narrowed`, whichever realization
produced it, so the elliptic bank and the FFT filter are equally served.

**With stage 3 out, NR is held off too.** The audio then reaches 3.7 kHz
(stage 1's 3 kHz plus the pitch). Decimating that to 6 kHz needs a
decimation filter cutting at about 2.7 kHz: roughly 600 taps, about 3 ms
each way, and 11% of stage 1 instead of 2.4%. Holding NR off when the
filter is off avoids all of that. The widest filter, 600 Hz, still takes
NR (decision §16.5).

**One switch, CW-only to begin.** `U NR` / `u NR` on rigctld, matching
`U NARROW`, and a panel button. Off at start-up, and not remembered, like
NARROW.

## 10. Latency

The live latency of a short-time FFT stage is its frame length. Each
output sample waits for the last frame that covers it, which ends N − 1
samples later. The decimation and interpolation filters add their group
delays. With stage 3 in, everything is below 1.3 kHz, so a 1.5 kHz
passband and a 4.5 kHz stopband suffice: about 128 taps at 96 kHz, 0.7 ms
each way. Derived:

| | frame | resamplers | added latency |
|---|---|---|---|
| in-filter | — | — | 0 (but §4) |
| LMS | — | 1.3 ms | ≈ 11.5 ms (10.4 ms envelope lag, measured) |
| **spectral, 128 at 6 kHz** | 21.2 ms | 1.3 ms | **≈ 22.5 ms** |
| spectral, 256 at 6 kHz | 42.5 ms | 1.3 ms | ≈ 44 ms |

For scale, the speaker already trails the antenna by about 43 ms:

- one 10.7 ms capture block;
- stage 3's 4.5 ms;
- the 28 ms playback queue measured in `cw_keyer_design_study.md` §7.

The 128-point NR makes that about 66 ms, and only while it is on. The TX
path and the sidetone are untouched, so keying feel doesn't change.
Break-in listening between elements gets 22 ms later.

The bin width and the frame length trade against each other. 46.9 Hz bins
mean about 21 ms frames, at any sample rate. Lower-latency designs exist if
22 ms proves to be a problem: asymmetric analysis and synthesis windows,
or computing the gains in the FFT but applying them with a short
time-domain filter. Neither is proposed now.

## 11. CPU

Per 1024-sample block, against stage 1's 327 taps × 2 × 1024 ≈ 670k
multiply-adds:

| | per block | of stage 1 |
|---|---|---|
| decimate and interpolate, 128 taps polyphase | ≈ 16k MAC | 2.4% |
| spectral: 2 frames of a 128-point real FFT, inverse FFT and 65 bins of gain | ≈ 10k operations | ≈ 1.5% |
| LMS instead: 64 samples × 64 taps × 2 | ≈ 8k MAC | 1.2% |

That is under 4% of stage 1 on a Pi Zero 2W, the board
`zbitx_port_study.md` flags for CPU.

The MMSE-LSA gain needs the exponential integral E1. A table indexed by the
two SNRs (WDSP does this), or a rational approximation, keeps it to a few
operations per bin. Minimum statistics needs eight sub-window minima per
bin, which is 65 × 8 floats.

## 12. Voice later

The same `rx_nr.c` takes a second parameter set for USB and LSB:

- α ≈ 0.98;
- a −20 dB floor;
- every bin to 3 kHz;
- probably 256-point frames, since speech tolerates the latency and
  benefits from 23 Hz bins.

Nothing in the CW design blocks it. It wants its own test signal (speech,
not keying), so it is out of scope here. The answer to "separate NR
algorithms for voice and CW?" is: one algorithm, two sets of constants.
Not the same constants.

## 13. Risks and unknowns

- **The copy measure is not an ear.** Human copy at −9 dB may weigh
  musical noise, onset softening and contrast very differently from the
  slot test. Listening (§15 step 0) is the real test.
- **White noise only.**
  - **QRN** will defeat minimum statistics in both directions. Crashes
    read as signal, and a long burst lifts the noise floor for 1.5 s.
  - **QSB** below the noise floor for a second or more can be learned as
    noise.
  - **Neither is measured.**
- **The settings come from one bench.** α, the floor, the 1.5 s window and
  the 150 Hz bin margin are textbook-plus-one-pass, not tuned on air.
  Keep them together in one table at the top of `rx_nr.c`, so on-air
  tuning is one edit.
- **Retuning.** Minimum statistics takes up to 1.5 s to find a new noise
  floor after a band change or a big retune. The estimator should be
  reset on `radio_tune_to()` beyond some distance, and on a pitch, width
  or mode change.
- **Latency on break-in.** 22 ms more is unmeasured on the air. If it
  bothers, the lower-latency structures in §10 are the next step.
- **Stage 3 off means NR off** (§9). An operator who copies with the
  filter out gets no NR. If that matters, the sharper decimation filter is
  the price.

## 14. Recommendation

1. **Listen before building.** The WAVs (§17) are the same 24 s at −15
   and −9 dB through:
   - stage 3 alone;
   - the 150 Hz filter;
   - voice- and CW-tuned in-filter NR;
   - `stft128` and `n150+stft`;
   - `lms` and `n150+lms`;
   - the sbitx's DSP and ANR.

   If `n150+stft` isn't worth having over `narrow150` to your ears, stop
   here. The 150 Hz filter is already the copy tool.
2. **If it is:** build `rx_nr.c` as §9 places it.
   - **Algorithm:** the CW-tuned spectral NR, MMSE-LSA with
     minimum-statistics noise.
   - **Frames:** 128-point at 6 kHz.
   - **AGC gain:** delayed by NR's latency.
   - **Untouched:** `uac_out`, the S-meter and DIGITAL.
   - **Held off** while stage 3 is off.
   - **Defaults:** off; CW and CWR only.
3. **Operating advice that goes with it:** 150 Hz plus NR for weak
   signals, 300 Hz plus NR for comfort on a quiet band, NR off on a crowded
   band if it softens the weak ones.
4. **Not the line enhancer, at least not first.** Its single-signal numbers
   are good, but it costs a weaker station in the passband real copy (§7),
   and its tuning is this bench's. If listening favours it anyway, it fits
   the same seam as a second choice, "NR 2", for under 100 lines.
5. **Not in-filter, and nothing like the sbitx's.** Both sit inside the
   filter's FFT, where gains alias (§4, §8).

## 15. Suggested order of work and testing

0. **Listen to the WAVs** and decide whether NR is wanted at all.
1. **The seam, with no NR in it.** Build the stage between stage 3 and
   the gain, the 6 kHz decimate and interpolate, and the gain delay line.
   - Bench check: with NR off, out[] is bit-identical to today's.
   - Bench check: with a passthrough NR, out[] is today's delayed by
     exactly D, and `uac_out` is unchanged.
   - `test-rx-audio-impulse` re-run to confirm the onset transient doesn't
     move.
2. **`rx_nr.c`**, the spectral NR in C, against `cw_nr_bench.py`'s
   `SpectralNR`. A small C harness feeds the bench's noise and keying and
   compares gap and mark levels to the Python numbers within 0.5 dB.
3. **Controls:** rigctld `U NR`/`u NR`, the panel button, and the reset
   on retune, pitch, width and mode change.
4. **Pi Zero 2W timing:** per-block processing time with NR on and off,
   measured on the board.
5. **On air:** A/B at the same signals with NR on and off; then QRN and
   QSB; then a busy band.
6. **Only if asked:** the narrow-filter note's §15 AGC alignment, as an
   extra delay constant; LMS as "NR 2"; voice settings.

## 16. Decisions wanted before building

1. **Build at all?** Decide after listening (§15 step 0).
2. **Spectral first, LMS later or never?** (Recommended: spectral.)
3. **The gain delay:** match NR's latency only, so NR on or off doesn't
   change the AGC's character (recommended)? Or also take
   `rx_narrow_filter_fft_vs_elliptic.md` §15's alignment, worth +1–1.5 dB
   of onset, while the buffer is being built?
4. **Default off**, and CW and CWR only, for now?
5. **NR held off while stage 3 is off** (recommended), or a sharper
   decimation filter so it works without the filter?
6. **Control names:** `U NR` / `u NR`, and a panel button beside NARROW.

## 17. Reproducing the numbers

```
python3 tools/nr_study/cw_nr_bench.py --wav DIR       # §4, §6, §8: about 8 min
python3 tools/nr_study/cw_nr_bench.py --wpm 35 --trials 2
python3 tools/nr_study/cw_nr_two_signals.py           # §7: about 2 min
```

numpy and scipy are needed. The random generator is seeded, so a run
reproduces these tables exactly. `--trials 1` gives a two-minute run with
wider error ranges. `--wav DIR` writes the 20 listening files: 12 kHz mono,
24 s each, every variant scaled the same as stage 3 alone, so level
differences are real.

## Sources

- R. Martin, "Noise power spectral density estimation based on optimal
  smoothing and minimum statistics," *IEEE Trans. Speech and Audio
  Processing* 9(5), 2001.
- Y. Ephraim and D. Malah, "Speech enhancement using a minimum mean-square
  error log-spectral amplitude estimator," *IEEE Trans. ASSP* 33(2), 1985.
- O. Cappé, "Elimination of the musical noise phenomenon with the
  Ephraim and Malah noise suppressor," *IEEE Trans. Speech and Audio
  Processing* 2(2), 1994.
- B. Widrow et al., "Adaptive noise cancelling: principles and
  applications," *Proc. IEEE* 63(12), 1975 (the adaptive line enhancer).
- WDSP (W. Pratt, NR0V), the DSP library behind Thetis and piHPSDR:
  `anr.c` (leaky LMS line enhancer) and `emnr.c` (spectral MMSE NR with
  minimum-statistics noise estimation).
- sbitx v5.401 (`54bugfixes`), `src/sbitx.c` `rx_process()` and
  `src/sbitx_gtk.c` (`noise_update_interval`).
