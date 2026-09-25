# CW break-in timing, and what I2C costs

Status: **investigation only — no code changed.** Two questions were asked
and answered by reading the code and doing the arithmetic: what I2C
activity does to RX/TX performance, and what actually limits break-in CW.
One change is *proposed* at the end (§9) and not implemented.

Where a number here is derived from the code it is exact. Where it depends
on something not visible in the source — above all the bit-banged bus's
real clock rate — it is labelled as an assumption with a way to measure it.
Two claims are predictions rather than measurements, and say so.

The headline of each half is the same shape: the thing that looks like it
should be the limit isn't, and the thing that is the limit is a delay
inherited from hardware this board doesn't have.

## 1. Where I2C lives in maxibitx

Every I2C caller, the thread it runs on, and how often:

| caller | what | thread | cadence |
|---|---|---|---|
| `si5351bx_init()` | reset, clocks off | main, startup | once |
| `radio_hw_detect_version()` | board revision probe, addr 0x8 | main, startup | once |
| `radio_hw_ina260_configure()` | power monitor config, addr 0x40 | main, startup | once |
| `radio_tune_to()`, RIT setters | clk2 | control (rigctld / CAT / HPSDR) | per dial or RIT change |
| `radio_tx_apply()` | clk1 **and** clk2 | `radio_tx_worker` | twice per T/R cycle |
| `sound_set_rx_capture()`, `sound_set_tx_drive()` | WM8731 mixer | `radio_tx_worker` | twice per T/R cycle |

Two things follow, and they are the answer to the question.

**Nothing polls.** The INA260 is configured at startup and never read;
`radio_hw_detect_version()` runs once. There is no periodic I2C traffic at
all — the bus is idle except when something is tuned or keyed.

**Nothing on the real-time path touches I2C.** The audio thread is
`audio_loop()`: `snd_pcm_readi` → `sound_process()` → `cw_poll_key()` →
`snd_pcm_writei`. Traced through, none of those reach a mixer call or
`i2c_*`. `cw_poll_key()` calls `radio_set_tx()`, which sets `in_tx`
synchronously and hands the actual sequence to a dedicated worker thread
(`radio.c`'s `tx_pending`/`tx_cond`) precisely so the audio thread never
blocks on it. So I2C cannot cause an xrun by stalling audio. That class of
failure is designed out rather than merely absent today.

The WM8731 is reached through ALSA mixer calls, not direct register writes
— the kernel's `wm8731` driver does the I2C on maxibitx's behalf, on the
hardware bus. Only the si5351 (and the two startup probes) go through
`i2c.c`.

## 2. What a frequency change costs

`si5351bx_setfreq()` is not one transaction. It calls `setup_pll()` (8
single-byte register writes) and `setup_multisynth()` (9), so **one clock
change is 17 separate I2C transactions**, each `START` + address + register
+ value + `STOP`.

The bus matters here: the si5351 is on bus 22, the `i2c-rtc-gpio` overlay,
which is `i2c-gpio` — bit-banged by the kernel, not a hardware peripheral.
So this is CPU time, not DMA.

At a **nominal 100 kHz** (an assumption — see below):

| | transactions | time |
|---|---|---|
| one register write | 1 | ~280 µs (≈28 bit periods) |
| one `si5351bx_setfreq()` | 17 | ~4.8 ms |
| one T/R transition (clk1 + clk2) | 34 | ~9.5 ms |
| a full T/R cycle (down and back) | 68 | ~19 ms |

**The 100 kHz is not measured.** `i2c-gpio`'s rate comes from the
overlay's `delay-us` property, and kernel bit-banging is subject to
scheduling. To measure it without changing anything:

```sh
time for i in $(seq 200); do i2cget -y 22 0x60 0 >/dev/null; done
```

Divide by 200 for the per-transaction figure, then scale the table. If it
comes out far from ~280 µs, every number in this section moves with it.

### Finding: half the T/R traffic is redundant

`radio_tx_apply()` writes clk2 on the way into transmit and again on the
way out:

```c
/* TX */ si5351bx_setfreq(2, freq_hdr + xtal_filter_center);
/* RX */ si5351bx_setfreq(2, freq_hdr + rit_applied_hz() + xtal_filter_center);
```

With RIT at zero — the normal case — those are the **same value**. clk2 is
being rewritten to the frequency it already holds: 17 transactions, ~4.8 ms,
each way, ~9.5 ms per T/R cycle spent achieving nothing.

clk1 genuinely does change (RX `xtal_filter_center + RX_IF_FREQ_HZ` =
40,036,400 Hz against TX `bfo_freq` = 40,035,000 Hz, a 1400 Hz step), so
half the traffic is real.

Worth noting for whoever acts on this: `si5351v2.c` already declares
`uint32_t plla_freq, pllb_freq` and nothing uses them. A skip-if-unchanged
cache has a home waiting for it.

### Finding: an unbounded-ish tail in the retry loop

`i2cSendRegister()` retries a failed write up to `I2C_SEND_MAX_ATTEMPTS`
(20) times with `usleep(1000)` between attempts. One flaky register costs
up to 20 ms; a pathological `si5351bx_setfreq()` could reach 17 × 20 ms =
340 ms. It runs on the worker thread, so it cannot stall audio — but a
T/R transition taking a third of a second would be very visible on air, and
the failure would announce itself only as `Repeating I2C #n` on the
console. Not a problem observed in practice; recorded because the bound
exists and is large.

## 3. How sbitx 5.401 differs, verified

The question came up because sbitx is believed to poll its power and
voltage sensors continuously. Checked against `afarhan/sbitx` at v5.401
rather than assumed, the answer splits in two.

**The SWR/power read is on the audio thread, once per TX block.**
`read_power()` (`sbitx.c:1798`) is called from inside `tx_process()`
(`sbitx.c:1861`, call at 2222) — the TX audio processing function. It does
`i2cbb_read_i2c_block_data(0x8, 0, 4, response)`, a **bit-banged** 4-byte
read of the SWR bridge. And `i2cbb.c`'s bit-bang is a userspace busy-wait:

```c
delayTicks = 400;   // empirically twice the value that just starts to NACK
static void i2c_delay() { for (index = 0; index < delayTicks; index++) ; }
```

A spin loop between every bit transition, driving the pins through
wiringPi `digitalWrite`, with no yield — inside the TX audio callback. It
is gated on `if (!in_tx) return;`, so it costs nothing on receive, but
during transmit it runs every block.

**On a board without the SWR bridge it still bit-bangs, then fails.**
`i2cbb_read_i2c_block_data()` writes the address, samples for an ACK, and
returns −1 when none comes. So a DE board pays the address phase — `START`
+ 8 bits + ACK slot + `STOP`, roughly a quarter of a full read — on every
TX audio block, forever, for a device that isn't fitted. That is the
configuration this project's author runs.

**The INA260 is far less than continuous, and this corrects the
impression.** `check_read_ina260_cadence()` (`sbitx_gtk.c:9431`)
rate-limits the read to once per second, it runs on the GTK UI thread
rather than the audio thread, and it is gated on `has_ina260`, which comes
from a UI toggle whose default is `OFF`. Four bit-banged transactions per
second on a non-real-time thread, usually disabled. Not the problem the
power read is.

**maxibitx needs none of it, by construction.** The ALC is open-loop: the
limiter's ceiling is computed from `POWER`, `max_power` and
`full_scale_power` (see
[`tx_power_calibration.md`](tx_power_calibration.md) and
[`tx_test_tones_and_alc.md`](tx_test_tones_and_alc.md)) and the limiter
acts on the digital envelope before the DAC. Nothing downstream is
measured, so there is nothing to poll. The board's lack of a bridge is not
a limitation being worked around — the design never asks.

## 4. The break-in budget

`cw_poll_key()` runs once per audio block from the audio thread:
`PERIOD_FRAMES` (1024) at 96 kHz = **10.667 ms**. That is the granularity
of everything keying-related.

| term | key-down → RF | key-up → RX |
|---|---|---|
| notice the key edge | 0–10.67 ms (mean 5.3) | 0–10.67 ms |
| hang timer, `CW_HANG_POLLS` = 28 | — | **298.7 ms** |
| `sound_set_rx_capture()` / `sound_set_tx_drive()` | one mixer call | one mixer call |
| clk1 + clk2 rewrite | ~9.5 ms | ~9.5 ms |
| `usleep` between `EXT_PTT` and `TX_LINE` | **20 ms** | 5 ms |
| TX DSP already in flight | 18.0 ms | — |

The DSP term breaks down as block buffering 10.67 ms + linear-phase FIR
group delay (M=1025, so (M−1)/2 = 512 samples) 5.33 ms + ALC look-ahead
2.0 ms = 18.0 ms. It is latency the tone has already accumulated by the
time it reaches the DAC, not additional waiting.

## 5. The Si5351 is not the limit

This was the specific suspicion, and it can be dismissed on three
independent grounds.

**No PLL reset is ever issued on a retune.** `si5351_reset()` — register
177, `SI_PLL_RESET` — is called from exactly two places: `si5351bx_init()`
and once more in `maxibitx.c` at startup. A frequency change never resets
the PLL, so there is no forced re-lock and no lock-time penalty to pay.

**The steps are tiny.** clk1 moves 1400 Hz at 40 MHz — 35 ppm. clk2 does
not move at all when RIT is zero (§2). Whatever the device's settling is
for a 35 ppm change of the feedback divider, it is not a large-signal
re-lock.

**Delivery dominates it anyway.** Getting the 17 registers across a
bit-banged bus takes ~4.8 ms. Any plausible electrical settling for a
35 ppm step is orders of magnitude below that, so the software time to
*describe* the new frequency swamps the hardware time to *reach* it.

And all of it is behind a 20 ms `usleep`, which is the next section.

## 6. The 20 ms is external-amplifier sequencing, not T/R settling

`radio_tx_apply()` reads:

```c
radio_hw_set_ptt(1);
usleep(20000); // let PTT assert before keying the relay
radio_hw_set_tx_relay(1);
```

The comment says "relay", and the board's T/R switch is solid state — so
this looks at first like a delay waiting on hardware that doesn't exist.
It isn't, quite. The two calls drive **different lines**:

- `radio_hw_set_ptt()` → `EXT_PTT`, BCM 12 — the *external* PTT line, which
  leaves the radio for an amplifier or accessory.
- `radio_hw_set_tx_relay()` → `TX_LINE`, BCM 23 — the radio's own T/R
  control.

So the 20 ms sits between telling an external amplifier to switch and
producing RF. That is textbook amplifier sequencing, 20 ms is a typical
amp relay figure, and hot-switching an amp relay is how amplifiers are
destroyed. The delay is protecting equipment that may not be connected.

The consequence for break-in is that a station running barefoot pays 20 ms
per transmission for nothing, and it is the largest single controllable
term in the TX-up path.

**Unconfirmed, and it matters:** what `TX_LINE` gates on a DE board beyond
the solid-state switch itself. If it also brings up PA bias, that stage may
want settling time of its own, which the 20 ms currently supplies by
accident. Anyone reducing this should check that before trusting §9.

## 7. Prediction: the first element is truncated

Not measured — derived, and stated separately for that reason.

`cw_poll_key()` sets `tx_active` as soon as the key is seen, and the audio
thread begins generating the keyed tone on the next block. But
`sound_set_tx_drive(TX_MASTER_VOL)` — the step that unmutes the exciter
feed — is the *last* action of the TX-up sequence. Between those two
moments the tone is being generated into a muted path.

Summing §4's TX-up terms, that gap is roughly 30–40 ms. At 20 WPM a dit is
60 ms; at 25 WPM, 48 ms. So **the leading half or more of the first element
of every transmission should be missing from the air**, while sounding
complete in the sidetone.

It is cheap to test with a remote receiver and no code changes: send five
evenly spaced dits and compare the first against the rest. If the first is
short and the others are not, this is confirmed; if all five are equal,
something in this reasoning is wrong and worth finding.

## 8. Why full break-in is out of reach, and that being fine

A T/R round trip costs ~44 ms of sequencing alone — 20 ms up, ~9.5 ms of
clocks, 5 ms down, ~9.5 ms of clocks — before the 18 ms of TX DSP and the
10.67 ms keying quantisation. A 20 WPM inter-element space is 60 ms. True
QSK, returning to receive between elements, would need the amp delay gone,
the redundant clock writes gone, and the key polled faster than once per
audio block: three changes, one of which (sub-block key polling) means
restructuring how `cw.c` is driven.

**That is not the goal.** Semi break-in with the 298.7 ms hang is the
accepted design. Recording the arithmetic so the choice is visible as a
choice, not an accident, and so nobody re-derives it later.

## 9. Proposed, not implemented: `ext_ptt_delay_ms`

The one change this investigation recommends. Make the §6 delay a
`hw_settings.ini` value instead of a compiled-in 20 ms:

```ini
# Milliseconds to hold EXT_PTT before TX_LINE, so an external amplifier's
# relay settles before RF appears. 0 if nothing is connected to EXT_PTT.
ext_ptt_delay_ms=0
```

A top-level key alongside `bfo_freq` and `full_scale_power`, defaulting to
the current 20 ms if absent so no existing installation changes behaviour
by being upgraded. An operator driving an amplifier sets it to 20 or more;
one running barefoot sets 0 and recovers that much of the first element.

Two cautions for whoever implements it. It should almost certainly be
clamped to a sane maximum, since a mistyped value lands directly in the
keying path. And the RX-side `usleep(5000)` is a separate question with a
separate justification — it is not obviously the same delay in reverse and
should not be folded into the same setting without its own look.

## 10. What is still open

- **The bus rate is assumed, not measured** (§2). Every I2C figure scales
  with it.
- **The first-element truncation is a prediction** (§7), awaiting the
  five-dit test.
- **What `TX_LINE` gates** beyond the solid-state switch (§6) — the one
  fact that decides whether `ext_ptt_delay_ms=0` is actually safe on a DE
  board.
- **Whether the redundant clk2 write is worth removing** (§2). It costs
  ~9.5 ms per T/R cycle and the cache variables already exist, but it is
  a change to the TX sequencing path, which is the last place to want an
  unforced error.
- **Nothing here was measured on hardware.** It is all static analysis plus
  arithmetic. The measurement recipes are in §2 and §7.
