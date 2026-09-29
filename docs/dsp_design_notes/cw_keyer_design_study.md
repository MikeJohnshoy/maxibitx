# CW keyer: a feasibility study

Status: **study only — nothing is implemented.** Written as the precursor
to building a keyer, to find out before any code exists whether the
requested design holds up, and where it doesn't, what to do instead.
§1–§13 are the study as first written. **§14 records the decisions taken
after reading it**, and §15–§17 are the resulting specification — the
keyer's modes, its input path, and its weighting — which is what the
implementation should be built and tested against. Where §14–§17 and an
earlier section differ, the later section is the decision.

The request, as stated:

- A selectable input: straight key, or an electronic keyer offering
  **bug** emulation, **ultimatic**, **iambic A/B**, and sending a supplied
  **string of text** as Morse.
- Key or paddle on the existing sBitx key connector.
- **1 to 60 WPM.**
- The existing Blackman-Harris keying shape, with **1:1 weighting defined
  at the 50% points** — the mark measured rise-50% to fall-50% equal to the
  space between elements.
- Keyer logic and Morse tables **based on** sbitx `dev-54bugfixes`
  `src/modem_cw.c` (the "reference keyer" below).
- **Block-level key polling and TX control, if it gives a responsive
  paddle**, with sample-level envelope interpolation and sample-level
  oscillator reads.
- **Easy to remove or replace** later.

Every number here is either measured by the tools in
[`tools/keyer_study/`](../../tools/keyer_study/) against the real
maxibitx and reference sources, derived from the code with the derivation
shown, or cited. Predictions are labelled as predictions.

## 1. The answer in brief

**It is feasible, and most of the requested structure already exists.**
`cw.c` today polls the key once per audio block and runs the envelope and
the oscillator per sample — exactly the split asked for, for a straight
key. A keyer slots in between the poll and the envelope.

Five findings change the shape of the build:

1. **The reference keyer does not poll the paddles at 96 kHz.** Its state
   machine runs at 96 kHz, but it reads a variable that the GTK UI timer
   refreshes nominally every 1 ms (§3). So the real comparison is
   *1 ms versus 10.67 ms*, not 10 µs versus 10.67 ms.
2. **Plain block polling works, but its cost grows with speed.** At
   20 WPM it is acceptable; at 60 WPM a keyer decision can go either way
   for about a fifth of a dit, and a tap shorter than one block can be
   missed outright (§5).
3. **The GPIO interface maxibitx already uses can timestamp every paddle
   edge in the kernel.** Draining those events once per block keeps
   block-level processing and still gives sample-exact input timing — no
   missed taps, no ambiguous decisions, exact straight-key timing — for a
   fixed one-block delay (§6).
4. **Neither polling scheme is the dominant latency. The ALSA playback
   queue is**, at roughly 29 ms against at most 10.7 ms for polling (§7).
   Sidetone and RF share that queue, which ties sidetone latency to the
   T/R switch's head start: shortening one shortens the other (§8).
5. **The existing envelope table does not give 1:1 weighting** — every
   mark is 1.56 ms light at every speed, which is 46% weighting at 60 WPM.
   It is fixable without changing the shape, by a compensation derived from
   the table itself (§4). The reference table has an additional defect
   worth knowing about before porting anything from it (§3).

The recommended design (§10) is three small pieces behind one narrow
seam, so that the keyer can be removed by swapping one file for a stub.

## 2. What maxibitx already has

The audio thread's loop in `sound.c`, once per 1024-sample block
(10.667 ms at 96 kHz):

```
snd_pcm_readi()        blocks until a capture period is in
sound_process()        RX DSP, ~3.08 ms on the Pi (ARCHITECTURE.md §10 step 7)
cw_poll_key()          reads CW_KEY (BCM 4) once; T/R and hang logic
for each sample:       cw_get_sample() - envelope step, then oscillator read
tx_pipeline           the same block, CW path to the R (exciter) channel
snd_pcm_writei()       L = sidetone, R = exciter, one interleaved write
```

`cw_get_sample()` moves an index one step per sample toward the key state
and reads the envelope table there, then multiplies by `vfo_read()` of the
keyed-tone oscillator. So the envelope is already sample-level and
slew-limited, and the oscillator is already read per sample. The key is
read once per block, and a key-down becomes audible from sample 0 of the
block being generated — `cw_poll_key()` runs immediately before the block
is synthesised, so there is no internal delay beyond the poll itself.

The sidetone (left channel) is the keyed tone scaled by
`SIDETONE_PEAK_AMPLITUDE`; the RF (right channel) is the same tone through
`tx_pipeline.c`. Same buffer, same write, same queue — which matters in §8.

TX is asserted by `radio_set_tx()`, which only hands the sequence to a
worker thread (`radio_tx_apply()`), so the audio thread never blocks on
the ~30 ms of mixer calls, I2C writes and the 20 ms amplifier delay. That
also holds for anything a keyer does: TX control at block level is already
safe.

## 3. The reference keyer, read closely

`modem_cw.c` on `dev-54bugfixes` (checked out at `a8e2aa9`). What it
does, and what is worth carrying over.

**Worth porting:** the per-mode handlers — `handle_mode_straight()`,
`handle_mode_bug()`, `handle_mode_ultimatic()`,
`handle_mode_iambic_common()` for A and B, `handle_mode_kbd()` for text —
driven by two counters, `keydown_count` and `keyup_count`, in samples, with
`cw_period = 115200 / wpm` (one dit at 96 kHz). The Morse table and its
256-entry lookup. The text spacing rules in `handle_mode_kbd()`: every
element is followed by one dit of space, the end of a character adds two
more, and a word space adds four more after a character end (seven in
total), all in the same counters.

**It does not poll at 96 kHz.** The comment on `cw_read_key()` says so
directly: it is called 96 000 times a second but "should not poll gpio
lines … we only read the status from the variable updated by
modem_poll()". The variable is `cw_key_state`, written by `cw_poll()` from
`key_poll()` (`sbitx_gtk.c`); `cw_poll()` is reached through
`modem_poll()`, which `ui_tick()` calls on every tick in CW — and
`ui_tick` is a `g_timeout_add(1, …)` GTK timer. The paddles are
therefore sampled nominally every millisecond, on the GUI thread, and a
GTK timeout is best-effort: it runs later when the main loop is busy
drawing. The 96 kHz part is only the evaluation of a value that is up to
a millisecond old, and sometimes older.

**Pins.** `key_poll()` reads `PTT` (wiringPi 7 = BCM 4) and `DASH`
(wiringPi 21 = BCM 5). In paddle modes a low on BCM 4 means *dash* and a
low on BCM 5 means *dot* — so the line named `DASH` carries the dot
paddle — and `cw_reverse` swaps them. In straight-key mode either
line low is key-down. maxibitx already claims BCM 4 as `CW_KEY`; BCM 5 is
unused. Which contact is the jack's tip and which its ring on a DE board
is not recoverable from the code and should be checked with a meter before
the lines are named in `radio_hw.h`.

**The envelope table has a hole in it.** `cw_envelope_data` is declared
`[480]` but initialised with 479 values, so C zero-fills element 479. The
rise reads `data[0..479]` and the fall starts by reading `data[479]`, so
every element has **a single sample at zero at the top of its rise, and
another at the start of its fall**:

```
rise, samples 476..482 after key-down: 0.9999 1.0 1.0 0.0 1.0 1.0 1.0
fall, first four samples after key-up:  0.0 1.0 1.0 0.9999
```

Measured on the envelope alone (30 WPM dits at 700 Hz, before any
filtering sbitx applies afterwards), the −60 dBc occupied bandwidth goes
from 875 Hz with the last value set to 1.0 to the entire 48 kHz band as
written. So the table should not be ported verbatim — and it is worth
fixing on that branch regardless. maxibitx's own table is not affected: it
has all 480 values and ends at exactly 1.0.

**Not worth porting:** the wiringPi reads and the GTK coupling, obviously;
and one behaviour of the text path. While text is queued the whole machine
switches to `CW_KBD`, and a paddle press during a message is fed to
`handle_mode_kbd()` as a symbol — so touching the paddle mid-message sends
a single keyboard-style element rather than either aborting the message or
keying iambically. §9 proposes the conventional behaviour instead.

## 4. Weighting: meeting 1:1 at the 50% points

Both tables — maxibitx's and the reference's — are **the rising half of a
Blackman-Harris window** (maxibitx's fits a 959-point window to within
5×10⁻⁷). That shape is smooth at both ends, but it is not antisymmetric:
it crosses 50% at sample 314.6 of 479, not at the midpoint 239.5.

Driven the way `cw_get_sample()` drives it — one step up per key-down
sample, one step down per key-up sample — the rise reaches 50% 314.6
samples after key-down, and the fall reaches 50% only 164.4 samples after
key-up. Every mark measured at the 50% points is therefore **150 samples
(1.56 ms) short**, and every space 1.56 ms long, at any speed:

| WPM | dit | mark as today | weighting | with compensation | weighting |
|---:|---:|---:|---:|---:|---:|
| 1 | 1200.0 ms | 1198.44 ms | 49.9% | 1200.00 ms | 50.0% |
| 10 | 120.0 ms | 118.44 ms | 49.3% | 120.00 ms | 50.0% |
| 20 | 60.0 ms | 58.44 ms | 48.7% | 60.00 ms | 50.0% |
| 30 | 40.0 ms | 38.44 ms | 48.0% | 40.00 ms | 50.0% |
| 40 | 30.0 ms | 28.44 ms | 47.4% | 30.00 ms | 50.0% |
| 60 | 20.0 ms | 18.44 ms | 46.1% | 20.00 ms | 50.0% |

This already applies to the straight key today, where it shortens every
mark the operator makes by 1.56 ms.

Three ways to get to 50.0%:

| option | how | −40 / −60 / −80 dBc bandwidth |
|---|---|---|
| as today (for reference) | — | 500 / 875 / 1650 Hz |
| **A. keep the table, compensate** | hold key-down 150 samples longer | 475 / 900 / 1613 Hz |
| B. integrated BH, same 5 ms | antisymmetric edge by construction | 575 / 975 / 1275 Hz |
| C. integrated BH, 6.49 ms | same, stretched to the table's 2.335 ms 10–90% rise | 475 / 775 / 976 Hz |
| hard keying (for contrast) | — | 1550 / 16725 / 47976 Hz |

(30 WPM continuous dits at 700 Hz. B and C are the running integral of a
Blackman-Harris window, which crosses 50% exactly at its midpoint.)

**Recommendation: A**, because it is what was asked for — the existing
shape — and it costs one addition in the scheduler. The 150 samples should
be *computed from the table at init*, not written down, so that a future
table swap keeps 1:1 automatically; `envelope_study.py` shows the
derivation (`2 × index(50%) − (N − 1)`). It applies to every mode: in
straight-key mode it makes the transmitted 50% mark equal the operator's
contact closure. **C is the interesting follow-up**: narrower at every
level, exact by construction, at the cost of a 6.5 ms ramp — still well
inside a 60 WPM dit — and worth an on-air listen once the keyer exists.

**The TX limiter does not undo any of this.** Checked by feeding the
table-shaped dits through the real `tx_pipeline.c` at four limiter
ceilings (`envelope_study.py --limiter`): the 50% mark is 38.43–38.44 ms at
every ceiling from 1.0 down to 0.316, including the first element of the
transmission, because the look-ahead has the gain settled before each
peak and the 0.25 s release holds it across the gaps. The only effect is a
slight widening far out: −80 dBc bandwidth 1463 Hz at unity, up to
1787 Hz at a 0.5 ceiling. Scaling the CW source by the power fraction
before the pipeline would remove even that, but it is not a keyer problem.

## 5. Block-level polling: what it costs, measured

`tools/keyer_study/iambic_sim.c` ports the reference state machines
unchanged and runs them at 96 kHz against scripted paddle input, read
three ways: every sample (ideal), every 96 samples (the reference's real
1 ms), and every 1024 samples (a block). For each scenario it sweeps the
time of one operator action finely across several elements, and at each
point tries poll phases spread across the whole polling interval,
recording whether the *sequence of elements sent* differs from ideal.

**Reaction from idle** — the delay before the first element starts:

| scheme | mean | worst |
|---|---:|---:|
| ideal | 0 | 0 |
| reference (1 ms) | 0.49 ms | 0.99 ms |
| block | 5.33 ms | 10.66 ms |

**Decision ambiguity.** Every keyer decision — whether the opposite
element was latched, whether iambic B adds one more, how many auto-dits a
bug sends — has a boundary in time. An operator action close enough before
the boundary is seen only after it, and the outcome flips. The table gives
the *equivalent width* of that zone per decision point, in ms and as a
fraction of a dit:

| scenario | 20 WPM | 40 WPM | 60 WPM |
|---|---|---|---|
| iambic A, squeeze, release both | 4.2 ms (7%) | 5.5 ms (18%) | 4.1 ms (20%) |
| iambic B, squeeze, release both | 3.8 ms (6%) | 4.6 ms (16%) | 3.9 ms (19%) |
| iambic B, dot tapped for 1 dit during a dash | 4.6 ms (8%) | 4.9 ms (16%) | 9.4 ms (47%) |
| iambic B, dot tapped for ½ dit during a dash | 4.6 ms (8%) | 4.9 ms (16%) | 14.1 ms (70%) |
| ultimatic, dash added to a held dot | 3.1 ms (5%) | 4.1 ms (14%) | 3.1 ms (16%) |
| bug, auto-dits then release | 3.1 ms (5%) | 4.1 ms (14%) | 3.1 ms (16%) |
| *reference 1 ms, any scenario* | *0.00 ms* | *0.23 ms (0.8%)* | *0.32 ms (1.6%)* |

Two patterns. For paddles that are *held*, the zone is 3–5.5 ms per
decision whatever the speed — about half a block, as it should be — so its share
of a dit grows in proportion to WPM. *Taps* are worse once they are only a
block or two long: a tap can be noticed too late for the window it was
aimed at, and one shorter than a block can fall between two polls and be
lost entirely — with probability `1 − d / 10.67 ms` for a tap of length
*d*. At 60 WPM a half-dit tap is 10 ms.

**Straight key and the bug's manual dashes** are different in kind: the
operator's own timing *is* the output, and each edge is quantised to a
block. With both edges independently late by up to a block, an element's
length error is triangular over ±10.67 ms with a standard deviation of
10.67/√6 = **4.4 ms** — 7% of a dit at 20 WPM, 22% at 60 WPM. This is how
the straight key already behaves today.

**Verdict on plain block polling.** Usable at ordinary speeds — at 20 WPM
the zone is 5–8% of a dit — and increasingly worse than the reference as
speed rises. It meets "responsive" for
reaction time only in the sense that reaction time is dominated by
something else (§7). It does not meet it for high-speed iambic, for short
taps, or for a straight-key fist.

## 6. Exact input without faster polling: kernel-timestamped edges

maxibitx drives GPIO through the Linux GPIO character device, v2 uAPI
(`gpio.c`: `GPIO_V2_GET_LINE_IOCTL` on `/dev/gpiochip0`) — no wiringPi, no
libgpiod. The same interface, confirmed in `linux/gpio.h`, provides:

- **Edge events**: request the line with `GPIO_V2_LINE_FLAG_EDGE_RISING |
  GPIO_V2_LINE_FLAG_EDGE_FALLING`, and every transition is queued in the
  kernel as a `struct gpio_v2_line_event`, read with a plain `read()` on
  the line fd.
- **A timestamp on each event**, `timestamp_ns`, the kernel's estimate of
  when the edge occurred, from `CLOCK_MONOTONIC` by default.
- **Sequence numbers** (`seqno`, `line_seqno`), so an overflowed queue is
  detectable rather than silent. `event_buffer_size` sets the queue depth.
- **Debounce**, `GPIO_V2_LINE_ATTR_ID_DEBOUNCE` with `debounce_period_us`,
  done by the kernel.
- **Several lines in one request** (up to 64), so both paddle contacts
  arrive on one fd, each event carrying its line's `offset`.

That gives a scheme that keeps everything else block-level:

1. Request BCM 4 and BCM 5 together as inputs with pull-ups, both edges,
   debounce, and `O_NONBLOCK` on the resulting fd.
2. Once per block, where `cw_poll_key()` runs now, drain the fd — typically
   zero or one event, never more than a handful.
3. Map each event's timestamp to a sample offset within the block,
   against the capture timestamp `sound.c` already takes after
   `snd_pcm_readi()` (`t_read1`): events during the previous block
   interval land at the same relative position in the block about to be
   generated.
4. Run the keyer over that block with the events at their exact sample
   positions.

By construction this is **the ideal row of §5 delayed by exactly one
block** — so every ambiguity in the table goes to zero, taps are never
missed, and the straight key's timing is reproduced to within scheduling
noise on `t_read1` (sub-millisecond; `snd_pcm_status()`'s hardware
timestamps would tighten it if it ever mattered). No new thread, no new
dependency, and the audio thread still does one non-blocking read per
block.

The cost is latency. Plain polling adds 0–10.67 ms (mean 5.33); this adds
a constant 10.67 ms. **The worst case is the same; the average is 5.3 ms
worse.** A refinement exists — start a character from idle as soon as the
block allows, then run its timeline from the exact timestamps — but it
adds bookkeeping for 5 ms, and §7 shows where the larger milliseconds are.

Two practical matters this scheme brings into view, both needed anyway:

- **A mono straight-key plug in a stereo jack grounds the ring**, which
  reads as a permanently closed second paddle. The usual keyer answer is to
  sample both lines at start-up and, if one is held closed, treat the
  input as a straight key and ignore that line — and say so on the console.
- **Debounce and timestamps.** Whether a debounced event's timestamp marks
  the first edge or the end of the debounce period is a kernel detail to
  confirm on the Pi. Either way it is the same offset on press and release,
  so element timing is unaffected.

## 7. The latency that dominates: the playback queue

`sound.c` sizes the playback buffer at four periods and primes it full
immediately before starting the audio thread. Capture and playback run
from the same codec clock and the loop writes exactly one period per
period captured, so the fill level set at start-up persists. Just before
each write it is

```
F ≈ (periods − 1) × 1024 − (thread start + process time) × 96 frames/ms
  ≈ 3072 − 3.08 × 96  ≈ 2776 frames  ≈ 28.9 ms
```

using the 3.08 ms process time measured on the Pi and taking the thread
start as negligible (a slower start only lowers `F`). The first sample of a
newly written block reaches the DAC about 29 ms later — measured on the
Pi 4 at 28.0–28.8 ms, below. So key-to-sidetone latency, the four-period
row confirmed by that measurement and the others derived from it:

| playback buffer | queue | + plain polling | + timestamped edges |
|---|---:|---:|---:|
| 4 periods (today) | ~28.9 ms | 29–40 ms | ~40 ms |
| 3 periods | ~18.2 ms | 18–29 ms | ~29 ms |
| 2 periods | ~7.6 ms | 8–18 ms | ~18 ms |

For scale: FlexRadio's community manager has quoted their internal keyer
at **8 ms** key-down to sidetone, and in the same thread operators called
a keyer that felt like "up to about 50 ms at times" sluggish, one of them
going back to an external keyer to be comfortable at 20–25 WPM
([FlexRadio community](https://community.flexradio.com/discussion/5635304/internal-keyer-latency-paddle-press-to-sidetone)).
The Hermes-Lite discussion draws the useful distinction: key-to-*RF*
latency of 50–70 ms "doesn't matter in practice", while *sidetone*
latency is what the operator feels
([Hermes-Lite list](https://groups.google.com/g/hermes-lite/c/P-i_EYCN--k)).

The largest lever is therefore the buffer depth, not the polling scheme.
It is a global setting — RX audio, WSJT-X's feed and TX all share it — and
its other face is headroom: the same `F` is how long the loop can stall
before the DAC underruns, so two periods leaves about 7.6 ms of margin
against today's 29, less on a Pi Zero 2W whose process time is longer. The
start-up xrun flood this project fought was a sequencing race, not a
steady-state margin problem (ARCHITECTURE.md §10 step 7), so fewer periods
may well be fine — but it has to be measured, on both boards, before a
keyer depends on it.

**Measured on the Pi 4, and it matches.** The kernel publishes the running
playback stream's queue as `delay` in
`/proc/asound/card0/pcm0p/sub0/status`, so no code change is needed.
Sampling it 300 times, 13 ms apart so the readings walk across the whole
block cycle:

```
for i in $(seq 300); do
  awk '$1=="delay"{print $3}' /proc/asound/card0/pcm0p/sub0/status
  sleep 0.013
done | sort -n | awk 'NR==1{min=$1} {max=$1} END{printf "min %d frames (%.1f ms)  max %d frames (%.1f ms)\n", min, min/96, max, max/96}'
```

| run | minimum | maximum |
|---|---:|---:|
| 1 | 2688 frames, 28.0 ms | 3848 frames, 40.1 ms |
| 2 | 2768 frames, 28.8 ms | 3844 frames, 40.0 ms |
| 3 | 2736 frames, 28.5 ms | 3836 frames, 40.0 ms |

The minimum is the queue just before a write — `F`, the wait for the first
sample of a key-down — and at 28.0–28.8 ms it is within 1 ms of the 28.9 ms
derived above, slightly under it as a real thread start should make it.
The maximum is the queue just after a write. It sits 1076–1160 frames
above the minimum rather than exactly one 1024-frame period, because the
write itself lands a little earlier or later in each cycle as process
time varies: that spread, about 0.5–1.4 ms, is the loop's own timing
jitter showing through. Key-to-sidetone latency on this board today is
therefore about 28–39 ms with plain polling, as the table says.

The same recipe, run on a Pi Zero 2W and with fewer playback periods, is
what the next step in §12 needs. The depth is set at start-up by
`MAXIBITX_PLAYBACK_PERIODS` (2–8, default 4;
[`08_troubleshooting_and_bringup.md`](../08_troubleshooting_and_bringup.md)).
Predicted minimums, from the same arithmetic less the ~0.5 ms the Pi 4 came
in under it: about 17.7 ms at three periods and 7.1 ms at two — the second
also being all the margin the loop would have against an underrun.

**Two periods, measured on the Pi 4:** four runs of the same recipe gave
minimums of 640, 676, 688 and 764 frames (6.7–8.0 ms) and maximums of
1676–1784 frames (17.5–18.6 ms), with **no xruns**. The prediction holds to
within a millisecond. Key-to-sidetone latency at two periods is therefore
about 7–19 ms with plain polling, against 28–39 ms at four — the whole
difference being queue, none of it polling. What the no-xrun result covers
is only the load it was run under; the margin it leaves is the ~7 ms
minimum itself, against ~0.5–1.4 ms of observed write-time jitter, so it
should be confirmed while transmitting, retuning, changing filter settings,
and with WSJT-X on the gadget, and separately on a Zero 2W.

## 8. Sidetone and RF share one queue — the T/R consequence

Because the exciter feed is the right channel of the same write, **RF is
delayed exactly as much as the sidetone**, plus the TX pipeline's own
7.3 ms (5.33 ms linear-phase FIR group delay and 2.0 ms limiter
look-ahead). That delay is, in effect, the T/R switch's head start.

The TX-up sequence in `radio_tx_apply()` is two ALSA mixer calls (each
opens, attaches and loads the mixer — unmeasured), two Si5351 writes
(~4.8 ms each at the *assumed* 100 kHz bus rate,
[`cw_breakin_and_i2c_costs.md`](cw_breakin_and_i2c_costs.md) §2) and
the 20 ms amplifier delay, with the exciter drive unmuted last: about
30 ms plus the mixer calls. The first keyed sample reaches the DAC about
29 + 7.3 ≈ 36 ms after the poll that saw the key.

That revises [`cw_breakin_and_i2c_costs.md`](cw_breakin_and_i2c_costs.md)
§7, which predicted a truncated first element but did not count the
playback queue. **With the queue included the race is close, not lost** —
a few milliseconds either way, depending on the mixer calls and the real
bus rate. The five-dit test in that note is still the arbiter. It also
means **any cut in playback latency eats the head start**: at two periods
the tone would reach the DAC roughly 15 ms before the drive is unmuted.

With two periods now measured (§7), that is a sharp, testable prediction:
**at two periods the first element on air should be about 15 ms short**,
while at four it should be intact or nearly so. The five-dit test at each
setting — same remote receiver, first dit against the other four — checks
the whole §8 model at once. Until it has been run, two periods is a
sidetone setting, not one to transmit CW with.

A first listen on a WebSDR at two periods heard all five dits clean, which
does not settle it: 15 ms off a hand-keyed dit is within a straight key's
own variation, a WebSDR's AGC can hide it, and bursts less than the hang
time apart only expose the very first dit. So the race is now measured in
software on every transmission start: `MAXIBITX_TR_TIMING=1` has the TX
worker print, when it unmutes the drive, how long after the TX request that
happened and when the first TX sample reaches the DAC — the write time,
the queue from `snd_pcm_delay()` at that write, and the pipeline's 7.3 ms —
and whether the drive won (`spare`) or lost (`CLIPPED`) and by how much
([`08_troubleshooting_and_bringup.md`](../08_troubleshooting_and_bringup.md)).
The prediction is spare at four periods and about 13–15 ms clipped at two.

**Measured on the Pi 4, and the model holds.** Each figure is one
transmission starting from receive:

| playback periods | result, per transmission | mean |
|---|---|---:|
| 4 (default) | spare 9.8, 6.2, 7.6, 10.1 ms | 8.4 ms spare |
| 3 | clipped 4.3, 0.3, 0.2, 0.2, 3.7, 0.2 ms | 1.5 ms clipped |
| 2 | clipped 11, 14, 15, 11 ms | 12.8 ms clipped |

Two periods lands inside the predicted 13–15 ms; four has the few
milliseconds to spare predicted, and three sits almost exactly on the
boundary. Backing the drive-up time out of the figures gives about
25–30 ms after the request, of which the amplifier delay is 20 ms. The
spread within each setting, about 4 ms, is the drive-up sequence's own
variation plus the queue's start-up phase.

So the default loses nothing to the T/R switch today — the first-element
truncation that
[`cw_breakin_and_i2c_costs.md`](cw_breakin_and_i2c_costs.md) §7 first
predicted does not happen at four periods, at least in the software
sequence — and **any cut in sidetone latency below about three periods
needs one of the two remedies above first**:

- **`ext_ptt_delay_ms = 0`** (break-in note §9, now implemented), for a
  station with no amplifier on `EXT_PTT`: drive-up falls to about 5–10 ms,
  which at two periods would leave roughly 5–9 ms spare. Checkable with the
  same line — but it is safe only once it is known what `TX_LINE` gates on
  a DE board, which is outside what this measures.
- **The right-channel RF delay**, for everyone else: 20 ms of it at two
  periods restores about the margin four periods has today, whatever
  `EXT_PTT` is driving.

Either way, `MAXIBITX_TR_TIMING` is the acceptance test.

**`ext_ptt_delay_ms = 0` at two periods, measured on the Pi 4:** drive-up
at +9.1, 6.9, 6.8, 6.8, 6.8 ms and the first TX sample at the DAC at
+14.4–15.7 ms, so **spare 5.2, 8.8, 8.9, 8.9, 8.8 ms** — the predicted
5–9 ms. With the delay at 20 on the same run, drive-up was +26.7–28.9 ms
and 11–15 ms was clipped, so removing the delay moved drive-up by exactly
the 20 ms and nothing else. The first transmission after start-up is
consistently about 2 ms slower to drive-up than the rest.

For a station with nothing on `EXT_PTT`, then, two periods and
`ext_ptt_delay_ms = 0` together give 7–19 ms of sidetone latency with the
first element intact, as far as software can see — a margin similar to the
default's. What remains is the hardware side, the five-dit test on air at
exactly this setting, and for stations that do run an amplifier, the
right-channel RF delay — deferred for now (§14).

The clean decoupling is a **delay line on the right channel only**, in
CW: the sidetone leaves immediately, the RF follows a fixed D ms later.
Sidetone latency is then free to fall with the buffer depth while the T/R
sequence keeps whatever head start D gives it, and RF latency — the kind
that does not matter — rises by D. Element timing is unaffected, because
the delay is constant. On key-up the tail drains well inside the hang
time. Combined with `ext_ptt_delay_ms = 0` for a barefoot station (the
break-in note's §9), the question of first-element truncation disappears
for paddles and straight key alike. For *text*, the keyer knows its input
in advance and can simply request TX first and start sending once the
worker reports the drive is up — which needs a small "TX ready" flag set at
the end of `radio_tx_apply()`.

**The hang timer needs a speed-dependent floor.** `CW_HANG_POLLS` holds TX
for 298.7 ms after the last element. A character space is three dits, so
below about 12 WPM TX drops between characters — true of the straight key
today — and below about 4 WPM it drops between *elements*, each time
paying the full T/R sequence. With the keyer setting the speed, a hang of
at least eight dits, never less than today's 300 ms, holds TX across word
spaces (seven dits) even at 1 WPM, and changes nothing at 20 WPM or above,
where eight dits is under 300 ms.

## 9. Text to CW

The standards already exist on both control surfaces, which settles most
of the interface:

- **rigctld**: `b <text>` (`send_morse`), `\stop_morse` (`0xbb`), and the
  `KEYSPD` level in WPM, all standard Hamlib
  ([rigctl(1)](https://www.mankier.com/1/rigctl)). `KEYSPD` is the route to
  the full 1–60 WPM range, and adding it means extending `dump_state`'s
  level masks as `CWPITCH` did.
- **Kenwood CAT**: `KY` sends text and `KS` sets speed. On a TS-480, `KS`
  takes **010–060** WPM — so 1–9 WPM is reachable over rigctld and the
  panel but not over CAT — and `KY` carries a fixed 24-character,
  space-padded message, answering `KY0;`/`KY1;` for buffer free/full
  ([TS-480 PC command reference](https://www.kenwood.com/i/products/info/amateur/ts_480/pdf/ts_480_pc.pdf)).
  The QMX — the rig type FLRig is set to when it drives maxibitx — accepts
  a variable-length
  `KY <text>;` into an 80-character circular buffer and answers
  `KY0;`/`KY1;`/`KY2;` for sending/nearly full/idle
  ([QMX CAT reference](https://qrp-labs.com/images/qmx/manuals/cat_1_03_000.pdf)).
  Which of the two answer conventions FLRig's QMX driver expects is the one
  interface question to settle with FLRig itself.

**Prosigns disagree between interfaces.** Kenwood maps `[`→BT, `_`→AR,
`<`→AS, `#`→HH, `>`→SK, `]`→KN, `\`→BK, `%`→SN. The reference table uses
`=`→BT, `+`→AR, `&`→AS, `(`→KN, `>`→SK and `<`→BK — so `<` means AS on one
and BK on the other. The fix is structural: each interface translates its
own convention into one internal representation, and the keyer's table
never contains a protocol's punctuation.

**Paddle over text.** The conventional behaviour, and the one proposed: any
paddle or key closure aborts the queued text immediately and keys normally
from there. It is also the natural safety: the operator can always stop a
runaway message with the key.

The spacing rules and the Morse table port from the reference directly
(§3). Farnsworth spacing was not asked for; the scheduler in §10 would
take it as one more parameter if it ever is.

## 10. Proposed structure: one seam, three pieces

```
key_input.c   GPIO v2 edge events for BCM 4 and BCM 5, read by its own thread
              woken on each edge (§16): paddle reversal, mono-plug detection,
              early TX request. Queues events with their kernel timestamps.
              Knows nothing of Morse.
                 │  timestamped events, mapped to sample offsets
                 │  by the audio thread once per block
                 ▼
keyer.c       mode logic (§15), WPM, text queue and Morse table. Emits a
              key-down/up value per sample for the block, plus "TX wanted".
              Knows nothing of GPIO or audio.
                 │  key[n]
                 ▼
cw.c          envelope (with the §17 weighting correction) and oscillator, per
              sample; hang timer and radio_set_tx(). As today, with key[i] in
              place of the single key_down.
```

The audio-thread interface is one call per block:

```c
// keyer.h
int keyer_run_block(const struct key_event *ev, int n_events,
                    uint8_t *key, int n);   // returns 1 while TX is wanted
```

plus control-side setters (`keyer_set_mode()`, `keyer_set_wpm()`,
`keyer_send_text()`, `keyer_abort()`, `keyer_busy()`, and
`key_input_set_reverse()` on the input side), called from the rigctld and
CAT threads. Text reaches the
audio thread through a ring buffer: the two writers serialise between
themselves on the control side, and the audio thread's reading side never
takes a lock.

**Removal and replacement.** A `keyer_straight.c` implements the same
header in a few lines — `key[]` follows the key line, text and paddle
setters return "unsupported" — and the Makefile chooses between it and
`keyer.c`. Removing the keyer is building with the stub; replacing it is
writing another implementation of one function. `key_input.c` stands on its
own: it improves the straight key's timing with the stub in place, and a
different keyer can use it unchanged. Nothing else in the tree includes
`keyer.h` except `cw.c` and the two control surfaces.

**What does not change:** mic PTT in USB/LSB keeps following BCM 4's
level (which `key_input.c` tracks from the same events); DIGITAL keeps
ignoring the key; `tx_pipeline.c`, the envelope table and the
oscillator are untouched.

**Control-surface additions:** rigctld `KEYSPD`, `b`, `\stop_morse`, and
extensions for keyer mode and paddle reverse in the style of `CWWIDTH`;
CAT `KS` and `KY`; in `rigctl_panel.py`, a mode selector, WPM, reverse, and a
text line with Send and Stop.

## 11. Risks and unknowns

- **The playback queue depth** (§7) is measured on a Pi 4 at four
  periods (28.0–28.8 ms); the Pi Zero 2W and the shorter buffers are not
  yet.
- **The T/R race** (§8) depends on two unmeasured mixer calls and the
  assumed I2C rate. The five-dit test settles it.
- **Fewer playback periods on a Pi Zero 2W** may not hold without xruns.
  The whole latency plan should be tried on that board before relying on
  it.
- **Kernel debounce** on the Pi's GPIO driver: available through the
  uAPI, but its behaviour and timestamp semantics need a bench check with
  a real, bouncy paddle.
- **Which jack contact is dot and which dash** on a DE board (§3).
- **The reference state machines have quirks** — for example, in the DOT
  and DASH states an idle paddle leaves the state unchanged, and the text
  path's paddle handling (§3). *Decided (§14): the keyer is written from
  scratch against the specification in §15, whose golden cases are in
  `keyer_spec_model.c`.*
- **Whether 5 ms of average latency matters** between plain polling and
  timestamped edges is a feel question. If it does, the start-from-idle
  refinement in §6 recovers it.

## 12. Suggested order of work

Each step stands alone and is useful even if the keyer is never finished.

1. **Measure the playback queue**, and try three and two periods on a Pi 4
   and a Pi Zero 2W. Nothing else in this list decides the feel of the
   paddle as much. *Four and two periods on the Pi 4 are done (§7):
   28.0–28.8 ms and 6.7–8.0 ms, no xruns at two under the load tried.
   Fewer periods: `MAXIBITX_PLAYBACK_PERIODS=2`..`8` sets the depth at
   start-up (default 4), with the same measurement recipe.*
2. **Measure the T/R race**; add the right-channel delay line and
   `ext_ptt_delay_ms` if the first element is short. *Done in software on
   the Pi 4 (§8): spare at four periods, clipped at two and marginal at
   three, so both remedies are needed before the buffer shrinks. The
   five-dit test on air remains the check on the hardware side.*
3. **Weighting correction in `cw.c`** (§17), derived from the table — the
   straight key gets 1:1 immediately.
4. **`key_input.c`** (§16): the edge-woken thread, timestamped events,
   early TX request, mono-plug detection and reversal — first driving the
   existing straight key, which gains exact timing.
5. **`keyer.c` and a `keyer_test` harness**, written from §15: scripted
   events in, element sequences and timings out, 1–60 WPM, all modes, with
   `keyer_spec_model.c`'s golden cases as the acceptance test and
   `keyer_straight.c` built alongside to keep the seam honest.
6. **Text**: table, queue, spacing, `b`/`KY`/`KS`/`KEYSPD`, abort on
   paddle, the speed-dependent hang floor.
7. **Panel controls.**

## 13. Reproducing the numbers

```
python3 tools/keyer_study/envelope_study.py --ref PATH/TO/modem_cw.c --limiter
gcc -O2 -std=gnu11 tools/keyer_study/iambic_sim.c -o iambic_sim && ./iambic_sim
gcc -O2 -std=gnu11 tools/keyer_study/keyer_spec_model.c -o keyer_spec_model && ./keyer_spec_model
```

The first covers §3's envelope defect and all of §4, including the pass
through the real `tx_pipeline.c`; it needs numpy and scipy, and gcc with
fftw3f for `--limiter`. The second produces §5's tables and takes a few
minutes. The third runs §15's golden cases against the model of the
specification and exits non-zero if any fails. The reference file is `src/modem_cw.c` from
`github.com/MikeJohnshoy/sbitx`, branch `dev-54bugfixes`.

## 14. Decisions after review

Taken after reading §1–§13, and binding on the implementation:

- **The keyer is written from scratch, not ported.** The reference state
  machines work, but they fold mode logic into per-sample counter
  bookkeeping and carry behaviours nobody would specify on purpose (§3,
  §11). A keyer built around timestamped events has a simpler natural
  shape anyway: a handful of decision instants per element, each looking at
  a known paddle history. What *is* kept from the reference is what was
  never in question — the Morse table and its lookup, the dit as
  1.2 s ÷ WPM, and the text spacing rules. §15 is the specification, and
  `tools/keyer_study/keyer_spec_model.c` is that specification in
  executable form with its golden cases.
- **Paddle edges are timestamped by the kernel and read by a thread woken
  on each edge** (§16). Once every edge carries its own timestamp, how
  often the queue is emptied no longer affects keying accuracy or sidetone
  latency — elements can only be rendered once per audio block, and the
  timestamps place them exactly within it. What the read rate does decide
  is how soon the T/R sequence can start after the first closure. Waking on
  the edge is sooner than any polling interval, and costs nothing at all
  while the key is idle, where a 1 ms timer would wake a thousand times a
  second to find nothing.
- **The existing Blackman-Harris table stays**, unchanged, and 1:1
  weighting comes from correcting the mark length by an amount derived
  from the table (§17).
- **A mono straight-key plug in the stereo jack is detected and reported
  on the console** (§16).
- **Dot and dash paddles can be reversed** (§16).
- **The right-channel RF delay is deferred** (decided after §8's
  measurements). It is the remedy for stations with an amplifier on
  `EXT_PTT`, and nothing is built for it yet. Until it is, the default
  playback depth stays at four periods, which leaves the first element
  intact with the 20 ms amplifier delay in place. A station with nothing on
  `EXT_PTT` can opt in to low sidetone latency with
  `MAXIBITX_PLAYBACK_PERIODS=2` and `ext_ptt_delay_ms = 0` together; either
  one alone is no gain (four periods) or clips the first element (two
  periods at 20 ms).

## 15. Keyer specification

Written for a from-scratch implementation. Every paddle and key behaviour
below is exercised by a golden case in `keyer_spec_model.c`; the text,
speed-change and mode-change rules are specified here but not yet
modelled, and belong in the real harness's first cases.

**Units.** The dit, `T`, is 1.2 s ÷ WPM rounded to a whole sample:
`T = round(115200 / WPM)` at 96 kHz (1920 samples at 60 WPM, 115 200 at
1 WPM; the rounding is at most half a sample, 5 µs). A dot is a mark of
`T`, a dash a mark of `3T`. Every element the keyer times is followed by a
space of `T`, and the end of that space is the element's **decision
point**. An element is *in progress* from the start of its mark to its
decision point. All times are the kernel timestamps of the edges (§16),
so "before" and "during" are exact.

**Paddles.** Two contacts, dot and dash, after reversal (§16).

**Iambic A and B.**

- From idle: one paddle closed sends its element. Both closed sends the
  element of the paddle that closed *first*; a tie goes to the dot.
- A closure of the opposite paddle while an element is in progress sets
  that paddle's memory. Memories clear whenever an element starts.
- **Mode B only:** the opposite paddle being already closed when an element
  starts also sets its memory — so in B, closed at *any* point during the
  element counts, while in A only a fresh closure does.
- At the decision point: if the opposite paddle's memory is set or that
  paddle is closed, send the opposite element; otherwise if the same paddle
  is closed, send it again; otherwise stop.

That is the whole of the difference between the modes, and it produces
the textbook behaviour: release a squeeze during an element and A
finishes the element and stops, while B finishes it and sends one more
opposite element. Sending C (`-.-.`) with a dah-first squeeze, A needs the
release in the final dit; B allows it in the second dah. Both are golden
cases, as is the release in the second dah under A, which gives K
(`-.-`).

**Ultimatic.**

- Both closed: the paddle closed *most recently* wins, and its element
  repeats while both stay closed.
- One closed: its element repeats. So releasing the winning paddle hands
  control back to the one still held.
- A closure of the other paddle during an element sets its memory, and if
  neither paddle is closed at the decision point, one element of the
  memorised kind is sent. This is a choice rather than part of the classic
  definition: it means a quick tap is never silently dropped. Deleting that
  one rule gives the pure form.

**Bug.** The dot contact sends automatic dots (mark `T`, space `T`) for as
long as it is closed; releasing mid-dot completes that dot. The dash
contact keys directly with no timing at all, OR'd with the automatic dots,
exactly as the two contacts of a mechanical bug are wired.

**Straight key.** The key follows the contact. Either contact keys it,
unless one is excluded as a grounded ring (§16).

**Text.** Characters come from the Morse table; `T` between elements,
`3T` between characters, `7T` between words. A character the table lacks
is skipped and reported. Prosigns arrive in the keyer's own internal form,
each control surface translating its own convention (§9). Queued text
requests TX as soon as it is queued, since the keyer knows what is coming.
**Any paddle or key closure aborts the message:** the element in progress
completes, the rest of the queue is discarded, and the closure is then
handled as ordinary paddle input from its own timestamp.

**Changes while sending.** A speed change takes effect at the next element
start, never mid-element. A mode change takes effect once the keyer is
idle.

**Weighting.** The keyer times nominal 1:1 marks and spaces. The
correction that makes the transmitted envelope 1:1 at its 50% points
belongs to the envelope table, and is applied where the table is (§17).

**Golden cases** (`keyer_spec_model.c`, all passing against the model):

| mode | input | sends |
|---|---|---|
| iambic A | dot tapped from idle | `.` |
| iambic A | dot held for 5 dits | `...` |
| iambic A | dah-first squeeze, released in the last dit | `-.-.` |
| iambic A | same squeeze, released in the second dah | `-.-` |
| iambic B | same squeeze, released in the second dah | `-.-.` |
| iambic A, B | dah, with the dot tapped inside it | `-.` |
| iambic B | squeeze from idle, dot closed 0.1 dit first | `.-` |
| iambic B | squeeze from idle, dah closed 0.1 dit first | `-.` |
| ultimatic | dot held, dah added, dah released, dot released | `..--..` |
| ultimatic | dah held, dot tapped inside it | `-.` |
| bug | dot held for 5 dits, then a 2-dit dash closure | `...` + manual |
| straight | two closures | two manual marks |
| iambic A, 1 WPM | C, as above | `-.-.` |
| iambic B, 60 WPM | C, as above | `-.-.` |

Each case also checks that every mark the keyer times is exactly `T` or
`3T` and every space between elements exactly `T`, in samples.

## 16. Input path

**One line request, one thread.** `key_input.c` requests BCM 4 and BCM 5
together (§6): inputs, pull-ups, both edges, kernel debounce, and an event
buffer well beyond anything a paddle can fill. A dedicated thread blocks in
`poll()` on that one fd — no CPU at all while the key is idle — and on each
wake reads every waiting event. For each one it:

1. checks `line_seqno` for a gap, and warns on the console if the kernel
   ever dropped an event;
2. skips a line excluded as a grounded ring (below);
3. maps the physical line to *dot* or *dash*, applying reversal in paddle
   modes;
4. pushes `{timestamp, contact, closed/open}` into a single-producer,
   single-consumer ring — this thread writes, the audio thread reads,
   neither ever blocks;
5. on a closure in CW or CWR with TX idle, **requests TX immediately**, up
   to one block sooner than the audio thread would notice.

The debounce period is a bench setting against a real paddle. If the
kernel stamps a debounced edge at the end of the period rather than the
start, that costs latency equal to the period on both edges alike, and no
timing accuracy (§6).

**TX ownership.** Two threads can now start a transmission, so the
idle→active transition becomes a single atomic compare-and-swap in `cw.c`
that both paths go through; whichever wins calls `radio_set_tx(1)`, and the
other finds TX already active. Releasing TX — the hang timer — stays solely
with the audio thread, as today.

**Into the audio thread.** Once per block, where `cw_poll_key()` runs now,
the audio thread takes every event stamped up to this block's capture
timestamp (`t_read1`) and converts each to a sample offset measured from
the *previous* block's `t_read1`: the block about to be generated replays
the last block's interval, one block late and sample-exact (§6). An event
stamped after `t_read1` stays in the ring for the next block. One that
arrives too late for its interval — only possible if this thread was
starved — is placed at offset 0 rather than lost.

**Levels.** `key_input.c` keeps each contact's current state, read once at
start-up and then followed from the events. The mic-PTT path in USB and
LSB reads BCM 4's state from there.

**Mono plug in the stereo jack.** At start-up, and again whenever the keyer
mode changes, both contacts are sampled over a short window (a quarter of a
second is ample). If exactly one is closed for the whole window, it is
taken to be a ring grounded by a mono plug: that contact is excluded, the
input is treated as a straight key on the other, and the console says so —

```
key: BCM 5 closed since start-up - treating as a mono straight-key plug; that contact is ignored
```

If both are closed, the console says that instead and detection waits until
one opens. An excluded contact that later opens means the plug has changed,
which is reported and clears the exclusion. None of this needs to know which
jack contact is the tip: whichever line is held closed is the grounded one.

**Reversal.** One flag swaps which contact is the dot and which the dash,
in paddle modes only — for a straight key it has no meaning. The default
follows sbitx's mapping (BCM 5 dot, BCM 4 dash, §3) until the DE board's
jack has been checked with a meter. There is no Kenwood CAT command for it
(the TS-480 sets it from a menu), so it is a rigctld extension alongside
`NARROW` and `FFTFILT` — `u`/`U PADREV` — and a checkbox on the panel.

## 17. Weighting with the existing table

The table's rise crosses 50% at index `i50` = 314.6 of `N − 1` = 479, and
the fall, reading the same table backwards, crosses 50% at `N − 1 − i50`
samples after it starts. So a key-down of `n·T` produces a 50%-to-50% mark
of `n·T − C`, where

```
C = 2·i50 − (N − 1) = 150 samples (1.562 ms) for the current table
```

computed from the table at start-up, never written down, so a future table
change carries its own correction.

**The corrected mark length at each WPM** is therefore `n·T + C` samples of
key-down, and the following space `n·T − C`, for a dot (`n` = 1), a dash
(`n` = 3) and every element space alike. At 60 WPM that is 2070 samples
(21.56 ms) of key-down and 1770 (18.44 ms) of key-up, which the table's
edges turn into exactly 20.00 ms and 20.00 ms at the 50% points; at
20 WPM, 5910 and 5610 samples for 60.00 and 60.00 ms (§4's table, "with
compensation"). The correction is the same number of samples at every
speed — 0.13% of a dit at 1 WPM, 7.8% at 60 — because it is a property of
the edge, not of the speed.

**Where it is applied: in `cw.c`, as "every fall starts `C` samples after
the key-up"**, which is exactly the mark-length correction above, done in
one place. Doing it in the envelope stage rather than the keyer keeps the
table's property next to the table — the keyer then needs no knowledge of
which table is in use — and it covers the straight key and the bug's
manual dash, which have no WPM to correct against.

It holds while the fall completes before the next rise, `T − C ≥ N − 1`,
so `T ≥ 629` samples: up to 183 WPM, three times the required range.
