# CW keying sidebands and ARRL's Clean Signal Initiative

Status: implemented 2 November 2026. `cw.c` keys with DL1YCF's 9 ms ramp,
`tx_pipeline.c` drives CW at the power ceiling instead of limiting it,
`make test-cw-shape` checks the transmitted spectrum against the mask on
the bench, and `tools/cw_shape.py` measures a CW signal off the air. Not
yet measured on the air: the sBitx's real RF, through its power amplifier
(§7).

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
side of the limiter (`docs/03_tx_processing_pipeline.md`, "Setting
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

It reports the keying sidebands beyond each mask point on each side, with
PASS, FAIL or "can't tell" when what rises above the limit is only the
noise; rise and fall times, 10–90%; speed and weighting; and chirp. Its
spectrum is held at its peak over frames of 5.9 Hz bins, so its numbers
are close to the bench test's but not the same measurement, and not
ARRL's. Fading and noise limit what an off-air signal can show. In the
simulation, the 338 Hz point needed the carrier about 55 dB above the
noise in 2.5 kHz: at 55 dB it read −62 dBc, at 50 dB "can't tell".

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
way to compare.

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
