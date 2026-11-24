# CW keying sidebands and ARRL's Clean Signal Initiative

Status: implemented 2 November 2026. `cw.c` keys with DL1YCF's 9 ms ramp,
`tx_pipeline.c` drives CW at the power ceiling instead of limiting it,
`make test-cw-shape` checks the transmitted spectrum against the mask on
the bench, and `tools/cw_shape.py` measures a CW signal off the air; §8
is a worked example on W1AW. Not yet measured on the air: the sBitx's
real RF, through its power amplifier (§7).

## 1. The goal

ARRL's Clean Signal Initiative (CSI) sets benchmarks for transmitted
signal quality, and the ARRL lab now tests new radios against them. CW is
one of the benchmarks: how far a keyed signal's sidebands, the key clicks,
spread from the carrier. The panel's finer spectrum (`rigctl_panel.py`'s
longer FFT) made the difference between stations easy to see, and raised
the question of where maxibitx stands.

The goal of this change is for maxibitx's CW to meet the CSI mask at the
speed ARRL tests, 40 WPM, at every power setting, and to be able to show
it: on the bench, every time the tests run, and off the air, on a real
signal.

## 2. The CSI mask

From ARRL's May 2024 *QST* article (reference 1), the CW mask has three
points:

| Mask column "Attenuation Bandwidth" | Level |
| --- | --- |
| 180 Hz | −20 dBc |
| 300 Hz | −40 dBc |
| 675 Hz | −60 dBc |

The article sums it up as clicks "down to at least 60 dB with a bandwidth
of 675 Hz or less". From a summary of the April 2026 launch article
(reference 3): the CW test is made at 40 WPM, with QSK on, at **every
rise-time setting the radio offers**, and a radio earns a Gold badge for
meeting all three CSI benchmarks, Silver for two and Bronze for one. The
April 2026 article itself (reference 2) could not be read for this note,
and neither the resolution bandwidth nor the detector ARRL uses is stated
in what was read.

**Two readings.** The figures can be read as offsets from the carrier
(−60 dBc 675 Hz away) or as total bandwidths (−60 dBc 338 Hz either side).
The articles' own words, "bandwidth" and "Attenuation Bandwidth", point to
the second, and it is the one DL1YCF uses (§3). This note and both tools
use it, the stricter of the two:

| Beyond, either side of the carrier | Level |
| --- | --- |
| 90 Hz | −20 dBc |
| 150 Hz | −40 dBc |
| 338 Hz | −60 dBc |

A transmitter that meets it meets the offset reading with room to spare.

## 3. DL1YCF's ramp

DL1YCF (Christoph van Wüllen, author of piHPSDR) designed a CW ramp for
this mask (reference 4, `src/transmitter.c`, `init_dl1ycf_ramp()`). It
rises from 0 to 1 over the ramp's width as a straight line plus five sine
terms, y = 0 to 1 along the ramp:

```
ramp(y) = y − 0.12182865361171612 sin(2πy) − 0.018557469249199286 sin(4πy)
            − 0.0009378783245428506 sin(6πy) + 0.0008567571519403228 sin(8πy)
            + 0.00018706912431472442 sin(10πy)
```

He optimised the coefficients against the spectrum of a string of dots,
"the maximum peak strength at frequencies with a distance to the carrier
that is larger than an offset", for the mask as read in §2, plus three
tighter limits of his own: −80 dBc beyond 600 Hz, −100 beyond 900 and −120
beyond 1200. The width he gives depends on speed: 7 ms for 5–15 WPM, 8 ms
for 16–32 and 9 ms for 33–40. piHPSDR's default is 9 ms. It starts from
VE3NEA's ramp, the integral of a 4-term Blackman-Harris window.

## 4. The ramp alone

A string of dots at 40 WPM, shaped by each ramp with nothing else in the
path: the strongest line beyond each offset, in dBc. A bold figure misses
its limit.

| Ramp | >90 Hz (−20) | >150 (−40) | >338 (−60) | >600 (−80) | >900 (−100) | >1200 (−120) |
| --- | --- | --- | --- | --- | --- | --- |
| maxibitx until now: rising half of a Blackman-Harris window, 5 ms | −23.6 | **−30.2** | **−49.4** | **−64.7** | **−77.0** | **−82.9** |
| the same shape stretched to 9 ms | −25.5 | **−36.1** | −60.5 | **−77.1** | **−86.3** | **−92.8** |
| VE3NEA, integrated Blackman-Harris, 9 ms | −24.5 | **−32.5** | −70.9 | −129.8 | −134.0 | −143.6 |
| DL1YCF, 9 ms | −30.2 | −45.1 | −61.7 | −99.2 | −111.5 | −123.3 |

Only DL1YCF's meets every limit, as he says it does. The shape matters as
much as the length: the old shape stretched to 9 ms still misses at
150 Hz by 4 dB. Under the offset reading of §2, the old 5 ms ramp passed,
with 3.5 dB to spare at 300 Hz; that reading, used before this note, made
the old ramp look adequate.

## 5. What the radio sends

`make test-cw-shape` (`src/cw_shape_test.c`) measures the radio's own
signal rather than a ramp on its own: a dot paddle held on the iambic
keyer, keyed through `key_input.c`, `keyer.c` and `cw.c` one sample at a
time as `sound.c` does, then through `tx_pipeline.c` — its 300–3000 Hz
filter, sideband step, shift to the IF and limiter — to the IF the codec
is given. The spectrum is taken over 2.7 s with a 7-term Blackman-Harris
window and normalised to the carrier.

At 40 WPM, before and after:

| | >90 Hz (−20) | >150 (−40) | >338 (−60) |
| --- | --- | --- | --- |
| before, full power | −23.1 | **−26.4** | **−49.7** |
| before, 20 dB below full power | −22.8 | **−26.0** | **−49.0** |
| DL1YCF ramp, CW still limited, full power | −30.6 | −42.9 | −61.8 |
| DL1YCF ramp, CW still limited, 20 dB below | −32.1 | **−39.2** | **−55.4** |
| as built, full power | −30.6 | −42.9 | −61.8 |
| as built, 10 and 20 dB below | −30.6 | −42.9 | −61.8 |

**The limiter.** Until now, reduced POWER lowered the limiter's ceiling
and let the limiter pull the unit-amplitude CW down to it. The limiter's
gain recovers during each space and has to come down again, over its
2 ms attack, on every rise, so it reshapes every element. With the old
ramp that hid behind the ramp's own sidebands; with DL1YCF's it cost
almost 5 dB at 338 Hz at 20 dB below full power, enough to miss the
mask. Now `tx_pipeline.c` scales CW's input by the ceiling, on the drive
side of the limiter (`docs/00_intro.md` §7, "Setting
power"), so a key-down carrier sits exactly at the ceiling and the
limiter never acts on CW; the spectrum is the same at every power. The
drive follows a POWER change over 20 ms, so moving the slider during an
element doesn't click, and while it falls the limiter still holds the
ceiling.

**Reading the numbers.**

- At 40 WPM the dot rate puts a line at exactly 150.0 Hz. The test counts
  it as beyond 150 Hz, the cautious choice; without it the figure is
  −45.3 dBc, matching §4. The margins as built are 10.6, 2.9 and 1.8 dB.
- Beyond 600 Hz every figure is −86.8 dBc: one line at +1397.5 Hz, the
  second harmonic of `vfo.c`'s tone oscillator. It is far below the CSI
  mask, but above DL1YCF's −100 and −120 dBc. A cleaner oscillator could
  remove it; the codec's and the power amplifier's own floors are likely
  higher anyway.
- At 20 WPM the margins are 10.3, 11.4 and 7.5 dB. At 60 WPM the 338 Hz
  point misses by 2 dB (−58.0 dBc): a dot there is 20 ms, almost all
  ramp, and DL1YCF's widths stop at 40 WPM. ARRL tests at 40.

## 6. Decisions

- **9 ms at every speed** (Mike, 2 November 2026). ARRL tests at every
  rise-time setting a radio offers, so one fixed ramp that passes at
  40 WPM is the safe choice. DL1YCF's 7 and 8 ms for slower speeds would
  sound a little crisper and pass too, but would be more settings for
  ARRL to test.
- **The old ramp is replaced, not kept as an option**, for the same
  reason.
- **CW is driven at the ceiling**, not limited to it (§5).

What the 9 ms ramp costs:

- Each element reaches 50% 4.5 ms after key-down, where the old ramp took
  3.3 ms. The 1:1 weighting needs no correction: the ramp crosses 50%
  halfway up whichever way it is read, so the old 150-sample hold on each
  fall is now 0.
- A mark shorter than 9 ms never reaches full output and comes out
  shorter. A dot is 9 ms at 133 WPM; only a very quick tap on a straight
  key is affected.
- The sidetone is the same signal and has the same softer edges.

## 7. Checking it

**On the bench**, `make test` runs `test-cw-shape` with the rest:

```
make test-cw-shape && ./test-cw-shape
```

It fails if the 40 WPM signal misses the mask at full power or at 10 or
20 dB below.

**Off the air**, `tools/cw_shape.py` measures any CW signal maxibitx can
hear, from the I/Q stream:

```
python3 tools/cw_shape.py --host sbitx.local               # strongest signal near the dial
python3 tools/cw_shape.py --host sbitx.local --offset 420 --plot sig.png
```

It reports, for each mask point on each side, the strongest level and
where it is, with a verdict:

- PASS when it is below the limit;
- "can't tell" when it is within 6 dB of the noise, since a sideband and
  the noise in the same bin add;
- "another signal?" when something stronger than −20 dBc lies past
  338 Hz, which is most likely a neighbouring station;
- FAIL otherwise.

Before that, it looks for other stations by symmetry. Keying sidebands
come in mirrored pairs, so a peak beyond 150 Hz that stands 10 dB above
the same offset on the carrier's other side, and 6 dB above the noise,
is another station whatever its level: it is named and left out of the
mask. The carrier the levels are referred to is the strongest peak
within 150 Hz, not the strongest in view, which can be a neighbour.

It also reports:

- the carrier's signal-to-noise and how far down the noise lets it see;
- speed and weighting, from an envelope within 100 Hz of the carrier. A
  mark starts at 60% and ends at 40% of the key-down level, so a fade
  doesn't split it, and the dot length is found by clustering the marks'
  lengths, so a stray fragment isn't taken for a dot;
- where the sidebands come from: 43 ms frames inside long elements (kept
  10 ms clear of the edges), on the edges, and in the gaps, compared by
  their medians at 150, 338 and 600 Hz. Keying sidebands are only on the
  edges; phase noise or hum, the transmitter's or the receiver's, is
  there in steady key-down too;
- rise and fall times, 10–90%, and chirp, from the widest envelope (up to
  1200 Hz) that is 26 dB above the noise and clear of other stations,
  with that filter's own rise time beside them, or left out when the
  signal is too weak.

Its spectrum is held at its peak over frames of 5.9 Hz bins, so its
numbers are close to the bench test's but not the same measurement, and
not ARRL's. Fading and noise limit what an off-air signal can show. In
the simulation, the 338 Hz point needed the carrier about 65 dB above
the noise in 2.5 kHz to read PASS (−60 to −61 dBc); at 55 and 60 dB it
said "can't tell".

`--simulate` measures a synthetic signal instead, to check the tool and to
show the difference this change makes:

```
python3 tools/cw_shape.py --simulate bh5 --plot before.png       # the old ramp
python3 tools/cw_shape.py --simulate dl1ycf --plot after.png     # cw.c now
```

At 40 WPM and 90 dB SNR these read −23.0, −27.0 and −49.6 dBc for the old
ramp and −30.5, −43.4 and −62.4 dBc for the new one, within a dB of the
bench test.

**The sBitx's own RF** is what ARRL measures, after the codec, the
mixer and the power amplifier, and it is still to be measured. The sBitx
can't hear itself, since receive is muted in transmit. The plan:

1. Key the sBitx into a dummy load, at 40 WPM dots from the keyer.
2. Couple a small sample of its output into the zBitx running maxibitx,
   enough for a strong signal but well below overload. `cw_shape.py`
   prints the stream's peak; keep it below −6 dBFS, or the receiver's own
   distortion shows as splatter.
3. Run `cw_shape.py` against the zBitx, at full power and at reduced
   POWER.

A power amplifier that compresses, or a supply that sags, reshapes the
envelope and shows here as wider sidebands than §5's. The same
measurement on other stations' signals, from the panel's spectrum, is a
way to compare. §8 does that for one.

## 8. A worked example: W1AW off the air

W1AW was strong on 23 November 2026, and is a station many operators
know, so it made a good first real signal for `cw_shape.py`. Mike ran
the tool four times between about 15:40 and 16:15 local time. The first
run, before the changes below, wasn't saved; the other three were, and
the figures here are those recordings replayed with the tool as it now
stands (`--replay`).

### What it measured

The worst level beyond each mask point, either side of the carrier, in
dBc. A bold figure fails; "?" is "can't tell", within 6 dB of the
noise.

| Run | SNR in 2.5 kHz | Noise reaches | >90 Hz (−20) | >150 Hz (−40) | >338 Hz (−60) | Speed | Rise / fall, 10–90% |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 15:54 | 31.3 dB | −44.1 dBc | −26.9 | **−30.6** | −38.7 ? | 17.2 WPM | 3.29 / 2.68 ms |
| 16:02 | 41.2 dB | −51.2 dBc | −27.8 | **−34.1** | **−41.9** | 16.3 WPM | 2.28 / 2.23 ms |
| 16:13 | 34.3 dB | −46.5 dBc | −31.5 | **−37.5** | −44.4 ? | 16.3 WPM | 2.55 / 2.42 ms |

- **90 Hz passes** in every run, by 7 to 12 dB.
- **150 Hz fails** in every run, on both sides, by 2.5 to 9.4 dB, and
  each failing figure stands about 9 to 17 dB above the noise, so it is
  the signal and not the noise.
- **338 Hz fails by 18 dB** in the one run strong enough to see it
  (16:02, 9 dB above the noise there). In the other two the noise is
  higher than anything a clean signal would show, so the tool can't tell.
- Nothing beyond 600 Hz stands clear of the noise in any run.

The tool named and set aside two other stations, at −1494 Hz and
+885 Hz, each with nothing like it on the other side of the carrier.
Chirp was within a hertz.

### Keying, not phase noise

The steady-against-edges check at 150, 338 and 600 Hz. Its figures are
medians of 43 ms frames in 23 Hz bins, a different measure from the
peak-held mask figures above, so they compare with each other, not with
the mask:

| Run | | 150 Hz | 338 Hz | 600 Hz |
| --- | --- | --- | --- | --- |
| 15:54 | steady / edges / gaps | −47.9 / **−26.1** / −48.9 | −48.8 / −45.9 / −47.4 | −47.8 / −47.4 / −49.3 |
| 16:02 | steady / edges / gaps | −52.4 / **−25.8** / −58.6 | −58.8 / **−49.2** / −58.2 | −58.3 / −55.0 / −59.0 |
| 16:13 | steady / edges / gaps | −49.8 / **−25.6** / −52.6 | −52.4 / **−47.6** / −52.8 | −53.0 / −51.4 / −53.0 |

At 150 Hz the edges are 22 to 27 dB above steady key-down, and steady
key-down is within 1 to 6 dB of the gaps. At 338 Hz the edges stand
10 dB (16:02) and 5 dB (16:13) above steady key-down, which matches the
gaps. The sidebands are on the edges only: they are W1AW's keying, not
phase noise or hum at either end, and not this receiver.

### The edge

Averaging each run's edges (elements over 35 ms long, aligned on their
50% point, the median in the envelope the tool timed them in: 600 Hz,
or 1200 Hz at 16:13) gives the strongest run's rise as:

- 2% to 85% of full in about 3 ms (−1.5 to +1.5 ms about the 50% point);
- then a slow creep: 86% at +2 ms, 93% at +8 ms, 95–96% from +12 ms on.

Its fall goes from 90% to 2% in under 4 ms, with no tail. So the shape
is a fast edge with a corner at the top, and the corner, more than the
2–3 ms rise, is the likely source of the energy at 338 Hz. The 16:13
run shows the same fast edge, reaching full in about 8 ms. The 15:54
run's elements were fading, and its averaged rise never settles above
90%; its 150 Hz figure, the worst of the three, is likely inflated by
that.

For scale, the tool's own synthetic signals at W1AW's speed (16.3 WPM,
90 dB SNR):

| Ramp | Rise, 10–90% | >150 Hz | >338 Hz |
| --- | --- | --- | --- |
| maxibitx's old 5 ms Blackman-Harris ramp | 2.34 ms | −37.4 | −57.1 |
| DL1YCF's 9 ms ramp, `cw.c` now | 4.94 ms | −54.7 | −72.5 |
| W1AW, 16:02 | 2.28 ms | −34.1 | −41.9 |

W1AW's rise time is about that of the ramp maxibitx replaced, and its
150 Hz figures are within 3 dB of it. Its 338 Hz figure is 15 dB worse,
which fits the corner at the top of its edge. DL1YCF's ramp
passes at this speed with about 15 and 12 dB to spare.

### How far to trust it

- **It is one off-air path.** Fading moves the level of a whole element
  over tens of milliseconds, which puts energy within a few hertz of the
  carrier, not at 150 Hz, and multipath smears an edge, which would make
  it look softer, not harder. Neither explains a 150 Hz failure. A fade
  during an edge can still raise a figure, as in the 15:54 run.
- **It isn't ARRL's measurement.** The tool holds 5.9 Hz bins at their
  peak; ARRL's resolution bandwidth and detector are not stated in what
  was read (§2). The comparison with the synthetic ramps uses the same
  tool, so that part holds whatever the measurement's offset.
- **It is at 16–17 WPM, not ARRL's 40.** At 40 WPM there are more than
  twice as many edges a second, so the same edges would put more energy
  into the sidebands, not less.
- **The noise limits what it can show.** The peak-held noise sits at
  about −(SNR + 10 to 13) dBc, with SNR the carrier's in 2.5 kHz. So
  seeing −60 dBc at all takes about 50 dB SNR, and seeing a clean
  signal's sidebands below the mask, with room for a PASS, 60 to 65 dB. W1AW at
  31 to 41 dB could show a failure at 338 Hz only where it was gross,
  and in one run it was.

So, as received here, W1AW's CW misses the CSI mask at 150 Hz in every
run, and at 338 Hz by 18 dB where the signal was strong enough to tell.
The sidebands are its keying edges.

### What it taught the tool

The first runs found three faults in `cw_shape.py`, each fixed before
the figures above were taken:

- **Neighbours.** Its first rule took a valley between the carrier and
  a peak as the sign of another station, which also flagged a clean
  signal's own sidelobes. Another station is now a peak with no mirror
  image on the carrier's other side, and levels are referred to the
  carrier within 150 Hz rather than to the strongest peak in view.
- **Speed.** A fading signal read 248 WPM: noise hovering about the 50%
  point split elements into fragments a few milliseconds long, and the
  shortest was taken for a dot. Hysteresis (60% on, 40% off) and finding
  the dot by clustering the marks' lengths read the same recording as
  17.2 WPM.
- **Where the sidebands come from.** Nothing told keying sidebands from
  phase noise or the receiver's own floor; the steady-against-edges check
  was added for that, using medians so one stray frame can't skew it.

## References

1. ARRL, "ARRL's Clean Signal Initiative", *QST*, May 2024:
   <https://www.arrl.org/files/file/QST/This%20Month%20in%20QST/2024/05%20may%2024/05-2024%20CSI.pdf>
2. ARRL, "The Launch of the Clean Signal Initiative", *QST*, April 2026
   (not read for this note):
   <http://www.arrl.org/files/file/QST%20Binaries/April%202026/Spatta%20Clean%20Signal%20Initiative.pdf>
3. "Understanding ARRL's Clean Signal Initiative" (a summary of the
   launch article's benchmarks and badges):
   <https://hamradio.my/understanding-arrls-clean-signal-initiative-what-it-means-for-every-hf-operator/>
4. DL1YCF, piHPSDR, `src/transmitter.c`: <https://github.com/dl1ycf/pihpsdr>
5. ARRL Clean Signal Initiative: <https://www.arrl.org/arrl-clean-signal-initiative>
