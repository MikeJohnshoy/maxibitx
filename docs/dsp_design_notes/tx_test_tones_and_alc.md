# TX test tones, carrier-offset check, and ALC

Status: in progress. Step 1 (the test-tone generator) is built and
bench-tested, and step 2 (the carrier-offset check and fix) is measured
and fixed, pending an on-air re-check. Steps 3-5 are proposed.

## Why

Three open transmit questions need a known, repeatable test signal:

- **The SSB carrier offset.** A bench model of `tx_pipeline.c` plus the
  analog mixers predicts that USB, LSB and DIGITAL put the suppressed
  carrier about 700 Hz off the dial
  ([`../03_tx_processing_pipeline.md`](../03_tx_processing_pipeline.md),
  "Known limitations"). On-air SSB tests so far used a mic and a whistle,
  which showed the correct side of the dial but not the exact position.
- **Voice and DIGITAL power.** CW is calibrated on a wattmeter (a steady
  tone), but speech and FT8 audio have different peak-to-average ratios,
  and nothing limits their peaks.
- **Linearity.** How hard the PA can be driven before its
  intermodulation products (IMD) spread outside the channel hasn't been
  measured.

A mic can't answer any of these precisely. A generated tone can.

## The plan

1. **Test-tone generator** (done). A TX audio source that replaces the
   mode's normal source (key tone, mic or USB audio) while it's on.
2. **Carrier-offset check and fix** (measured and fixed; see below).
3. **Two-tone calibration.** Measure average power per band and look at
   the IMD products on a remote SDR's spectrum, to find the highest
   clean peak envelope power (PEP).
4. **Digital ALC.** A peak limiter on the transmit envelope inside
   `tx_pipeline.c`, with its ceiling set from step 3. Because output
   power in this design is set only by the DAC amplitude, which is
   already calibrated per band, the limiter can hold PEP without a
   feedback loop. Exposed as Hamlib's standard `RFPOWER` (the ceiling)
   and `ALC` (gain reduction) levels.
5. **Closed-loop trim on sBitx v2.** v2 boards have a forward/reflected
   power bridge that maxibitx doesn't read yet. A slow loop on it could
   trim for temperature and supply drift, protect against high SWR, and
   provide transmit metering. It sits on top of step 4, not instead of it.

The order matters: each step gives the next a trustworthy measurement.

## The generator (step 1)

**Where it lives.** `tone_gen.c`/`.h`, an audio source like `cw.c`, not
part of `tx_pipeline.c`. `tx_pipeline.c` stays a pure processor (audio
in, IF out) that knows nothing about modes or PTT; that is what lets
`tx_pipeline_test.c` check it without hardware. Putting the source inside
the pipeline would also hide it inside the thing it's meant to test.
`sound.c`'s transmit branch already chooses one source per mode, so the
generator is one more branch there, taking priority while it's on.

**Signals.**

| Mode | Tones | Amplitude | Peak | Average power vs. CW |
|---|---|---|---|---|
| Single | 1000 Hz | 1.0 | 1.0 | Same as CW |
| Two-tone | 700 + 1900 Hz | 0.5 each | 1.0 | Half of CW |

Both peaks equal full scale, the same as `cw.c`'s key-down tone. Since
`tx_pipeline.c` has unity gain for tones in its passband and CW is
calibrated at about 5 W, both signals should reach the same ~5 W PEP.
The single tone should read about 5 W on an averaging wattmeter, and the
two-tone about 2.5 W, since two equal tones average half their peak
power. If the two-tone reads noticeably different from half the single
tone, the analog chain isn't linear at that drive level.

700 and 1900 Hz are a common two-tone pair. Their third-order IMD
products (2f₁ − f₂ and 2f₂ − f₁) land at −500 and +3100 Hz from the
carrier, clear of both tones and easy to find on a spectrum display.

The tones use double-precision phase accumulators and `sin()`, not
`vfo.c`'s table oscillator, so the generator's own spurious content sits
far below anything the PA produces.

**Keying.** The generator doesn't key the radio itself. With it on, any
PTT source transmits the tones: rigctld `T 1`, Kenwood `TX;`, HPSDR MOX,
or the key/mic PTT line. This also gives remote PTT something to send in
CW, USB and LSB, which otherwise transmit silence. The sideband follows
the mode: LSB keeps the lower sideband, everything else the upper.

**Safety timeout.** If the radio stays in transmit with the generator on
for 30 seconds (`TONE_GEN_TIMEOUT_S`), `maxibitx.c`'s once-a-second idle
loop turns the generator off, drops PTT (unless the local key is
holding it), and logs it. The check runs in the idle loop rather than
the audio thread, so logging and the PTT change stay off the real-time
path.

**Control.** rigctld `u TONE` / `U TONE <0|1|2>` (off, single, two-tone),
an extension in the same style as `NARROW`, and a "TX Test" section in
`tools/rigctl_panel.py` with the tone selector and a transmit toggle.

**Bench results.** `tx_pipeline_test.c` Case E runs `tone_gen.c`'s
output through the real pipeline (upper sideband) and measures it with a
Hann-windowed DFT:

| Check | Result | Expected |
|---|---|---|
| Single 1000 Hz tone at its IF | −0.00 dB | 0 dB (full scale) |
| Two-tone, each tone | −6.02 / −6.02 dB | −6.02 dB (0.5 each) |
| Two-tone peak vs single-tone peak | −0.00 dB | 0 dB (same PEP) |
| IMD3 at carrier −500 / +3100 Hz | −136 / −158 dB | numerical floor |

The pipeline adds no measurable intermodulation, so any IMD seen on the
air comes from the analog chain and PA. (A rectangular-window
measurement over the same 16 blocks reads about −62 dB at the IMD
frequencies - leakage from the tones 1200 Hz away, not real IMD - which
is why Case E windows.) Re-run with
`make test-tx-pipeline && ./test-tx-pipeline`.

## The carrier offset (step 2)

**Measured, 2026-09-22.** Dial 7,218,000 Hz, single 1000 Hz tone, read
on a remote receiver:

| Mode | Measured | Model | Should be |
|---|---|---|---|
| USB | dial + 280 Hz | dial + 291 Hz | dial + 1000 Hz |
| LSB | dial − 317 Hz | dial − 303 Hz | dial − 1000 Hz |

The offset is confirmed, and both readings sit within ~14 Hz of the
model - close enough to treat the bench model of the mixer chain as
correct.

**The cause.** Both rotations were derived to land a `CW_PITCH_HZ` tone
on the dial. That is what CW wants (a key-down carrier on frequency),
but a sideband mode's audio is referenced to its *suppressed carrier*,
i.e. audio 0 Hz. Anchoring a 700 Hz tone instead puts the carrier 700 Hz
off in either direction.

**The fix.** `tx_pipeline.h` now has two placements, selected by an
`enum tx_pipeline_signal` (CW, USB, LSB) that replaced the old
upper/lower sideband argument:

| Signal | Anchor | Ideal shift | Bins | Actual | Carrier |
|---|---|---|---|---|---|
| CW | 700 Hz tone on the dial | 21,900 Hz | 467 | 21,890.6 Hz | dial − 9.4 Hz |
| USB, LSB, DIGITAL | carrier on the dial | 22,600 Hz | 482 | 22,593.8 Hz | dial − 6.3 Hz |

Both sidebands share the SSB shift: USB rotates its kept positive half
onto the carrier point and LSB its kept negative half, each band running
from that same point in its own direction. The separate mirrored LSB
shift the earlier tone-anchored derivation needed is gone.

`tx_pipeline_test.c` Case D now checks LSB placement directly: a 1000 Hz
tone reads 0 dB at the carrier − 1000 Hz and −122 dB at carrier + 1000
Hz, which is what distinguishes LSB from USB.

**Confirmed on air, 2026-09-22.** Dial 7,219,000 Hz, 1000 Hz tone:
USB read 7,219,986 Hz and LSB 7,217,981 Hz, against 7,219,993.75 and
7,217,993.75 predicted. Sideband placement is correct; what's left is a
few Hz of bin quantization plus the reference error below.

## Frequency calibration (the si5351 reference)

Every measurement so far read low by about the same fraction:

| Date | Mode | Error vs. predicted | Implied reference |
|---|---|---|---|
| 09-22 (before the fix) | USB | −10.6 Hz | 24,999,963 Hz |
| 09-22 (before the fix) | LSB | −13.9 Hz | 24,999,952 Hz |
| 09-22 (after the fix) | USB | −7.8 Hz | 24,999,973 Hz |
| 09-22 (after the fix) | LSB | −12.8 Hz | 24,999,956 Hz |

An error in the si5351's reference oscillator scales every clock, so the
transmitted carrier moves by `(T/C − 1)(clk2 − bfo_freq)`, where `T` is
the real reference, `C` the value the code assumes, and
`clk2 − bfo_freq = f − 22,600`. That is proportional to the operating
frequency: about −11 Hz at 7 MHz, so about −22 Hz at 14 MHz and −44 Hz
at 28 MHz. Receive shifts by the same fraction, so the radio reads low
in both directions - which is why nothing in an RX-only test showed it.

Solving for the reference: `T = C × (1 + e / (f − 22,600))`. The four
readings average −11.25 Hz at 7.219 MHz, giving 24,999,961 Hz, about
1.6 ppm low.

**Applied.** `hw_settings.ini` now carries `cal=24999961` under a
`[tcxo]` section - the same key, units and section sbitx's own file
uses, so a settings file copied from either project works in the other.
A top-level `cal` is accepted too. `hw_settings_load()` passes it to
`si5351_set_calibration()` before any clock is set; left out, the
nominal 25,000,000 applies.

**Refining it.** The four readings spread about ±0.3 ppm, which is the
remote receiver's own accuracy as much as anything. Better references,
in order:

- Receive WWV (5, 10, 15 MHz) or CHU and read the carrier offset in a
  spectrum display. Higher frequency means more resolution per ppm.
- Transmit a tone into a dummy load and read it on a GPS-locked
  receiver (a KiwiSDR advertising GPS).
- Measure clk2 directly with a counter locked to a good reference.

Whichever is used: `new cal = old cal × (1 + error / measured frequency)`,
where `error` is how far low the reading is. Re-check after a big
temperature change; a TCXO drifts far less than a plain crystal, but
not to zero.

## Doing the measurements

- **Where to look.** maxibitx can't watch its own transmission - RX
  capture is muted during TX. Use a remote receiver (a nearby
  WebSDR/KiwiSDR, or a second radio), and transmit into a dummy load or
  at low power where possible.
- **Carrier offset.** Single tone, USB, note the dial. The signal should
  appear exactly 1000 Hz above the dial; repeat in LSB (1000 Hz below).
- **Power.** Know whether your wattmeter reads average or peak. An
  averaging meter should show the two-tone at about half the
  single-tone reading.
- **IMD.** On the remote spectrum, compare the products at −500 and
  +3100 Hz (relative to the carrier) with the two tones. Record the
  level in dB below each tone at a few drive levels per band.
