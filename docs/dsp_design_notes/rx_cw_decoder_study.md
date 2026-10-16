# A CW decoder for maxibitx: study

Status: **built** - §8 is the decoder in maxibitx (`src/cw_decoder.c`)
and its measurements. §1-6 are the study that led to it; §7 is a detector
fix for the KB2ML decoder in sbitx (branch `decoder_fix`), measured on the
same bench.

The question: is it worth building a new CW decoder for maxibitx? The
target is hand-sent code at 5 to 35 WPM, at good signal-to-noise ratios,
in one module well under 1000 lines. Slow senders are assumed to use
Farnsworth-like spacing, so the slowest case is 5 WPM overall at 18 WPM
character speed. Decoded text would appear on the panel, as two lines
under a shorter waterfall in CW mode.

This study does three things. It builds a model of hand-sent CW and a
bench that runs it. It measures the decoder Mike wrote for sbitx (the
"KB2ML decoder", `modem_cw.c` on MikeJohnshoy/sbitx branch `mj_decoder`,
commit 6875d43) on that bench, against what the fists themselves allow.
Then it uses those numbers to say what a new decoder should do
differently.

The short answer is yes, it's worth building. The bench puts most of the
present decoder's errors in its detector, before any timing decision is
made, and a plain detector removes them. The timing classifier behind it
then only has to deal with the fist itself.

## 1. The bench

[`tools/cw_study/`](../../tools/cw_study/):

- `cw_fist.py` makes QSO-like text (CQ POTA, reports, names, QTHs,
  callsigns with `/P`, park numbers) and keys it with a fist.
- It then makes the audio a receiver would hand its decoder:
  - a tone at 700 Hz with 5 ms raised-cosine edges,
  - white noise at a given SNR (dB in 2500 Hz, the WSJT-X convention),
  - a 500 Hz CW filter,
  - scaled so the tone peaks where sbitx's AGC puts it (10⁸).
- `cw_bench.py` runs the fists through the decoders and scores them.
- `kb2ml/harness.c` runs the receive half of `modem_cw.c` off the radio.
  It stubs out what the rest of sbitx would supply: the key is up, the
  WPM and PITCH fields are set from the command line, and the clock
  advances with the audio. Audio goes in 1024 samples at a time, as
  `rx_linear()` hands it to `modem_rx()`.

```
python3 tools/cw_study/cw_bench.py --sbitx ~/sbitx/src      # all of it, ~10 min on 2 cores
python3 tools/cw_study/cw_bench.py --only ceiling,ambiguity # no sbitx needed, seconds
python3 tools/cw_study/cw_bench.py --sbitx ~/sbitx/src --decode rec.wav --wpm 20
```

**Scoring.** The character error rate (CER) is the edit distance between
what was decoded and what was sent, divided by the number of characters
sent. Spaces are removed from both texts, so CER counts characters. A
run is counted as *lost* when its CER is over 10%: the decoder lost the
sender's speed and stayed lost for much of the run. Each cell is 4 runs
of about 250 characters with different noise and timing draws. Because
runs are either decoded or lost, the grid gives the median rather than
the mean.

**The fists.** Each fist is a set of timing statistics, in dot units at
the sender's speed:

- the dash length and its spread,
- the spread of the dot,
- the three gaps (inside a character, between characters, between
  words),
- a weight (added to marks, taken from gaps),
- the chance of running two characters together, or breaking one in two,
- a slow wander of the speed.

| Fist | Dash | Spread of marks / gaps | Weight | Run together / broken | Drift |
|---|---|---|---|---|---|
| machine | 3.0 | 0 | 0 | 0 | 0 |
| good | 3.0 | 10% / 15-20% | 0 | 0 | 3% |
| average | 2.8 | 15% / 20-25% | 0.1 | 2% / 1% | 5% |
| poor | 2.5 | 20% / 25-30% | 0.2 | 5% / 3% | 8% |
| heavy | 3.3 | 12% / 20-25% | 0.4 | 2% / 0 | 4% |
| bug | 3.8, hand-made | dots 3%, dashes 20% / 5-30% | 0 | 3% / 0 | 3% |

These presets are a model. I chose them to span "machine" to "hard to
copy". They are not measurements of real operators. Recordings of real
hand-sent QSOs are what should calibrate them (§6).

**Two references for what is possible:**

- **Ceiling.** A timing-only decoder that knows the sender's true dot
  length at every moment. It uses the best fixed dot/dash,
  character-gap and word-gap thresholds for that run, found from the
  truth. It has no detector errors and no language model. Its errors are
  the fist's own: a 1-unit gap that came out longer than a short 3-unit
  gap, and so on. No real decoder knows the truth, but this is the score
  a decoder working from timing alone should approach.
- **Simple detector.** About 50 lines of numpy. It mixes the audio down
  from 700 Hz, lowpasses at 150 Hz and takes the magnitude every 1 ms. It
  calls mark or gap at half way between a peak tracked in marks and a
  floor tracked in gaps, with 10% hysteresis. Its marks and gaps go
  through the ceiling's classifier, so this measures what a real detector
  costs on top of the ceiling.

## 2. The detector is where KB2ML loses

Machine-perfect code at 20 WPM, with the decoder's WPM setting at 20.
The true dot, dash and gap are 60, 180 and 60 ms. Medians of what each
detector measured, then the mean of 12 runs:

| SNR, dB | KB2ML marks found | dot, ms | dash, ms | gap, ms | marks > 5 units | CER | runs lost | | simple: dot, ms | dash, ms | gap, ms | CER |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 6 | 98% | 75 | 203 | 40 | 0.8% | 7.0% | 3 of 12 | | 59 | 179 | 61 | 0.7% |
| 10 | 96% | 75 | 199 | 43 | 2.1% | 11.2% | 8 of 12 | | 59 | 179 | 61 | 0.0% |
| 15 | 93% | 75 | 196 | 43 | 5.2% | 15.5% | 4 of 12 | | 60 | 180 | 60 | 0.0% |
| 20 | 86% | 75 | 196 | 41 | 12.3% | 24.9% | 4 of 12 | | 60 | 180 | 60 | 0.0% |
| 30 | 95% | 75 | 203 | 42 | 4.8% | 8.0% | 1 of 12 | | 60 | 180 | 60 | 0.0% |
| 40 | 78% | 81 | 205 | 32 | 25.3% | 29.3% | 4 of 12 | | 60 | 180 | 60 | 0.0% |

The simple detector found every mark, at every SNR.

KB2ML makes every mark 15 to 25 ms longer and every gap 17 to 28 ms
shorter, at every SNR. A 1-unit gap at 20 WPM is 60 ms, so a third to a
half of it is gone. Any gap that comes out short goes completely, and
two marks become one: the "marks > 5 units" column. Even on perfect
code, a third of the runs lose the speed somewhere and stay lost for a
while. Which runs are lost depends on the noise draw, so these counts
move by a few between sets of draws.

The simple detector measures the same elements to within a millisecond
from 6 to 40 dB.

Reading `modem_cw.c` with a trace of each mark and gap
(`cw_rx_add_symbol()`) shows why:

- **Its time step is one call of `cw_rx()`: 1024 samples, 10.67 ms.** At
  35 WPM a dot is 34 ms, about three steps.
- **The magnitude passes through four delays, two of which scale with
  the decoder's own estimate of the dot length:**
  1. a 3-step median per bin,
  2. an EMA with α = 2/`dot_len` (at least 0.3),
  3. a 2-D k-means on log(1 + magnitude), whose mark/gap boundary sits
     near the geometric mean of the two levels,
  4. a second EMA, with τ = `dot_len`/2, before the hysteresis.
- **The k-means stage takes longer as SNR rises.** The magnitude falls a
  fixed number of dB per step after key-up, and the boundary sits further
  below the peak. That is why gaps are shortest at 40 dB.
- **The two `dot_len`-scaled stages form a loop.** A lost gap makes a
  long mark. A long mark raises `dot_len`. A higher `dot_len` slows both
  EMAs, so more gaps are lost. One trace climbed from 5 to 22 steps
  before `cw_rx_classify_and_output()`'s pull toward the WPM setting
  brought it back.

The detector's lag is a fixed fraction of a dot whatever the speed. This
matches the grid below: machine code at 15 WPM with the setting at 15
lost every run, measuring 7.5-step dots as 10 to 12 steps and 7.5-step
gaps as 3 to 4.

## 3. The grid

15 dB SNR. Median CER for KB2ML with the WPM setting at 20, as for a
sender of unknown speed:

| Fist | 10 | 15 | 20 | 25 | 30 | 35 |
|---|---|---|---|---|---|---|
| machine | 0.0 | 1.0 | 52.9 | 69.4 | 62.8 | 90.3 |
| good | 10.1 | 28.4 | 62.2 | 72.3 | 78.4 | 87.5 |
| average | 32.4 | 55.8 | 75.1 | 83.9 | 80.9 | 86.2 |
| poor | 65.4 | 76.4 | 80.1 | 85.6 | 88.3 | 89.0 |
| heavy | 72.9 | 82.9 | 86.1 | 88.6 | 90.3 | 89.9 |
| bug | 21.5 | 33.5 | 65.5 | 80.2 | 73.4 | 83.3 |

Farnsworth, overall / character WPM: good 5/18 65.2%, 10/18 72.7%;
average 5/18 78.7%, 10/18 82.5%.

The same with the WPM setting at the sender's speed (for Farnsworth, the
character speed):

| Fist | 10 | 15 | 20 | 25 | 30 | 35 |
|---|---|---|---|---|---|---|
| machine | 3.0 | 27.8 | 52.9 | 1.4 | 0.5 | 39.5 |
| good | 51.3 | 62.9 | 62.2 | 33.0 | 43.0 | 45.9 |
| average | 74.5 | 75.9 | 75.1 | 74.6 | 60.3 | 75.9 |
| poor | 79.1 | 81.9 | 80.1 | 81.2 | 77.9 | 82.5 |
| heavy | 84.8 | 87.2 | 86.1 | 87.5 | 84.8 | 86.9 |
| bug | 48.4 | 63.9 | 65.5 | 60.0 | 54.6 | 72.4 |

Farnsworth: good 5/18 72.1%, 10/18 85.9%; average 5/18 82.5%,
10/18 85.6%.

Outside machine code, every cell lost all 4 runs, except good at 10 WPM
with the setting at 20, which lost 2. Setting the WPM to the sender's
speed helps above 20 WPM and hurts below it. It isn't a cure, because
the detector's lag scales with the same number.

Mean CER for the ceiling. The simple detector's column is within a
point of it everywhere (largest gap: 1.4 points, poor at 25 WPM):

| Fist | 10 | 15 | 20 | 25 | 30 | 35 |
|---|---|---|---|---|---|---|
| machine | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| good | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| average | 9.0 | 8.7 | 8.4 | 9.1 | 9.0 | 9.1 |
| poor | 37.5 | 29.9 | 30.1 | 35.8 | 35.6 | 32.2 |
| heavy | 3.9 | 2.5 | 2.7 | 3.4 | 1.6 | 3.9 |
| bug | 3.8 | 1.5 | 2.3 | 3.8 | 3.4 | 3.4 |

Farnsworth: good 0.0% at both; average 3.0% at 5/18 and 3.7% at 10/18.

Read together:

- **Headroom.** For good, heavy and bug fists, timing alone can be
  close to perfect at every speed, and a plain detector keeps it there.
  KB2ML is at 10 to 90% on the same audio, so the room for improvement
  is large.
- **Heavy fists are KB2ML's worst case.** A weight of 0.4 leaves
  0.6-unit gaps, which its detector can't keep.
- **What timing alone can't fix.** On the average fist, about 9% CER is
  the fist's own: gaps that overlap between classes. That is the part
  where context (a language prior) has to help. The poor fist is about a
  third errors on timing alone; most operators would struggle with it
  too.
- **Farnsworth spacing is easy for a classifier that learns gaps from
  gaps** (0 to 4% ceiling). A decoder that derives gap thresholds from
  the dot length or the WPM setting gets it badly wrong.

## 4. Ambiguity: does validity tell a split from a merge?

Mike's objection to the earlier suggestion ("decide whether to split or
merge by which reading gives valid Morse characters") was that almost
every fragment is a valid character. Over about 20,000 characters of the
QSO text:

| Change | Gives valid characters |
|---|---|
| split a character at one of its gaps | 93% of 33,139 gaps |
| ... a character of 2, 3 or 4 elements | 100% |
| ... of 5 elements (digits, `/`) | 81.7% |
| ... of 6 elements (`?`) | 40.0% |
| merge two adjacent characters into one | 26% of 10,529 pairs |

He is right. For letters, validity never rules out a split. It rules out
a merge three times in four, but a merge only comes up when a gap
between characters came out short, so validity can settle only some of
the doubtful cases.

What decides the ambiguous gaps has to be:

- **the gap's own duration**, under the gap statistics learned for this
  sender: which of the 1, 3 and 7-unit clusters it most likely belongs
  to, and how sure that is;
- **for the few gaps that timing leaves near even**, a prior on the text
  (character pairs, and the QSO vocabulary: CQ, DE, TU, 5NN, RST, `/P`,
  callsign shapes).

The earlier suggestion should have said that. The ceiling in §3 is the
score with the first alone.

## 5. What a new decoder should do

1. **Detect at a fine, fixed time step.**
   - Mix down from the pitch and take the magnitude at about 1 ms (the
     simple detector uses a 150 Hz lowpass; 35 WPM needs about that).
   - Decide marks and gaps half way between a peak learned in marks and
     a floor learned in gaps, with hysteresis.
   - Keep the time constants in milliseconds, never tied to the speed
     estimate, which breaks KB2ML's loop.
   - The bench shows this gives timing within a millisecond from 6 to
     40 dB. Low SNR would need more (the target is good SNR).
2. **Classify marks and gaps separately, each from its own statistics.**
   - Marks: two running clusters, dot and dash, in log duration, with
     the threshold at their geometric mean.
   - Gaps: three clusters, inside a character, between characters, and
     between words, learned from the gaps themselves. That handles
     Farnsworth spacing, heavy fists and bugs without special cases.
   - Learn quickly while the clusters are unsure, then slowly. Use no
     prior from the operator's own keyer WPM: the grid shows that a
     prior from it hurts as often as it helps.
3. **Decide each character one gap late.** When the gap after a
   character is ambiguous between two clusters, keep both readings. The
   next character, and the text prior, choose between them.
4. **Use a small text prior only where timing is unsure.** Character-pair
   tables and the QSO words cost a few hundred bytes. On the average
   fist, the bench's 9% ceiling is what this has to cut into. It is the
   one part whose benefit this study hasn't measured.

**In maxibitx.**

- **Where it reads its audio.** It would take the demodulated CW audio
  after the receiver's filter, decimated to 6 kHz. It does its own
  mix-down at the pitch, so it doesn't depend on which filter is
  selected.
- **Threading.** The audio thread does only the cheap part: decimation
  and the 1 ms magnitude, written into a ring. The classification and
  text run on their own thread, as the keyer's edge handling does.
- **Size.** Detector about 100 lines, classifier about 250, segmentation
  and the prior about 250, tables about 100. That's well under 1000.
- **Getting the text to the panel.** rigctld has no push, so the panel
  would poll a new extended command for text since a sequence number.
  It would show two lines under a waterfall shortened in CW mode.

## 6. Before building

1. **Check the harness against the radio.** Record CW audio on the
   sBitx and run `cw_bench.py --decode rec.wav --wpm N`, then compare
   that with what sbitx's console showed for the same signal. The harness
   reproduces `modem_cw.c` exactly, but not how the rest of sbitx calls
   it: `cw_poll()` runs once per audio block here rather than from
   `ui_tick()`, and the input level is assumed, not measured. If its
   output is much worse than the radio's, the grid overstates the
   problem.
2. **Calibrate the fists from real hand-sent recordings**, so "average"
   means a real operator's spread. The simple detector can measure
   their element and gap distributions directly.
3. **Prototype the classifier in Python on this bench, without the
   oracle**, and keep it only if it gets within a point or two of the
   ceiling on good, heavy and bug fists at 10 to 35 WPM and Farnsworth.
   Then add the text prior, and keep it only if it beats the ceiling on
   the average fist.
4. **Then write it in C** as one maxibitx module, with the bench's audio
   as its test (`make test`), and the panel's two lines.

## 7. A detector fix for KB2ML (sbitx, branch `decoder_fix`)

§2's diagnosis suggests a fix inside `modem_cw.c` that leaves its timing
classifier (Sections 6 to 8) alone. It changes Sections 2 to 5:

- **A 2.67 ms time step.** Sections 3 to 8 run four times per
  1024-sample block, each time on a Goertzel window (still 128 samples,
  10.67 ms, at 12 kHz) slid along by 32 samples (`CW_HOP`). All receive
  timing (`dot_len`, `ticker`, the gaps) is counted in hops, so every
  `(6 * SAMPLING_FREQ) / (5 * N_BINS * wpm)` on the receive side
  becomes `CW_HOP`. The transmit self-decoder keeps its own blocks.
- **A linear half-way decision.** Mark when the magnitude rises above
  0.55 of the way from the noise floor to the peak, space when it falls
  below 0.45. This replaces the 0.5 / 0.15 thresholds with the log-domain
  k-means deciding in between. The k-means is still updated, because
  Section 7's confidence score uses its centroids.
- **No delays that scale with `dot_len`.**
  - The magnitude EMA (α = 2/`dot_len`) is gone. The window and the
    3-hop median already smooth the magnitude, and both delay rising and
    falling edges alike.
  - Section 5's EMA (τ = `dot_len`/2) is replaced by a fixed 2-hop
    debounce. It is symmetric, so it rejects one-hop glitches without
    changing lengths.
- **Level tracking in hops.** The peak follows the signal down over
  about 1 s in marks and 8 s in gaps, so the threshold holds through word
  gaps and Farnsworth spacing. The noise floor adjusts over about 1 s.
  Both are computed in 64 bits.
- **The anti-alias FIR keeps its history across blocks.** It used to
  start every block with zeros, upsetting the first 8 of each block's 128
  decimated samples. It now computes only the samples it keeps, which is
  8 times less work.

On the bench (`cw_bench.py --sbitx <decoder_fix>/src`; the bench reads
`CW_HOP` to scale the trace), machine code at 20 WPM with the setting at
20 now measures 59 / 179 / 61 ms (dot / dash / gap) at every SNR from 6
to 40 dB. No marks run together, and no run is lost; it was 24 of 72
before. CER is 0.5%, the first character of each run.

The grid at 15 dB, median CER before → after.

WPM set to 20:

| Fist | 10 | 15 | 20 | 25 | 30 | 35 |
|---|---|---|---|---|---|---|
| machine | 0.0 → **2.7** | 1.0 → **0.5** | 52.9 → **0.5** | 69.4 → **0.5** | 62.8 → **1.9** | 90.3 → **7.0** |
| good | 10.1 → **7.8** | 28.4 → **2.0** | 62.2 → **2.6** | 72.3 → **4.9** | 78.4 → **17.0** | 87.5 → **38.4** |
| average | 32.4 → **25.9** | 55.8 → **12.9** | 75.1 → **15.2** | 83.9 → **27.7** | 80.9 → **39.4** | 86.2 → **58.7** |
| poor | 65.4 → **60.9** | 76.4 → **41.0** | 80.1 → **44.1** | 85.6 → **58.4** | 88.3 → **68.0** | 89.0 → **72.9** |
| heavy | 72.9 → **11.1** | 82.9 → **17.1** | 86.1 → **36.1** | 88.6 → **55.2** | 90.3 → **65.2** | 89.9 → **75.7** |
| bug | 21.5 → **18.2** | 33.5 → **8.7** | 65.5 → **14.9** | 80.2 → **23.0** | 73.4 → **37.6** | 83.3 → **53.9** |

Farnsworth, before → after: good 5/18 65.2 → 0.5, 10/18 72.7 → 1.4;
average 5/18 78.7 → 13.0, 10/18 82.5 → 12.3.

WPM set to the sender's speed:

| Fist | 10 | 15 | 20 | 25 | 30 | 35 |
|---|---|---|---|---|---|---|
| machine | 3.0 → **0.5** | 27.8 → **0.5** | 52.9 → **0.5** | 1.4 → **0.5** | 0.5 → **0.5** | 39.5 → **0.5** |
| good | 51.3 → **3.3** | 62.9 → **2.5** | 62.2 → **2.6** | 33.0 → **1.7** | 43.0 → **2.8** | 45.9 → **1.0** |
| average | 74.5 → **17.5** | 75.9 → **19.3** | 75.1 → **15.2** | 74.6 → **18.0** | 60.3 → **20.7** | 75.9 → **16.4** |
| poor | 79.1 → **47.9** | 81.9 → **47.2** | 80.1 → **44.1** | 81.2 → **50.1** | 77.9 → **49.7** | 82.5 → **44.8** |
| heavy | 84.8 → **42.5** | 87.2 → **40.1** | 86.1 → **36.1** | 87.5 → **39.8** | 84.8 → **37.4** | 86.9 → **33.7** |
| bug | 48.4 → **19.8** | 63.9 → **14.4** | 65.5 → **14.9** | 60.0 → **14.7** | 54.6 → **17.5** | 72.4 → **18.0** |

Farnsworth: good 5/18 72.1 → 0.5, 10/18 85.9 → 0.8; average 5/18
82.5 → 11.5, 10/18 85.6 → 12.0.

Mean CER over the grid: 66.5% → 26.4% with the setting at 20, and
63.4% → 19.0% with it at the sender's speed.

**What is left is on the timing side.**

- **Fast senders with the setting at 20.** Good fist at 30 and 35 WPM:
  17% and 38%, against 3% and 1% with the setting matched.
  `cw_rx_classify_and_output()` pulls `dot_len` 40 to 80% of the way back
  toward the WPM setting after any character that didn't adapt it.
  `cw_rx_detect_symbol()` blends `dot_len` 50/50 with the setting for its
  gap thresholds.
- **Heavy fists and bugs.** They stay well above the ceiling (heavy at
  20 WPM: 36% against 2.7%), because the gap thresholds are multiples of
  the dot length rather than learned from the gaps.
- **Word breaks.** With word breaks counted, the grid's mean CER rises
  from 26.4% to 38.1%. The word gap starts at 7 dots and is only learned
  from gaps already above it.

These are the next changes to measure.

**Trying them one at a time doesn't work.** I tried them on top of the
fix, each on 16 cells × 3 runs at 15 dB, with the WPM setting at 20
(mean CER, then with word breaks):

| Variant | Mean CER | With word breaks |
|---|---|---|
| fix as built | 25.2% | 38.8% |
| no pull toward the setting | 25.2% | 39.3% |
| and gap thresholds from `dot_len` alone | 54.2% | 63.3% |
| and the nominal word gap at 5 dots | 54.2% | 71.5% |

Taking the setting out of the gap thresholds fixes fast senders. Good
fist at 35 WPM goes from 38.9% to 1.9%, and machine code at 35 WPM to
0.5%. But it wrecks slow ones: good at 15 WPM 2.0% → 88.3%, and machine
code at 10 WPM prints garbage, 320%. With nothing anchoring it, `dot_len`
runs down: the early adaptation pulls it toward the shortest recent
mark. So the setting is holding up other weaknesses in Sections 6 and
8.

Farnsworth shows the same coupling from the other side. Good fist at
5/18 decodes its characters (0.5%), but the long character gaps exceed
the 7-dot word threshold, so spaces appear between letters: 52.6% with
word breaks.

The timing side needs gap thresholds learned from the gaps themselves
(§5, step 2), replacing the dot-length multiples, the WPM anchor and the
self-referential word-gap EMA together. That is a separate change from
the detector fix, about the size of Sections 6 and 8's threshold code,
and it should be measured on this bench before any of it goes on the
air.

**Weak signals.** Neither version copies at −3 dB in 2500 Hz. The fix
does better at 0 dB (machine and good fists at 20 WPM, median CER 89% and
58% before, 41% and 45% after) and at 3 dB (11% and 24% before, 0.6% and
1.0% after). At −3 dB it prints more noise as characters (95 to 100%
against 76 to 81%).

**Cost.** With the FIR computing only the samples it keeps, the
decoder runs about 3 times faster than before on the same audio (on the
x86 bench: 0.19% of real time against 0.61%).

## 8. The decoder as built (`src/cw_decoder.c`)

The classifier was prototyped first in Python
([`tools/cw_study/cw_proto.py`](../../tools/cw_study/cw_proto.py)) on the
simple detector's marks and gaps, and tuned on the bench before it was
written in C. `cw_decoder.c` is the same classifier behind a detector of
the same kind, under 600 lines with its comments.

**How it works.** All of it runs on the audio thread, fed by
`rx_audio.c` with stage 2's output in CW and CWR, before the narrow
filter. It costs about 4 µs per 1024-sample block on the x86 bench, 0.04%
of the block's time.

1. **Envelope.**
   - Mix the tone down from the pitch, sum 16 samples at a time to
     6 kHz, and lowpass at 150 Hz (4th-order Butterworth on I and Q).
   - Take the magnitude every millisecond.
2. **Mark or gap**, every millisecond.
   - Half way between a tracked peak and a tracked noise floor, with 10%
     hysteresis.
   - The peak falls over 0.5 s in marks and 10 s in gaps. The floor
     learns in gaps only.
   - The first 300 ms only learn the floor.
   - Nothing counts as a mark while the peak is under 4 times the mean
     noise level. 30 s of noise alone gives no text.
   - Every time constant is fixed; none follows the speed estimate.
3. **Timing.**
   - **History:** the last 32 marks and 64 gaps, in log ms.
   - **Dot or dash:** the marks are split by Otsu's method (the split with
     the largest between-class variance), with the threshold half way
     between the two centres. A run of one kind of mark (E, T, S, O…) is
     judged against the gaps inside characters, which are one unit long.
   - **Gaps:** split the same way, short from long. The threshold sits
     0.42 of the way up from the short centre, which the bench preferred to
     half way: gaps between characters spread more than gaps inside them.
     The long gaps split again, between characters and between words.
   - **Blips:** a mark or gap shorter than 0.15 dot (6 ms at least) is
     noise and joins the event before it.
   - **Start-up:** nothing is decoded until 4 marks and 4 gaps have been
     seen; those are then decoded with what was learned.
   - **Pauses:** a gap of 2 s, and 2.5 word gaps, ends the transmission. A
     line break is written and the timing is learned afresh.
   - **Output:** a character is written as soon as its gap passes the
     character threshold, a space as soon as it passes the word threshold.
4. **Starting afresh.** `radio.c` resets the decoder on entering CW or
   CWR from another mode, and when the receiver moves more than 300 Hz.
5. **Out.** A 1024-character ring:
   - rigctld `\get_cw_text`, `l DECODEWPM` and `u`/`U CWDECODE`
     ([`../06_api.md`](../06_api.md)).
   - The control panel's two lines under a shorter waterfall in CW and
     CWR.

**On the bench**, 15 dB, the same 160 runs as §3 and §7. Median CER, then
averaged over 10 to 35 WPM. KB2ML has its WPM setting at 20; maxibitx has
none. The ceiling is the mean.

| Fist | KB2ML as it was | KB2ML, detector fix | maxibitx | ceiling |
|---|---|---|---|---|
| machine | 46.1% | 2.2% | **0.0%** | 0.0% |
| good | 56.5% | 12.1% | **0.1%** | 0.0% |
| average | 69.0% | 30.0% | **10.0%** | 8.9% |
| poor | 80.8% | 57.5% | **42.9%** | 33.5% |
| heavy | 85.1% | 43.4% | **5.0%** | 3.0% |
| bug | 59.6% | 26.1% | **6.2%** | 3.0% |
| Farnsworth, 5/18 and 10/18 | 74.8% | 6.8% | **2.0%** | 1.7% |

Over the whole grid the mean is 10.1% (11.7% with word breaks), against
the ceiling's 7.4%. Per cell, every machine and good-fist run decodes
perfectly at every speed except one good-fist cell at 20 WPM (0.5%).
The rest of the maxibitx column:

| Fist | 10 | 15 | 20 | 25 | 30 | 35 |
|---|---|---|---|---|---|---|
| average | 10.4 | 8.6 | 10.0 | 10.2 | 10.7 | 10.3 |
| heavy | 4.9 | 4.1 | 5.5 | 5.4 | 4.9 | 5.1 |
| bug | 7.4 | 4.6 | 4.9 | 6.4 | 5.7 | 8.1 |

Farnsworth: good 0.0% at both; average 3.3% at 5/18 and 4.8% at 10/18.

At 6 dB the grid's mean is 9.5% (ceiling 6.9%), with machine and good
fists at 0 to 0.8%.

**What's left.**

- The average fist is 1 to 2 points above the ceiling, heavy and bug 2
  to 4 points, poor about 9. That is the cost of learning the thresholds
  from the signal instead of knowing them.
- The text prior (§5, step 4) is not built. The one-gap-late decision
  between two readings (§5, step 3) isn't either: a character is
  written as soon as its gap passes the threshold.
- Changes of speed. A pause of 2 s or more, a retune or a mode change
  starts afresh. A speed change without a pause is followed as the
  32-mark window turns over, about ten characters.
- Bench only: no on-air test yet, no fading, no QRM.

**Tests.**

- `make test-cw-decoder` (`src/cw_decoder_test.c`):
  - machine code at 5, 20 and 35 WPM,
  - Farnsworth 5/18 and 10/18,
  - hand-sent (12% spread) at 12, 25 and 35 WPM,
  - 6 dB,
  - noise alone,
  - a second station at another speed after a pause,
  - the text reader.
- `test-rx-audio` case K checks the decoder through `rx_audio_process()`.
- `test-radio` checks when it is told to start afresh.
- `cw_bench.py` runs the grid on `cw_decoder.c` as it is in the tree. Pass
  `--sbitx` to add KB2ML beside it.

## Caveats

- The fists are a model; their spreads are my estimates.
- The noise is white, and the signal is steady: no fading, no QRM, no
  chirp.
- The ceiling is an oracle (the true dot length and the best thresholds
  for each run). No decoder will quite reach it, but the simple
  detector's column shows detection is not what stands between a decoder
  and it at good SNR.
- The grid is at 15 dB (and, for maxibitx, 6 dB). The detector table
  covers 6 to 40 dB for machine code at 20 WPM only.
- KB2ML was measured at commit 6875d43. Its transmit-side decoder wasn't
  measured.
