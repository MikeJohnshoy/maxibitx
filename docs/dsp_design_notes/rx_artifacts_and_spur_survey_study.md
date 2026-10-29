# Receiver artifacts, and a spur survey: a design study

Status: **survey built** (`tools/spur_survey.py`, with rigctld's
`L SIDRIVE` for comparing the si5351's drive) and run on the sBitx:
four dummy-load runs on 29 October 2026 are in §2.5. §1–§3 review the kinds of
receiver artifact and the ones met so far on the sBitx; §4 is the
design of `tools/spur_survey.py`; §6 records the decisions taken.

Written to answer:

- What kinds of unwanted thing can a receiver show? Are ghosts and
  birdies the same? What are spurs?
- Which have we already met in maxibitx, why, what do they cost, and
  what would remove them?
- Can a dummy load and careful tuning find them all? Does a noise
  generator help?
- Are spurs a matter of board layout, shielding and supply filtering?

Measured figures are from the raw-capture dumps of 26 October 2026
(three bad-state captures and one clean one, 7.030 MHz) and the first
`lpf_sweep.py` run on 28 October 2026 (antenna noise, 1–40 MHz). Where
something is predicted rather than measured, it says so.

## 1. The family of artifacts

### 1.1 Terms

**Spur** is the general word: any signal the receiver shows that isn't
on the air at that frequency. It covers two quite different things:

- **Internally generated spurs** — signals the radio makes itself, which
  show with no antenna at all. The common case is a steady carrier, a
  **birdie**: tuned across, it whistles up and down through zero beat,
  like a bird. Clock harmonics are the usual source.
- **Spurious responses** — the receiver hearing a real signal where it
  isn't: images, intermodulation, aliasing. These need a signal on the
  air to show anything, and they move with that signal.

**Ghost** has no strict meaning; it usually names a copy of a real
signal in the wrong place — an image, an IMD product, or (as we found)
sidebands of every signal. A **backward birdie** is one that moves the
wrong way when you tune: an image, or a product of the radio's own
oscillators mixing.

### 1.2 The tuning test

What an artifact does when the dial moves is the best single clue to
where it is made. Tune up by Δ:

| Artifact | On the display, it… | Made where |
|---|---|---|
| Internal spur at a fixed RF frequency | moves Δ down, as a station does | radiated or conducted into the RF input: Pi clocks, the codec's crystal, switching regulators, si5351 harmonics |
| Fixed-offset spur | stays put | after the first mixer: the crystal filter's IF, the codec, the DSP |
| Mixing-product birdie | moves at another rate, or backwards | harmonics of clk1 and clk2 mixing with each other or with the IF |
| Image / mirror ghost | moves **backwards** (Δ up) | the receiver responding on the far side of a mixer or a sample rate |
| Comb copies (sidebands) | moves with its parent signal | the signal path or the sampling modulated; our codec's 750 Hz combs |
| Intermodulation (IMD) | only with strong signals; 3 dB more attenuation drops it 9 dB | overload in the mixer, amplifier or ADC — typically evening broadcast stations |
| Aliasing | reflected about an edge of the 96 kHz band | signals past the codec's band folding back |
| Reciprocal mixing | a raised noise floor around a strong signal | oscillator phase noise mixing with the strong signal |
| Hum / rhythm sidebands | small copies at a fixed spacing either side of strong signals | supply ripple, or the 93.75 Hz audio block rate |
| Passband ripple | a wavy noise floor | the shape of a filter, or an echo in the samples |
| Time-domain artifacts | clicks, gaps | relays, xruns, a stalled audio thread |

A fourth movement says the most: an artifact that **appears and
disappears** at particular dial frequencies is a product of the
oscillators — it exists only where the frequency arithmetic lands it in
the passband.

### 1.3 Where they can arise on the sBitx

The frequency plan decides where to look. With the dial at D and the
crystal filter centred at S (`xtal_filter_center`, about 40.0124 MHz):

- clk2 (first LO) = D + S; the mixer's difference product puts a signal
  at D + d at crystal frequency S − d.
- clk1 (second LO) = S + 24 kHz; the codec sees 24 kHz + d.
- The software mixes at exactly 24 kHz = fs/4 to I/Q at 96 kHz.

From that, the predicted responses, each with what defends against it:

| Response | Where the signal really is | How it moves | Defence |
|---|---|---|---|
| First-mixer image | D + 2S ≈ D + 80.025 MHz — FM broadcast (88–108 MHz) lands on dials of 8–28 MHz | backwards | the LPF in use, far into its stopband |
| First-mixer harmonic responses | n·D + (n ∓ 1)·S for n = 2, 3, … (e.g. 2D + 40 MHz) | n − 1 times as fast | the LPF |
| IF feedthrough | ≈ S − d, a signal near 40 MHz leaking straight through the mixer | fixed offset | the LPF, and the mixer's isolation |
| Second-mixer image | D − 48 kHz − d: a station 48 kHz below the dial appears at +d | backwards | the crystal filter, 48 kHz from its centre (not yet measured that far out) |
| Codec aliasing | codec inputs past about 0.45 fs fold back to the band's edges, i.e. offsets beyond about ±20 kHz | backwards, at the display's edges | the crystal filter's skirts; the codec's decimation filter |
| Software-mix self-image | 48 kHz + d, folded near Nyquist | — | `antialias.c`, −81 dB from 47.5 kHz (by design) |
| Opposite sideband in the demodulator | the other side of zero beat | backwards, in the audio | stage 1's complex filter: a few dB at 0 Hz, about 10 dB at −200 Hz, 40–75 dB beyond −400 Hz |
| Codec timing modulation | ±k·750 Hz around every signal | with the signal | `codec_check.c` (§2.1) |

## 2. Artifacts met so far

| # | Artifact | Cause | Impact | Status |
|---|---|---|---|---|
| 1 | Ghosts every 750 Hz (strongest ±3750 Hz) on every signal, about 1 start in 25 | the WM8731's sample timing modulated in a 128-sample cycle (12 MHz clock, USB mode, 125 clocks a sample) | spectrum full of ghosts and backward birdies; −4 to −11 dB copies of strong signals | **fixed in software**: start-up check restarts the codec's interface |
| 2 | Own transmission on the spectrum at −1400 / +1400 Hz | TX energy reaching the RX input while clk1 sits at `bfo_freq` | a false signal on clients' spectra during TX | **fixed**: line-input mute during TX |
| 3 | Mix self-image near Nyquist | real samples × complex LO at fs/4 | would alias into the band's edges | **fixed by design**: `antialias.c` |
| 4 | Opposite-sideband leakage near zero beat | stage 1's finite transition band | a signal just the other side of zero beat is heard, weakly | inherent; acceptable |
| 5 | 93.75 Hz comb on both channels | current pulses at the audio block rate (every 1024 samples) coupling into the analog inputs | RX: none (codec 0–1 kHz is 23–24 kHz below the dial, outside the crystal filter). TX: the mic carries it; 375 Hz and up pass the 300–3000 Hz SSB filter | **open**: check SSB on air |
| 6 | Noise floor rippling with an 8 kHz period | an echo 12 samples long in the codec's samples, or a filter ripple; unidentified | ±several dB of floor shape; reads into auto floor and noise readings | **open**: measure with the noise generator |
| 7 | Broad raises at 24.0 and 30.75 MHz, slight at 12.5 and 25.0 MHz, in all five LPF states | internal; 24.0 = 2 × the codec's 12 MHz, 12.5 / 25.0 the si5351's 25 MHz TCXO; 30.75 unidentified | small (1–4 dB), none inside a ham band | the survey finds 25.000 MHz as a narrow 35–37 dB line (12); the rest outside its bands |
| 8 | LPF_B possibly rolling off at 17 m (−1.3 dB at 18.25 MHz, −3.8 dB at 18.5 MHz vs LPF_A) | the filter's cutoff close to its top band | some RX sensitivity and TX power on 17 m (the per-band TX scale covers the power) | **to confirm** with the noise generator |
| 9 | (zBitx, TX) spur 24 kHz from the transmit frequency in CW | the BFO running through key-up gaps | an out-of-band signal | traced on the zBitx; maxibitx flagged for a bench check (`ARCHITECTURE.md`) |
| 10 | Clicks and gaps at start-up | the playback queue draining before the audio thread ran (xrun flood) | glitches in audio and I/Q | **fixed**: playback primed before the thread starts |
| 11 | A comb of narrow spurs, harmonics of 48.0135 kHz, 3.6–29.7 MHz (30 of 10 m's 41 lines) | an unidentified crystal-stable clock, not the si5351's; coupled on the board after the LPFs | a line every 48 kHz, 10–25 dB in an 11.7 Hz bin; on the waterfall, rarely heard | **open**: find the clock (§2.5) |
| 12 | si5351 spurs: 25.000, 10.1456 (30 m), 14.3084 (20 m) MHz and five lines at 28.98–29.71 MHz, 17–39 dB | the si5351's reference and products of its outputs, on the board | 30 m and 20 m lines inside the bands, 36–39 dB | **open**: fall 1.4–16 dB at lower drive; the cost of lower drive unmeasured |
| 13 | Second-mixer images of those spurs, 11–14 dB weaker | the crystal filter only 11–14 dB down 48 kHz from its centre (inferred) | a strong station 48 kHz below the dial would make a backward ghost about that much weaker | **to confirm** with a carrier |
| 14 | The strongest artifacts: narrow lines up to ~50 dB, each seen at a single dial (3.655, 5.308, 5.318, 5.362, 7.300, 28.62 MHz) | some are images or edge-truncated lines (60 m at 1 kHz steps); 60 m's near 5.362 MHz is PLL A, or clk2's 16th harmonic, beating with clk1's 18th harmonic — at PLL A − PLL B's frequency, but unmoved when PLL B moves (100 Hz-step runs at clk1 dividers 18 and 22); the others likely the same kind (inferred) | strong, but only within a few kilohertz of particular dials, sweeping 15 times as fast as the tuning | **open**: which of the two (clk2's divider, §2.5); moving PLL B doesn't, and 4 mA drive takes off only 3 dB |

### 2.1 The codec's timing pattern (1)

The worst so far, and the most instructive. The spectrum looked like
mirroring, but the captures showed no mirror in the samples: no
even/odd mismatch, no repeated samples, every bit in place. What they
did show was every strong signal copied at multiples of exactly
3750.00 Hz (envelope correlation 1.00 with its parent), and the mic
channel's noise gated in a 128-sample rhythm — phase modulation of the
sampling, locked to the codec's own clock. In USB mode the WM8731 makes
96 kHz from 12 MHz with 125 clocks a sample where its timing is built
for 128; normally that pattern is invisible, but about one start in 25
it comes up strong. The registers are identical in good and bad starts;
restarting the digital interface (register 9, ACTIVE) clears it.

What fixed it: `codec_check.c` measures the pattern on the quiet mic
line a second after start-up (0.91–0.94 bad, 0.002 clean) and restarts
the interface if needed. What would remove the mechanism: a 12.288 MHz
codec clock, so the codec runs in normal mode at 128 clocks a sample —
a hardware change, and not yet shown to help.

### 2.2 The block-rate comb (5)

Present in every capture, good or bad: lines at multiples of
96000 / 1024 = 93.75 Hz, −44 dBFS on the mic channel and −55 dBFS on
the RX channel, 0.89 correlated between them below 4 kHz. The audio
thread wakes every 1024 samples and the Pi draws a burst of current;
that reaches the codec's analog inputs through the supply or the
ground. On receive it lands far outside the crystal filter. On SSB
transmit the mic's lines from 375 Hz up are inside the 300–3000 Hz
filter (375 Hz was −56 dBFS in the capture; how far that is under
speech depends on the mic's level) — possibly a faint buzz on the air.
A remote receiver with the mic idle and PTT down would show it. Remedies are all hardware: supply decoupling
and filtering for the codec's analog side, and the mic's ground return
(§3).

### 2.3 The 8 kHz floor ripple (6)

The floor dips about every 8 kHz (at codec frequencies 2, 10, 18, 26,
34 and 42 kHz), in every capture. An echo of 12 samples would do that;
so could a filter's ripple. A flat source shows the shape cleanly: with
the noise generator on the antenna, one capture dump and
`capture_check.py`, or one `xtal_sweep.py` run (its C(d) term is
exactly this shape), tell whether it is fixed in the codec's frequency
— and could simply be equalised in software — or moves with the
crystal filter.

### 2.4 The LPF sweep's broad raises (7)

`lpf_sweep.py` measures the median of the bins 1–10 kHz either side of
the dial, which ignores a narrow line — so a raise in all five states
is something broad: a noisy clock, or digital hash centred there. That
it shows with every relay open says it gets in after the filters, on
the board. Narrow birdies the sweep can't see at all; finding them is
what the survey (§4) is for.

### 2.5 The dummy-load surveys (11–14)

Four `spur_survey.py` runs on the sBitx on 29 October 2026, 80 m to
10 m, a 50 Ω dummy load on the antenna jack. In the order run (the
order the 48 kHz comb drifted):

| Run | Conditions | Fixed RF | Fixed offset | Backward | Mixing | Floor, dBFS/Hz |
|---|---|---|---|---|---|---|
| A | 8 mA (09:03) | 83 | 7 | 7 | 18 | −116.2 |
| B | 4 mA | 74 | 3 | 5 | 6 | −115.7 |
| C | 6 mA | 64 | 4 | 7 | 15 | −115.7 |
| D | 8 mA, every LPF relay open (10:10) | 48 | 4 | 6 | 18 | −115.6 |

The floor rose 0.5 dB after run A and stayed there whatever the drive —
D is at 8 mA — so it drifted with time, not with the drive.

**The 48 kHz comb (11).** 45 of run A's 73 drifting fixed-RF spurs are
harmonics of 48,013.5 Hz, from the 75th (3.60 MHz) to the 618th
(29.67 MHz), within 3.6 Hz rms; 30 of 10 m's 41 lines are its. It is
crystal-stable but not the si5351's: between runs it drifted −1.2 to
−1.5 ppm against the TCXO (the 584th harmonic went from 28.039880 to
28.039838 MHz), and its 10 m lines fell from a median of 18.7 dB over
the floor in run A to 13.8–14.7 dB in B, C and D, whatever the drive.
It is there with every relay open, so it couples in on the board after
the LPFs, not through the antenna jack. No clock on the board is known
at 48.0135 kHz. Half the codec's frame rate would fit — a pattern on the
I2S lines repeating every two frames — if its 12 MHz clock ran about
280 ppm fast; measuring the codec's sample rate against the TCXO would
settle it.

**The si5351's own (12).** These don't move between runs (within
0.4 Hz): they are locked to the TCXO, as the receiver's oscillators
are. They are there with the relays open, and they weaken with lower
drive:

| MHz | 8 mA (A) | 6 mA (C) | 4 mA (B) | 8 mA, relays open (D) |
|---|---|---|---|---|
| 3.980863 | 28.6 | 24.3 | 22.5 | 28.5 |
| 10.145584 (30 m) | 38.0 | 36.6 | 35.5 | 36.0 |
| 14.308440 (20 m) | 39.0 | 35.4 | 33.8 | 38.8 |
| 24.999993 (the TCXO) | 36.8 | 34.8 | 35.4 | 37.2 |
| 28.980864 | 17.2 | 12.9 | 9.6 | 16.1 |
| 29.162850 | 30.9 | 28.7 | 25.4 | 30.0 |
| 29.344837 | 27.9 | 26.4 | 16.1 | (a gap in D's data) |
| 29.526826 | 36.8 | 31.9 | 20.9 | 36.0 |
| 29.708812 | 20.4 | 15.3 | 13.2 | 17.3 |

dB over the floor. Every one is a × clk1 + b × 25 MHz — the five from
28.98 to 29.71 MHz too, 5 × clk1 − 8 × 25 MHz (182 kHz) apart — §2.7.

**Images (13).** Four of the backward spurs are the second mixer's
images of fixed-RF spurs the survey also found — 14.3084, 29.1629,
29.3448 and 29.5268 MHz — each 11–14 dB weaker than its source. That
suggests the crystal filter is only 11–14 dB down 48 kHz from its
centre, so a strong station 48 kHz below the dial would throw a
backward ghost about that much weaker than itself. A carrier would
measure it directly (§5).

**Mixing products.** The clk2-harmonic products (slopes ±2, ±3, ±4):
18 at 8 mA, 15 at 6 mA, 6 at 4 mA, and 18 with the relays open (made on
the board).

**Single-dial lines (14).** The strongest narrow lines in every run,
up to about 50 dB over the floor (3.655, 7.300 and 28.62 MHz; about
36 dB at 5.308, 5.318 and 5.362 MHz), are each seen at only one dial,
so they aren't classified. They are at the same frequencies in all four
runs, relays open included, so they are not stations — a steady station
would be seen from four dials — but products steep enough (a slope of 4
or more) to cross the ±10 kHz window between one 5 kHz step and the
next. A run on one band with 1 kHz steps would classify them.

**60 m at 1 kHz steps.** A fifth run, 60 m only with 1 kHz steps
(--bands 60 --step 1000), sorted three of 60 m's single-dial lines:

- 5.308 and 5.318 MHz are one backward line, seen seven times (5.3085
  to 5.3225 MHz, 37.8 dB): the second mixer's image of a line at
  5.2548 MHz, below the band where the survey didn't reach, and by
  §2.5's 11–14 dB likely about 50 dB itself.
- 5.437 MHz is a fixed-RF line at 5.436761 MHz, 29 dB, just above the
  band: at 5 kHz steps the survey's edge left it too few sightings.
- 5.354, 5.359 and 5.362 MHz are still each seen at a single dial even
  at 1 kHz steps: slopes steeper than the survey fits (6), or lines
  that exist only at particular dials.

The +8688 Hz fixed offset was 37.7 dB in this run, against 8.6–15.6 dB
before: whatever makes it varies from run to run (it is outside every
passband).

**A likely source of the single-dial lines: the si5351's two PLLs
beating.** clk2 comes from PLL A at clk2 × its divider (the smallest
even one above 650 MHz / clk2), so PLL A moves about 10–16 times as fast
as the dial and covers 650–780 MHz over the bands; clk1 comes from PLL B
at 18 × 40.036 MHz = 720.655 MHz, in the middle of that range. Their
difference, and its sums with harmonics of the 25 MHz reference, sweep
through HF, each crossing a given dial for well under a kilohertz of
tuning: a strong line at one dial setting only. The first-order
predictions, from `si5351v2.c`'s own arithmetic (products of up to
twice each PLL and ±8 × 25 MHz landing within 10 kHz of the dial):

| Band | Product | Crosses the dial at |
|---|---|---|
| 80 m | 2A − 2B + 2 × 25 MHz | 3.5779 MHz |
| 80 m | A − B + 25 MHz | 3.6971 MHz |
| 60 m | A − B | 5.3638 MHz |
| 40 m | 2A − 2B + 5 × 25 MHz | 7.2579 MHz |
| 20 m | −(A − B − 2 × 25 MHz) | 14.0321 MHz |
| 20 m | −(2A − 2B + 5 × 25 MHz) | 14.2406 MHz |
| 15 m | −(2A − 2B − 2 × 25 MHz) | 21.2406 MHz |
| 10 m | −(2A − 2B + 2 × 25 MHz) | 28.1459 MHz |
| 10 m | 2A − 2B + 4 × 25 MHz | 28.4770 MHz |
| 10 m | −(A − B) | 29.1392 MHz |
| 10 m | −(2A − 2B + 25 MHz) | 29.3363 MHz |

Several fall where the plots show single-dial lines — 60 m's near
5.362–5.364 MHz in every run, 15 m's near 21.24, 10 m's near 29.14 —
but the plots aren't precise enough to call it proven, and the
strongest lines (3.655, 7.300, 28.62 MHz) aren't among these
first-order products. Two checks: each line's exact dial and frequency
from the runs' `_peaks.csv` against the table; and a narrow run with
100 Hz steps across one predicted crossing, which would show a line of
slope about +15 (the survey would need to fit slopes that steep).

Reanalysed with the steep-line fitting, the 60 m 1 kHz-step run gives
17 steep "lines", every one from just two sightings at neighbouring
dials, with slopes from −16 to +17 that are mostly not whole numbers
(10.26, −9.27, −10.40, 7.47 ...), as a PLL product's would be, and none
matching a PLL A/B product — no slope-15 line crosses near
5.3638 MHz. Two sightings are too few to tell a steep line from two
unrelated ones (§4.3), so this neither confirms nor rules out the
prediction; at 1 kHz steps a slope-15 line is seen at most twice. The
deciding run is 100 Hz steps across 5.3638 MHz, where it would be seen
about 13 times.

**The 100 Hz-step run finds the predicted line.** `--range 5360000 5368000 --step
100` (81 dials) found a line of slope +14.996, seen 13 times, 35.1 dB
over the floor (−67 dBFS), crossing the dial at 5.363786 MHz: the
predicted PLL A − PLL B, which crosses at 5.363785 MHz. (Its frequency,
not its source: see the divider-22 runs below.) With it:

| Slope | Crosses the dial at | Sightings | dB | What it is |
|---|---|---|---|---|
| +15.0 | 5.363786 MHz | 13 | 35.1 | the beat, at PLL A − PLL B's frequency |
| −15.0 | 5.360585 MHz | 13 | 34.3 | the same beat, folded (below) |
| +29.9 | 5.362985 MHz | 7 | 19.6 | twice the beat |
| −29.9 | 5.361385 MHz | 7 | 19.0 | twice the beat, folded |
| −40.0 | 5.362911 MHz | 4 | 14.4 | not identified |
| −12.0 | 5.365870 MHz | 15 | 13.5 | not identified |
| +79 | 5.367191 MHz | 3 | 14.5 | not identified |

All four of the first lines meet at 5.362185 MHz, where the beat
is 24 kHz below the dial: there it lands on clk1 itself, and the beat
with clk1 — the audio frequency the codec sees — passes through zero.
Either side of that dial the codec sees the same tone, at the same
audio frequency whichever side of clk1 the beat is on, so the line comes
back as the slope −15 line, crossing 48 kHz ÷ 15 = 3.2 kHz lower. It is
the same relation as the second mixer's images (13), with a difference: it is
only 0.8 dB weaker than its source, where the images of real signals
were 11–14 dB weaker. For the folded line the beat is 48 kHz above the
crystal filter's centre; a signal coming through the filter would lose
those 11–14 dB there. So the beat doesn't come in through the antenna
path and the crystal filter: it gets in after the filter — on clk1's
own output (both PLLs are on one chip), in the second mixer, or between
there and the codec. The ±30 pair, 15 dB weaker, meets at the same dial
with twice the slope: the beat's second harmonic, made after the beat
exists (or 2 × (A − B) beating with clk2 − clk1, the same frequency;
the run can't tell them apart). No product of up to six times each
PLL, eight times clk2, four times clk1 and forty times 25 MHz explains
the −12, −40 and +79 lines, so something other than the si5351's own
clocks is probably in them.

The run was made twice, first with the tool from before steep lines
needed three sightings (10:57), then with the current one (11:10).
Every line came back crossing the dial within 2 Hz of where it did the
first time, and within 0.6 dB: the beats are as steady as the TCXO.
The first run's −94 line, two sightings, was gone; its +78, also two,
was found as +79 with three, crossing at 5.367191 MHz both times, so it
is real. Each run had one marginal extra (a fixed offset at +6690 Hz,
8 dB, four sightings; a fixed-RF line at 5.371084 MHz, 17 dB, three
sightings out of the 13 dials it should have been seen from) — not
repeated, so not counted. Of the fixed offsets, −3547/−3594 Hz and
+308/+301 Hz moved between the runs as before, but +8688 Hz stayed
within 0.1 Hz (8688.4 Hz in the 1 kHz-step run): that one is probably
locked to the TCXO — made from the si5351's clocks after the first
mixer — rather than a beat of free-running ones.

Where it is heard: the beat sweeps through a 2.7 kHz passband in about
180 Hz of tuning, near 5.3638–5.3640 MHz and again, folded, near
5.3604–5.3606 MHz on USB — a whistle that races through the passband as
the dial moves. It is on the ±24 kHz display only for dials from
5.3590 to 5.3654 MHz.
Every predicted crossing in the table will have a twin like this, where
the product is 48 kHz below the dial.

Because the beat reaches the receiver after the crystal filter,
shielding the antenna path wouldn't touch it. If PLL B were in it, it
would be a matter of frequency planning, in software: PLL B need not
sit inside PLL A's range.

**Moving PLL B.** clk1 is on divider 18 because that is the smallest
even divider putting its PLL above 650 MHz — the rule every clock gets.
Any of 16, 18, 20 and 22 keeps PLL B inside the si5351's 600–900 MHz
over the whole `xtal_filter_center` range, with clk1's frequency
unchanged; rigctld's `L CLK1DIV` fixes one until restart, and
`spur_survey.py --clk1-div` makes a run with it. Where the products
cross the bands at each, from `si5351v2.c`'s arithmetic — products
±(m × PLL A − n × PLL B + k × 25 MHz), m 1–3, n 0–3, |k| up to 40:

| Band | 16 (640.6 MHz) | 18 (720.7 MHz, now) | 20 (800.7 MHz) | 22 (880.8 MHz) |
|---|---|---|---|---|
| 80 m | 11 | 10 | 14 | 9 |
| 60 m | 3 | 2 | 0 | 0 |
| 40 m | 4 | 5 | 5 | 4 |
| 30 m | 0 | 1 | 0 | 0 |
| 20 m | 3 | 8 | 5 | 6 |
| 17 m | 1 | 2 | 1 | 1 |
| 15 m | 7 | 5 | 7 | 3 |
| 12 m | 1 | 3 | 1 | 3 |
| 10 m | 20 | 20 | 16 | 15 |
| All | 50 | 56 | 49 | 41 |

Each also has its fold, 48 kHz ÷ slope away. The count matters less
than which products they are. The one measured, 35 dB, is the simplest,
A − B itself; the likeliest strong ones are A − B with a small multiple
of 25 MHz (up to 6), and those are:

- divider 18: A − B + 25 MHz at 3.6971 MHz (80 m), A − B at
  5.3638 MHz (60 m), −(A − B − 2 × 25 MHz) at 14.0321 MHz (20 m),
  −(A − B) at 29.1392 MHz (10 m);
- divider 22: A − B + 5 × 25 MHz at 21.2040 MHz (15 m), and no others;
- divider 20: 3.5606 MHz (80 m) and 29.6004 MHz (10 m);
- divider 16: six, on 40, 20, 17, 15 and 10 m.

So 22 is not the same on every band: 60 m and 30 m are left with no
product at all; 80, 20 and 10 m lose their simplest ones but keep many
with higher multiples of 25 MHz, whose strength nobody has measured;
15 m gains one; 40, 17 and 12 m change little. And the strongest
single-dial lines (3.655, 7.300 and 28.62 MHz) and 60 m's −12, −40 and
+79 lines match no PLL product, so they may not move at all. The
divider can be chosen per band if the measurements say so: a band's
clk1 divider is a few register writes when the band changes. Two
first runs:

```
spur_survey.py --range 5360000 5368000 --step 100 --clk1-div 22   # 60 m: the beat should go
spur_survey.py --range 21200000 21210000 --step 100 --clk1-div 22 # 15 m: does one come?
```

**The divider-22 runs: not PLL B.** On 60 m the beat stayed: slope
+14.996, 13 sightings, 35.0 dB, crossing at 5.363787 MHz against
5.363786 at divider 18; the fold, the ±30 pair and the −12, −40 and +79
lines all came back within 1.5 Hz and 2 dB. PLL B had moved 160 MHz,
so A − B had left 60 m altogether. Nothing else changed either: the
receiver worked, the floor (−115.5 dBFS/Hz) and the three fixed offsets
were as before, so clk1 was where it should be. On 15 m no steep line
came at all, where A − B + 5 × 25 MHz would have crossed at
21.2040 MHz.

So the line has PLL A − PLL B's frequency without PLL B in it. At
divider 18 that is no coincidence: PLL B is exactly 18 × clk1, so
A − B is also A − 18 × clk1, and also 16 × clk2 − 18 × clk1 (PLL A is
16 × clk2 on 60 m). Moving PLL B changes the first form only. What is
left:

- PLL A itself, on the chip, beating with clk1's 18th harmonic; or
- clk2's 16th harmonic with clk1's 18th, both from the outputs' square
  waves, mixed in either mixer. Put another way, 18 × clk1 − 15 × clk2
  lands in the first IF: the classic spur of two LOs' harmonics.

Either way it comes from the two output frequencies, which the
frequency plan fixes: it crosses where 16 × (dial + S) − 18 × (S + 24
kHz) = dial, that is at (2 × S + 432 kHz) ÷ 15. Moving it out of 60 m
would take S 250 kHz away from the crystal filter. The tests that tell
the two apart, and what might lower it:

- clk2 on another divider on 60 m (14, PLL A at 635 MHz): the first
  form moves to about 12.35 MHz, the second stays. Needs an `L CLK2DIV`
  like `L CLK1DIV`.
- `--drive 4` over the same range: both forms should weaken with the
  outputs' harmonics; how much says whether drive is a remedy. (Done:
  below.)
- If it is the outputs' harmonics, a low-pass or series resistor on the
  clock lines (the 16th and 18th harmonics are at 720 MHz) and the
  layout around them — hardware, not software.

Two more lines came with divider 22, each crossing where a main line
does with the opposite slope — +15 at 5.360586 MHz (8 sightings,
12.5 dB) and −15 at 5.363787 MHz (10 sightings, 11.2 dB) — the beat
mirrored about the dial, 21–24 dB below it. They weren't in either
divider-18 run. Not explained.

The table of crossings above still gives where products with PLL B in
them would be; with none of them seen to move, it is not yet known
whether any are strong.

**At 4 mA.** The same range at divider 18 and 4 mA, against the two
8 mA runs (dB over the floor):

| Line | 8 mA (two runs) | 4 mA |
|---|---|---|
| +15, the beat (5.363786 MHz) | 35.1, 35.6 | 32.4 |
| −15, its fold (5.360585 MHz) | 34.3, 34.1 | 35.9 |
| +30 (5.362985 MHz) | 19.6, 19.0 | 12.4 |
| −30 (5.361385 MHz) | 19.0, 18.4 | 15.9 |
| −40, −12, +79 | 13–15 | not found |
| fixed offset +8688 Hz | 12.4, 13.2 | not found |
| the mirrored pair (±15, opposite slope) | not found | 13.8 and 9.3 |

Halving the drive takes 3 dB off the beat and adds 1.6 dB to its fold:
it is hardly weaker. The outputs' 16th and 18th harmonics come from
their edges, which 4 mA slows, and the si5351's own harmonic lines fell
by up to 16 dB at 4 mA (§2.5, survey B); a product of two of them, made
in the board's mixers, would be expected to fall a lot more than 3 dB.
That leans towards the first form, PLL A beating on the chip, though it
doesn't prove it — the clk2 divider test does. If it is PLL A, the
remedy is in software after all: clk2 on divider 14 or 18 on 60 m (PLL A
at 635 or 817 MHz) puts the crossing at about 12.35 MHz or nowhere near
HF. The drive does take out the weaker lines — −12, −40, +79 and the
+8688 Hz offset (so that one is the si5351's, as its steadiness
suggested) — and the beat's second harmonic falls 3–7 dB. The mirrored
pair, missing from both 8 mA runs at divider 18, was there at 4 mA as
at divider 22: it comes and goes, rather than following either setting.
(A +29 line crossing at 5.367172 MHz, three sightings, may be +79's
sightings fitted another way; 19 Hz apart.)

**Pickup through the antenna jack.** 17 weak (8–11 dB) fixed-RF spurs
below 25 MHz, drifting −1.6 to −2 ppm between runs (another free-running
clock, not the comb's), are gone with the relays open: they come in
through the antenna path — the jack, the dummy load or its lead. (Two
more near 3.647 MHz, 15–19 dB, are 300 Hz wide and go too: a station
getting in.) Three
stronger ones stay (3.6169, 3.7989 and 21.0191 MHz). Opening the relays
doesn't shut everything out: the broadcast carrier at 7.295 MHz still
shows.

**Fixed offsets.** They moved between runs (−3428 to −3581 Hz, +364 to
+301 Hz): beats between free-running clocks, not fixed frequencies. The
one at +8688 Hz is the exception: steady to 0.1 Hz between the 60 m
runs (above). The
one near +300 Hz, at every dial and 11.5–14.5 dB, is inside the USB and
DIGITAL passbands but about 10 dB under the noise in 2.7 kHz: a faint
line in WSJT-X's waterfall at most.

**The drive.** Lower drive takes out most of the si5351's mixing
products (12 of 18 at 4 mA, 3 at 6 mA) and lowers its own lines by
1.4–5 dB at 6 mA and 1.4–16 dB at 4 mA. Whether it costs sensitivity
isn't known yet: the comb can't serve as a reference, since it changed
with time, and with a dummy load the floor is the receiver's own noise.
A steady signal measured at 8, 6 and 4 mA — a beacon, or the noise
generator through `lpf_sweep.py` — would tell.

**Gaps.** Each run lost some steps on 10 m (different ones each time:
28.63–28.74 MHz in A, 28.17–28.25 and 29.30–29.38 MHz in D), where no
I/Q arrived. The tool now measures such a step again once (§4.3).

### 2.6 Which are heard

From 29 October 2026 the aim is narrower: the spurs heard by ear, over
the antenna's noise, working CW and SSB; 60 m after every other band.
A spur is heard when, at some dial, it is in the CW filter or the SSB
passband and louder there than the antenna's noise in about 50 Hz
(§4.9). Most of what the dummy-load surveys found isn't: it is outside
the passband at every dial, or under the noise.

The antenna's noise, measured at 13:20 local time (a survey with the
antenna connected; its spur list is stations and is ignored), against
the dummy load's floor in survey A:

| Band | 80 m | 60 m | 40 m | 30 m | 20 m | 17 m | 15 m | 12 m | 10 m |
|---|---|---|---|---|---|---|---|---|---|
| Antenna, dBFS/Hz | −103.6 | −111.9 | −104.1 | −114.9 | −106.6 | −115.2 | −114.2 | −113.4 | −115.2 |
| Over the dummy load, dB | 12.3 | 4.2 | 12.2 | 1.4 | 9.8 | 1.0 | 2.4 | 2.8 | 0.8 |

On 30, 17, 15, 12 and 10 m the antenna adds almost nothing to the
receiver's own noise that afternoon, so whatever the dummy load shows
there is heard. Survey A's spurs (8 mA) judged against it:

| Band | Heard | Within 6 dB under | Of those heard, the 48 kHz comb |
|---|---|---|---|
| 80 m | 6 | 3 | 0 |
| 40 m | 0 | 4 | 0 |
| 30 m | 2 | 0 | 0 |
| 20 m | 2 | 4 | 0 |
| 17 m | 3 | 0 | 1 |
| 15 m | 8 | 0 | 6 |
| 12 m | 3 | 0 | 1 |
| 10 m | 48 | 0 | 30 |
| 60 m | 3 | 0 | 0 |

The loudest, in dB over the antenna's noise in 50 Hz:

- 29.526826 MHz, +33 (10 m), and 29.162850 and 29.344836 MHz, +25 and
  +22: the si5351's group 182 kHz apart (12), each with its image (13)
  48 kHz up, +10 to +18;
- 10.145584 MHz, +32: near the top of 30 m, 4 × clk1 − 6 × 25 MHz;
- 14.308440 MHz, +24: in 20 m's SSB segment, the si5351's;
- the 48 kHz comb (11): 30 lines on 10 m at +5 to +21, six on 15 m at
  +2 to +12, one each on 12 and 17 m — in survey A, when the comb was
  4–5 dB stronger than in the later ones;
- 3.980863 MHz, +12, in 80 m's phone segment, the si5351's;
- the fixed offset at +364 Hz (+301 Hz later), heard in USB at every
  dial on 10–17 m at +7 to +9 — but at the passband's 300 Hz edge,
  where the filter takes some of it.

40 m has nothing heard, and 20 m only two. Not in these lists: the
strongest single-dial lines (3.655, 7.300, 28.62 MHz), which a 5 kHz
survey doesn't classify (`--range` at small steps would).

By ear, with the antenna (29 October), on the maxibitx S-meter:

| Dial | Margin (§4.9) | Heard |
|---|---|---|
| 29.5268 MHz CW | +33 dB | S4 |
| 10.1456 MHz CW | +32 dB | S4 |
| 14.3084 MHz CW, 14.306 MHz USB | +24 dB | very audible, S2 |
| 28.3280 MHz CW (the comb) | +21 dB | very audible, S2 |
| any 10 m dial, USB (+300 Hz offset) | +8 dB, at the filter's edge | not a problem |

The margins order them as the ear does. The offset's 8 dB at the
passband's edge isn't enough to matter.

### 2.7 The clk1 lattice

The three loudest heard (29.5268, 10.1456, 14.3084 MHz) are all of one
family, and so is every other si5351 line the survey found that doesn't
move: a × clk1 + b × 25 MHz, within the 6–8 Hz by which the survey reads
the TCXO itself low (25 MHz reads 24.999993 MHz):

| MHz | Band | a | b | dB over the floor (survey A) |
|---|---|---|---|---|
| 3.980863 | 80 m | −28 | 45 | 28.6 |
| 10.145584 | 30 m | 4 | −6 | 38.0 |
| 14.126454 | 20 m | −24 | 39 | 18.5 |
| 14.308440 | 20 m | −19 | 31 | 39.0 |
| 21.019122 | 15 m | 28 | −44 | 13.0 |
| 28.980864 | 10 m | −28 | 46 | 17.2 |
| 29.162850 | 10 m | −23 | 38 | 30.9 |
| 29.344837 | 10 m | −18 | 30 | 27.9 |
| 29.526826 | 10 m | −13 | 22 | 36.8 |
| 29.708812 | (above 10 m) | −8 | 14 | 20.4 |

(clk1 as the si5351 makes it, 40.0363975 MHz.) Since 40 MHz and 25 MHz
are both multiples of 5 MHz, a × clk1 + b × 25 MHz is a multiple of
5 MHz plus a × (clk1 − 40 MHz) = a × 36.3975 kHz: each line sits near a
multiple of 5 MHz, offset by a times clk1's 36.4 kHz above 40 MHz —
10.1456 is 10 MHz + 4 × 36.4 kHz, 29.5268 is 30 MHz − 13 × 36.4 kHz.
Which multiples of 5 MHz a given a can reach is fixed too (8a + 5b
must be the multiple), and over a up to ±28 exactly nine of these land
in a ham band: the nine above that are in one. The survey found every
one. It is why 40 and 17 m are clean (no multiple of 5 MHz within a
megahertz) and 20, 30 and 10 m are not. Two of them hadn't been put
down to the si5351 before (14.126454 and 21.019122 MHz). The survey
now names this family as a suspect for a fixed-RF spur.

What makes them isn't known: clk1's harmonics mixing with the TCXO's
somewhere — in the si5351 (PLL B is a fractional multiple of 25 MHz, 18
× clk1), or on the board, where they get in after the LPFs (they stay
with the relays open). At 4 mA they fell by 2.5 dB (10.1456) to 16 dB
(29.5268).

Two ways at them:

- **Their level**: clk1's drive, PLL B's setting (`L CLK1DIV` changes
  PLL B and its fraction, not clk1), or hardware — the clock lines'
  filtering and layout. The divider is a by-ear test today.
- **Where they fall**: only clk1 − 40 MHz decides it, and that is
  `xtal_filter_center` − 40 MHz plus the second IF (+24 kHz now). With
  clk1 on the low side (S − 24 kHz, 11.6 kHz under 40 MHz) the lattice
  draws in to ±0.33 MHz around each multiple of 5 MHz, and the band
  crossings for a up to ±28 fall from nine to three (10.1276, 24.9420
  and 29.6867 MHz), though with different a, whose strength isn't
  known. With clk1 at exactly 40 MHz every one would sit on a multiple
  of 5 MHz, none in a ham band — but that is a 12.4 kHz second IF, and
  with the crystal filter only 11–14 dB down at 48 kHz, signals 15–35 kHz
  below the dial would fold into the passband. Low side is a software
  change (the second IF's sideband), not yet studied.

A test of the family needs no build: move `xtal_filter_center` 1 kHz
(`L XTALCENTER`), which moves clk1 1 kHz, and each line moves a kHz —
10.1456 to 10.1496 MHz, 14.3084 to 14.2894, 29.5268 to 29.5138.

## 3. What removes which

The question was whether spurs are addressed by board layout, better
shielding and supply filters. For the **internally generated** ones,
yes — they are coupling problems, and the remedies are hardware:

- **Shielding** the RF front end (LPFs, first mixer, crystal filter)
  from the Pi and its clocks, and the Pi from the antenna lead. Radiated
  clock harmonics enter wherever the receiver is most sensitive.
- **Supply filtering and decoupling**: a separate, filtered (LC or
  ferrite and LDO) supply for the codec's analog side and the mixers;
  switching regulators with clean layouts, or spread-spectrum ones so
  their harmonics smear below the noise.
- **Layout and grounding**: analog and digital return currents kept
  apart, clock traces short and terminated, the codec's and si5351's
  references quiet.
- **Slower clock edges**: the si5351 runs at 8 mA drive
  (`si5351v2.c`); a lower drive strength cuts its high harmonics. The
  cost is less LO at the mixers, which can mean less conversion gain and
  worse intermodulation, and a different transmit level (the per-band
  power scales were set at 8 mA). `L SIDRIVE` sets 2, 4, 6 or 8 mA until
  restart, so the survey can compare them (§4.6).
- **Frequency planning**: choosing clock frequencies whose harmonics
  miss the bands. Only partly open to us: the codec's 12 MHz and the
  TCXO's 25 MHz are fixed, but a mixing birdie can be moved by
  changing `xtal_filter_center` or the IF.

Software can't remove an internal spur — it is a real signal at the
ADC. It can only avoid it (frequency planning above) or hide it (a
notch or a blanked bin on the display, which also hides anything real
there).

**Spurious responses** are a different matter: they are set by the
design, not the layout — filtering before the mixer (the LPFs, the
crystal filter), the linearity and gain distribution of the mixers and
the ADC (IMD, overload), the oscillators' purity (reciprocal mixing,
mixing birdies) and the frequency plan (where images fall). An
attenuator before the first mixer is the operator's own remedy for IMD.
The codec's timing pattern was neither: a device state, fixed in
software.

## 4. The spur survey tool

### 4.1 Purpose

`tools/spur_survey.py`: with a 50 Ω dummy load on the antenna jack,
find every narrow internal spur in the bands, measure it, and say from
its movement which class it is (§1.2) — so each one can be traced to a
source and its remedy judged. It needs nothing new in maxibitx: `F` and
the I/Q stream, and optionally `L LPF` for a comparison run.

### 4.2 Measurement

- **Steps.** The dial moves in steps of 5 kHz; each step records the
  I/Q spectrum, of which offsets within ±10 kHz of the dial are used
  (the crystal filter's flat top). So every RF frequency is seen at four
  different offsets, from four dial settings.
- **Resolution.** 8192-point FFTs (11.7 Hz bins), Hann-windowed, half
  overlapped, averaged over the dwell: 0.4 s gives about 8 blocks, worth
  about 6 independent ones with the overlap. Averaged that far, noise
  reaches 7 dB above the floor in about one bin in 50 million — about
  0.03 false peaks in a whole survey of 1.4 million bins (at 6 dB it
  would be about 5) — while a steady carrier 7 dB up is found every
  time. A birdie is a
  steady carrier; averaging longer finds weaker ones, at a proportional
  cost in time.
- **Detection.** A bin is a candidate when it stands at least 7 dB above
  the median of the bins within ±1 kHz and is the highest within ±4
  bins, so a strong carrier's window sidelobes aren't counted as spurs
  of their own. Each candidate is kept as (dial, offset, level above the
  floor, width at −6 dB), with its frequency refined by interpolation
  between bins (to a few hertz).
- **Coverage and time.** Default: the bands from 80 m to 10 m, 60 m
  included, each widened by 25 kHz either side — about 4.1 MHz, 825
  steps, about 8 minutes. `--general` covers 1–30 MHz in 10 kHz steps (each frequency
  seen twice), about 30 minutes.

### 4.3 Classification

Each candidate is linked to candidates in the neighbouring steps. Its
offset, as a function of the dial, is fitted with a straight line:
`offset = a + s × dial`. The slope s is the class:

| Slope s | Class | Meaning |
|---|---|---|
| −1 | fixed RF | a signal at one RF frequency, seen at four offsets; reported at that frequency |
| 0 | fixed offset | the same offset at every dial: made after the first mixer; reported as an offset, with the dial range it covers |
| +1 | backward | an image or mirror: moves with the dial |
| other | mixing product | moves at another rate; the slope (−3, +2, …) says which multiple of the LO is involved |
| (one sighting) | unclassified | seen once — appears and disappears; a narrow mixing product, or intermittent |

Lines are found from pairs of sightings up to three steps apart, with
slopes in quarter steps; a sighting belongs to a line when it is within
35 Hz of it (three bins, plus the few hertz by which the si5351's
fractional settings miss each dial). A line needs three sightings from
three dials, whatever its slope — false peaks are so rare at 7 dB
(§4.2) that three aligned by chance don't happen — except where a spur
can't be seen three times: a slope of 2 or more crosses the ±10 kHz
window in two steps, and so does anything in `--general`'s 10 kHz
steps; those need two.

Steep lines (added after the 60 m runs, §2.5). A product of the
si5351's PLLs moves 10–30 times as fast as the dial, so it crosses the
±10 kHz window in under a kilohertz of tuning; at 5 kHz steps it is seen
once. `--range LO HI` surveys a short stretch at any `--step`, and the
steepest slope fitted grows as the step shrinks (20 kHz ÷ step: 200 at
100 Hz). Above a slope of 4 slopes are whole numbers — a PLL product
moves at a multiple of the PLL's divider, less one — and a line is
grouped by where it crosses the dial (offset 0), which a slope error of
one barely moves, then fitted by least squares. Its tolerance grows
with the slope, 35 Hz plus 5 Hz per unit: each PLL's fractional setting
misses by up to 48 Hz (25 MHz ÷ 2^19), about 5 Hz at clk2, and a
product multiplies that. Tested on a simulated radio with a PLL A − PLL
B line made by `si5351v2.c`'s arithmetic: at 100 Hz steps it was found
with slope +15.00, crossing the dial at 5.363786 MHz against 5.363785
predicted.

A steep line needs three sightings. Two sightings at neighbouring dials
fit some whole-number slope within the tolerance about one time in five
by chance, so two unrelated single lines would pass for one; the report
counts such pairs and suggests a smaller step. (Learned from reanalysing
the 60 m 1 kHz-step run, §2.5.)

A step that gets no I/Q, or under half of it, is measured again once.

### 4.4 Suspects

For every fixed-RF spur the report lists known clocks with a harmonic
within the spur's frequency tolerance:

- the codec's 12 MHz (±50 ppm: ±600 Hz at 12 MHz, ±1.2 kHz at 24 MHz),
- the si5351's 25 MHz TCXO (a few ppm),
- the Pi's crystal (54 MHz on the Pi 4, 19.2 MHz on the Zero 2W) and
  its sub-multiples,
- clk1 (`xtal_filter_center` + 24 kHz) and its sub-multiples,
- the codec's bit clock and frame clock multiples (12 MHz and 96 kHz).

For a mixing product up to a slope of 4, the report names the harmonic
of clk2 the slope implies. For a steeper one it names the products
±(m × PLL A − n × PLL B + k × 25 MHz), m up to 3, n up to 3 and |k| up
to 12, whose slope (± m × PLL A's divider − 1) matches the fitted one and
which cross the dial within 3 kHz of where the line does — computed
with `si5351v2.c`'s own choice of divider and fractional setting, and
the run's `xtal_filter_center`. These are suspects, not proof; the
comparison runs (§4.6) are what prove a source.

### 4.5 Output

- A printed table per class: frequency (or offset), level above the
  floor in dB and in dBFS, how many sightings, the bands it falls in,
  and its suspects.
- `PREFIX_peaks.csv` (every candidate), `PREFIX_spurs.csv` (the
  classified list) and `PREFIX.png`: per band, the spectrum with the
  dummy load in RF coordinates (max-hold over the steps), spurs marked
  by class.
- `--compare OTHER_spurs.csv`: which spurs appear, vanish or change
  level between two runs.

### 4.6 Runs that locate a source

One run finds the spurs; comparison runs say where each comes from:

1. **Dummy load** — the baseline.
2. **A lower si5351 drive** (`--drive 4`, or 2): spurs from its clocks'
   harmonics, and the products they make, should drop. Before keeping a
   lower drive, check what it costs: a real signal's level at each drive
   (a steady station, or `lpf_sweep.py`'s LPF_A level — the dummy load's
   floor is the receiver's own noise and won't show a loss of gain),
   and transmit power on a wattmeter.
3. **Every LPF relay open** (`L LPF 5` for the run): spurs that vanish
   come in through the antenna path (or are picked up by the dummy
   load's own lead); spurs that stay are made or coupled on the board.
4. **The Pi quieter**: HDMI off, Wi-Fi and Bluetooth off, USB devices
   unplugged, the panel not running. Spurs that vanish belong to those.
5. **A different `xtal_filter_center`** (by 2 kHz): fixed-RF spurs stay;
   mixing products move by a predictable amount.
6. **clk1 on another si5351 divider** (`--clk1-div 22`): the PLL A −
   PLL B products move to other dials (§2.5); others stay. Steep lines
   need `--range` runs at small steps to compare.
7. **Another supply**: a linear bench supply in place of the usual one.
   Spurs from the supply's switching move or vanish.

### 4.7 Limits

- A dummy load finds internally generated spurs only. Images, IMD,
  aliasing and comb copies need a real signal (§5).
- The dummy load's coax and the radio's case are not perfect shields: a
  spur radiated by the Pi into the lead is real (an antenna would pick
  it up too), but its level depends on how the lead is dressed. Keep
  it short and the same between runs.
- Sensitivity is set by the dwell: 7 dB above the floor in an 11.7 Hz
  bin at 0.4 s. A weaker birdie is below the band noise any
  antenna brings, except on the quietest high bands.
- Levels are in dBFS and dB above the floor, not microvolts; the
  receiver's gain isn't calibrated to the antenna port (see
  `rx_gain_and_level_calibration.md`).
- A product with a slope of 4 or more crosses the window between two
  steps and is seen once; at 5 kHz steps the strongest artifacts of the
  first runs went unclassified (§2.5). A smaller `--step` on one band
  catches them.
- Opening every LPF relay cuts most of the antenna path, not all of it:
  a strong broadcast carrier still shows (§2.5).
- A spur's level can change between runs by several dB for reasons of
  its own (§2.5, the comb): compare runs made close together, and
  repeat the baseline when it matters.

### 4.8 Testing

As for `lpf_sweep.py`: a simulated maxibitx (rigctld and the I/Q
stream) with spurs of each class injected — fixed RF, fixed offset,
backward, slope −3, and one that appears at a single dial — over a
noise floor, checking that each is found once, in its class, at the
right frequency and level, and that nothing is reported from noise
alone.

### 4.9 Which are heard

`--audible DUMMY_spurs.csv ANTENNA_bands.csv` judges a dummy-load run's
spurs against the antenna's noise, from another run's `_bands.csv` made
with the antenna connected. For each spur it works out the dials where
it lands in a passband — the CW filter, `--cw-width` (300 Hz) wide
around the pitch, so offsets within ±150 Hz of the dial in CW and CWR;
SSB 300–3000 Hz, LSB below 10 MHz, USB above and on 60 m, none on 30 m
— from its line (offset = slope × (dial − crossing)) within the dials it
was seen from, and its margin: its level (dBFS) less the band's antenna
floor taken in `--ear-hz` (50 Hz). 50 Hz is about the bandwidth the ear
picks a steady tone out of noise with, so a tone at 0 dB is about at
the edge of hearing, even though it is 17 dB under the noise in the
whole 2.7 kHz. Spurs at 0 dB or more are listed worst first, then those
within 6 dB under, 60 m last; a fixed offset within 100 Hz of a
passband edge is marked, since the filter is rolling off there. Fixed-RF
spurs with no suspects (runs before the comb was added) get the clock
suspects worked out again.

The antenna's noise is one afternoon's: it rises when a band opens and
at night on the low bands. A quiet band is the worst case.

## 5. Other sources

**The noise generator** is for shapes, not spurs: it raises the floor
and hides weak birdies, and an image of noise is only more noise. It is
the right source for the LPFs (`lpf_sweep.py`), the crystal filter
(`xtal_sweep.py`), the 8 kHz ripple (§2.3) and the depth of LPF_B's
17 m roll-off (8).

**A clean carrier** finds spurious responses: sweep it slowly across a
band with the dial still, and anything that answers anywhere but where
the carrier is — the second-mixer image 48 kHz below, an alias at the
edges, a harmonic response — is a spurious response, measured against
the carrier itself. A cheap signal generator does; so does the zBitx at
its lowest power through 60 dB or more of attenuation. WWV and strong
broadcast carriers are free single-frequency checks. A later tool
(`response_survey.py`) could automate it; it is out of scope here.

**Two carriers** at once, through a combiner, give IMD: the classic
two-tone test, raising both until third-order products appear.

## 6. Decisions

Taken (29 October 2026):

1. `spur_survey.py` built as §4 describes, with the si5351's drive as
   one of the comparisons (`--drive`, rigctld's `L SIDRIVE`).
2. Default coverage: the ham bands only; 1–30 MHz with `--general`.
3. Detection threshold 7 dB and dwell 0.4 s.
4. The dummy-load survey is in `08_troubleshooting_and_bringup.md`,
   "The receiver's own spurs".
5. The aim from here: the spurs heard by ear over the antenna's noise,
   working CW and SSB; 60 m after every other band (§2.6).
6. `--audible` built to list them (§4.9), with 50 Hz as the bandwidth
   the antenna's noise is taken in. Checked by ear: the loudest four are
   S2–S4; the +300 Hz offset isn't a problem.

Still open, the heard ones first:

- The clk1 lattice (§2.7), the loudest heard (S4 at 29.5268 and
  10.1456 MHz, S2 at 14.3084): by ear, `L XTALCENTER` +1 kHz to confirm
  the family, then `L CLK1DIV` 16/20/22 and `L SIDRIVE` for their level;
  then a study of clk1 on the low side.
- The 48 kHz comb (11), heard at S2 on 10 m: its source (below).
- From §2: check the block-rate comb on SSB transmit (5); measure the
  ripple (6) and LPF_B (8) with the noise generator.
- The 48 kHz comb's source (11): measure the codec's sample rate
  against the TCXO; a run with the Pi's HDMI, Wi-Fi and USB off.
- The cost of a lower si5351 drive (12): a steady signal's level at 8,
  6 and 4 mA. Keeping a lower drive would then want a setting in
  `hw_settings.ini` and the transmit power scales re-checked.
- The crystal filter 48 kHz from its centre (13): a carrier 48 kHz
  below the dial.
- The single-dial lines (14), lower priority (60 m): 60 m's has PLL A − PLL B's frequency
  but stayed with PLL B moved (clk1 divider 22, §2.5): PLL A or clk2's
  16th harmonic, with clk1's 18th; only 3 dB weaker at 4 mA, which
  leans towards PLL A. Next: an `L CLK2DIV` to move PLL A on 60 m; suspects that name products of
  the clocks' harmonics, a beat's fold and its harmonics. Also: 100
  Hz-step runs across other predicted crossings; the −12, −40 and +79
  lines; the mirrored pair seen at divider 22.
