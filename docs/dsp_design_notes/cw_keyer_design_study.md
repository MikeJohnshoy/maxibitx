# CW keyer: a feasibility study

Status: **study only — nothing is implemented.** Written as the precursor
to building a keyer, to find out before any code exists whether the
requested design holds up, and where it doesn't, what to do instead.

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
newly written block reaches the DAC about 29 ms later. So key-to-sidetone
latency, derived, not measured:

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

This is also why the figure above is labelled derived: `F` is set once by
the start-up phase between capture and playback, and one call to
`snd_pcm_delay()` in the existing `MAXIBITX_LOOP_TIMING` report would
replace the arithmetic with a measurement.

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
key_input.c   GPIO v2 edge events for BCM 4 and BCM 5, drained once per block,
              timestamps mapped to sample offsets. Knows nothing of Morse.
                 │  events[] for this block
                 ▼
keyer.c       mode state machines, WPM, reverse, text queue and Morse table.
              Emits a key-down/up value per sample for the block, plus
              "TX wanted". Knows nothing of GPIO or audio.
                 │  key[n]
                 ▼
cw.c          envelope (with the §4 compensation) and oscillator, per sample;
              hang timer and radio_set_tx(). As today, with key[i] in place
              of the single key_down.
```

The audio-thread interface is one call per block:

```c
// keyer.h
int keyer_run_block(const struct key_event *ev, int n_events,
                    uint8_t *key, int n);   // returns 1 while TX is wanted
```

plus control-side setters (`keyer_set_mode()`, `keyer_set_wpm()`,
`keyer_set_reverse()`, `keyer_send_text()`, `keyer_abort()`,
`keyer_busy()`), called from the rigctld and CAT threads. Text reaches the
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

- **The playback queue depth** (§7) is derived. Measure it with
  `snd_pcm_delay()` before designing around it.
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
  path's paddle handling (§3). Port to a written specification of each mode
  with `iambic_sim.c`'s scenarios as regression cases, rather than
  transliterating.
- **Whether 5 ms of average latency matters** between plain polling and
  timestamped edges is a feel question. If it does, the start-from-idle
  refinement in §6 recovers it.

## 12. Suggested order of work

Each step stands alone and is useful even if the keyer is never finished.

1. **Measure the playback queue** with `snd_pcm_delay()`, and try three
   and two periods on a Pi 4 and a Pi Zero 2W. Nothing else in this list
   decides the feel of the paddle as much.
2. **Run the five-dit test**; add the right-channel delay line and
   `ext_ptt_delay_ms` if the first element is short.
3. **Weighting compensation in `cw.c`**, derived from the table — the
   straight key gets 1:1 immediately.
4. **`key_input.c` with timestamped edges**, first for the existing
   straight key: exact timing, plus the mono-plug check.
5. **`keyer.c` and a `keyer_test` harness**: scripted events in, element
   sequences and timings out, 1–60 WPM, all modes, with `iambic_sim.c`'s
   scenarios as golden cases and `keyer_straight.c` built alongside to
   keep the seam honest.
6. **Text**: table, queue, spacing, `b`/`KY`/`KS`/`KEYSPD`, abort on
   paddle, the speed-dependent hang floor.
7. **Panel controls.**

## 13. Reproducing the numbers

```
python3 tools/keyer_study/envelope_study.py --ref PATH/TO/modem_cw.c --limiter
gcc -O2 -std=gnu11 tools/keyer_study/iambic_sim.c -o iambic_sim && ./iambic_sim
```

The first covers §3's envelope defect and all of §4, including the pass
through the real `tx_pipeline.c`; it needs numpy and scipy, and gcc with
fftw3f for `--limiter`. The second produces §5's tables and takes a few
minutes. The reference file is `src/modem_cw.c` from
`github.com/MikeJohnshoy/sbitx`, branch `dev-54bugfixes`.
