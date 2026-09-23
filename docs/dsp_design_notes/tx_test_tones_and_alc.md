# TX test tones, carrier-offset check, and ALC

Status: in progress. Step 1 (the test-tone generator) is built and
bench-tested; step 2 (the carrier-offset check and fix) is measured,
fixed and confirmed on air, and the reference-frequency question it
raised is settled. Step 4's limiter is built and bench-tested and is
live at its normal setting. Steps 3 and 5 are open.

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
4. **Digital ALC** (built; see below). A peak limiter on the transmit
   envelope inside `tx_pipeline.c`, with its ceiling set from step 3.
   Because output power in this design is set only by the DAC amplitude,
   which is already calibrated per band, the limiter can hold PEP
   without a feedback loop. Exposed as `RFPOWER` (the ceiling) and `ALC`
   (gain reduction).
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

The transmit tone measurements all read low by about the same fraction:

| Date | Mode | Error vs. predicted | Implied reference |
|---|---|---|---|
| 09-22 (before the placement fix) | USB | −10.6 Hz | 24,999,963 Hz |
| 09-22 (before the placement fix) | LSB | −13.9 Hz | 24,999,952 Hz |
| 09-22 (after the placement fix) | USB | −7.8 Hz | 24,999,973 Hz |
| 09-22 (after the placement fix) | LSB | −12.8 Hz | 24,999,956 Hz |

An error in the si5351's reference oscillator scales every clock it
makes. Writing `T` for the real reference and `C` for the value the code
assumes:

- **Transmit** moves by `(T/C − 1) × (f − 22,600)`, because the carrier
  comes out as `clk2 − bfo_freq + IF` and both clocks scale.
- **Receive** moves the *other way*, by `−(T/C − 1) × (f − 24,000)`: the
  first mixer's LO is above the signal, so a high clk2 pushes the IF up,
  which the inverted baseband then shows as a station lower in frequency.

That opposite sign is what settled this. Both are proportional to the
operating frequency: 1 ppm is 7 Hz at 7 MHz, 15 Hz at 15 MHz and 28 Hz
at 10 m.

**The reference is at nominal.** Receiving WWV 15 MHz (a GPS-referenced
transmitter) through SDR Console on the HPSDR I/Q, with `cal` at
25,000,000, put the carrier on frequency as closely as it could be
read - within a couple of Hz, so under ~0.2 ppm. A reference 1.5 ppm
low, as the transmit readings suggested, would have shown WWV about
22 Hz *high* instead.

So the ~11 Hz the transmit tests saw was not this radio: the remote
receiver used for them is itself about 1.5 ppm low. `cal` stays at its
nominal 25,000,000, and WWV on receive is the reference to re-check it
against. Transmit should now be within the ~6 Hz of bin quantization;
confirming that needs a GPS-locked receiver, not the one used so far.

**Lesson for the next measurement:** an absolute frequency check is only
as good as the receiver making it, and here transmit and receive respond
to a reference error with opposite signs - so a disagreement between a
transmit and a receive check localizes the error rather than averaging
into a wrong answer.

**Refining it.** Better references, in order:

- Receive WWV (5, 10, 15 MHz) or CHU and read the carrier offset in a
  spectrum display. Higher frequency means more resolution per ppm.
- Transmit a tone into a dummy load and read it on a GPS-locked
  receiver (a KiwiSDR advertising GPS).
- Measure clk2 directly with a counter locked to a good reference.

Whichever is used, apply the formula for that direction (transmit or
receive) from above - they have opposite signs. Re-check after a big
temperature change; a TCXO drifts far less than a plain crystal, but
not to zero.

## The limiter (step 4)

**Two numbers, not one.** The ceiling people mean by "ALC setting" is
really two separate facts. `full_scale_power` is what a full-scale
signal produces — a measurement, the thing the `[tx_band]` scales
encode, and the board's rated output. `max_power` is where the operator
chooses to run — a policy. Both live in `hw_settings.ini`; the runtime
`POWER` control rides underneath `max_power` and cannot exceed it, which
is why the protective number is in the file and not in a CAT command.

**The limiter needs no output-power headroom**, and an earlier draft of
this note was wrong to suggest it did. A 100 W rig makes 100 W on CW and
100 W PEP on SSB; its ALC isn't holding SSB down to a fraction of the
PA, it's stopping the *drive* from pushing past rated output. The same
applies here. Equal values put the ceiling at 1.0, and that is the
normal configuration: rated power in every mode, with the limiter
holding it.

The working range comes from the drive side instead, where it is
plentiful. `mic_tx_gain` reaches 64×, and in DIGITAL the level is the
host's entirely, so the signal arriving at the limiter can exceed full
scale by 20 dB or more. Before the limiter existed, that overshoot
reached `TX_SAMPLE_CLAMP` and was hard-clipped; now it is pulled back to
the ceiling as a gain. CW is the easy case: its envelope is constant at
exactly full scale, so the gain never moves and no headroom is wanted or
used.

Setting `max_power` below `full_scale_power` therefore means one thing
only — running below rated power, as when a board capable of more is
deliberately held at 5 W. It is the same operation as turning `POWER`
down, made persistent.

**Where it acts.** On `2*|out_c[i]|` in `tx_pipeline.c`, just before the
real part is taken — the magnitude of the still-complex signal, which is
the envelope the exciter will radiate. The input audio's peak is the
wrong quantity twice over: the 300–3000 Hz bandpass and the sideband
zero both change it, and an SSB envelope peak can exceed the audio peak
that produced it. The pipeline is complex at exactly that point anyway,
so the right quantity costs nothing.

**A gain, not a clipper.** The limiter multiplies by
`min(1, ceiling/envelope)`; a sine times a constant is still a sine.
This is the difference between holding 5 W cleanly and flat-topping the
waveform into harmonics, and it's what Case F measures.

**Look-ahead.** The output is delayed 192 samples (2 ms) while the gain
is computed from the undelayed signal, and attack is rate-limited to
cross the whole gain range in exactly that window — so however far the
target drops, the gain arrives before the sample that needs it. A
feedback design would have to choose between reacting fast enough to
distort and reacting slowly enough to let overshoots through; look-ahead
avoids the choice. Release is 250 ms, slow enough not to pump inside a
syllable. The 2 ms is real added TX latency: it shifts the whole
envelope, CW element timing included, without changing its shape.

**Downward only.** Gain never exceeds unity, so quiet speech transmits
quietly. An upward-acting control would raise room noise between words
to full power, which is why transmitters limit and receivers AGC.
Raising the average to meet the ceiling is a speech compressor — a
separate thing, belonging upstream of the pipeline, deliberately not
built here.

**Bench results.** `tx_pipeline_test.c` Case F, a single tone through
the real pipeline. The first three rows are the everyday case: the
ceiling at full scale, the drive 6 dB too hot.

| Check | Result | Expected |
|---|---|---|
| At the ceiling, drive 1.0: peak / tone / ALC | 1.0000 / −0.00 dB / 0.00 dB | untouched |
| Drive ×2, same ceiling: peak / tone / ALC | 1.0000 / −0.00 dB / 6.02 dB | held at rated power, 6.02 dB of reduction |
| Overdriven output vs. the reference | 7.70e−05, 82 dB down | the limiter gave back exactly what the drive added |
| What truncating at the ceiling would cost | 4.99e−01, 6.0 dB down | — |
| `POWER` 50% (ceiling 0.707), drive 1.0 | 0.7070 / −3.01 dB / 3.01 dB | running 3 dB below rated |

The middle two rows answer "will it clip my CW?": the limiter's
departure from a pure gain is 76 dB smaller than a clipper's would be.
Re-run with `make test-tx-pipeline && ./test-tx-pipeline`.

**Not yet radio-tested.** The rigctld `RFPOWER`/`ALC` commands and the
panel's slider and meter are written but haven't been exercised against
a running daemon — the bench harness links `tx_pipeline.c` without any
of the hardware code.

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
- **Setting `full_scale_power`.** The IMD measurement is what justifies
  the number: the highest PEP at which the products are still
  acceptable is what the board's rated output should be. Raise the
  `[tx_band]` scales by the square root of the power ratio, confirm each
  band on a wattmeter, then set `full_scale_power` to the confirmed
  figure. This is about how much power the radio makes, not about giving
  the limiter room - it already has all it needs on the drive side. The
  reason to keep the rating honest is that the limiter sees only its own
  envelope, never the PA's compression: rate it too high and the digital
  chain politely holds a number the PA stopped honoring.
