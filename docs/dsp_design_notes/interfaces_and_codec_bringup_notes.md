# Interfaces and codec bring-up: what was learned

Status: record, not maintained. Gathered on 23 November 2026 from the
retired docs 01, 04 and 05 when they were merged into
[`00_intro.md`](../00_intro.md), which describes how these parts work
now; [`06_api.md`](../06_api.md) is the client-side reference. What is
here is why they are built the way they are: the bugs met on the way,
and the fixes that shaped the current code. Source comments point here
for that history.

## 1. One entry point for tuning and transmit

`radio_tune_to()` and `radio_set_tx()` (`radio.c`) are the only
functions that change frequency or key the transmitter in hardware.
rigctld, the Kenwood CAT surface, HPSDR's MOX, TCI and the front-panel
knobs all call them rather than touching GPIO or the si5351 themselves,
so adding a control surface never adds a second place that can leave
the hardware in an inconsistent state. Each surface logs its own
result, since it knows which command caused the change; the two
functions print nothing.

## 2. HPSDR Protocol 1

### Inbound: three bugs in the command parser

Frequency and mode first lived entirely in rigctld; the only thing
`hpsdr_p1.c` read from the inbound stream was the MOX bit, since some
SDR applications key PTT through the I/Q link while using CAT for
everything else. (It later also followed the client's RX1 frequency, C&C
address 0x02, while receiving, and retuned to its TX frequency, address
0x01, on MOX.) Getting the parser right took three fixes, all in
`process_ep2_frame()` and `handle_command()`:

- An early version with no exact-length checks let torn or malformed EP2
  frames decode into wildly varying C&C addresses with the MOX bit
  effectively random. Strict per-packet-type length checks, modelled on
  piHPSDR's `hpsdrsim.c`, fixed it.
- Reading MOX from every C&C address, as `hpsdrsim.c` does literally,
  still picked up unrelated register data as PTT. It is now read from
  address 0 only.
- Comparing the network's MOX bit against the shared `in_tx`, which the
  local key also writes, let an unchanged, stale network value look like
  a fresh request the moment the key's hang timer released TX, latching
  TX on after a single key press. `net_mox` now tracks only the
  network's own last-seen bit, so only a change in what the network
  sends can act.

### Outbound: the pacer, and two failed attempts before it

`hpsdr_send_iq()`, called once per audio block, first packetised and
sent inline from the audio thread, firing a block's nine or so packets
within about 1 ms and then nothing for 10 ms. SDR Console tolerated it;
SparkSDR's jitter buffer apparently did not (the leading suspect for a
~170 ms audio warble it showed). Two fixes were tried and reverted:

- `usleep()` between packets on the audio thread caused continuous
  xruns: a capture period is wall-clock time already spent, so sleeping
  on top of it is a permanent rate deficit, and the capture ring
  overran within about six blocks.
- A mutex-protected queue reproduced the xruns for a different reason:
  a plain mutex shared between the audio thread at `SCHED_FIFO` maximum
  priority and an ordinary-priority pacer is a priority-inversion trap,
  since the default Linux mutex has no priority inheritance.

What stayed: a lock-free single-producer, single-consumer ring that the
audio thread only appends to (dropping a sample rather than ever
waiting), drained by a pacer thread that sends one packet every
1.3125 ms against an absolute wake time. iq_stream, the USB gadget and
TCI all use the same pattern
([`iq_stream_design.md`](iq_stream_design.md) for iq_stream's, with its
separately locked subscriber list, safe because no real-time thread
touches it).

### Outbound status bytes: the "buzzsaw"

Each frame to the host starts with C0, whose layout the protocol fixes:
bits 7:3 say which status field C1-C4 carry, bit 2 is the CW dot
paddle, bit 1 the dash paddle, bit 0 PTT. Until 22 September 2026 the
code put a 0-31 counter in bits 5:1 (the host-to-radio layout), so the
dash bit toggled every frame and the dot bit every other one - 750 of
1,000 frames in a bench capture carried a paddle closure - and fields 0
and 2 carried the dial frequency, which a client reads as the ADC
overload flag and firmware version. SDR Console ignored it; SparkSDR's
receive audio had a loud buzzsaw across the dial. Status fields now
rotate 0-4 one per frame, as real hardware and sbitx do, all zero but a
firmware version in field 0. Confirmed with SparkSDR the same day: the
buzz was gone.

## 3. The USB gadget

The gadget first carried raw baseband I/Q, for SDR applications to
demodulate. That path was removed
([`ARCHITECTURE.md`](../ARCHITECTURE.md) §10 step 9): the gadget now
carries demodulated 48 kHz audio in both directions, for WSJT-X and
similar programs, as a real radio's USB codec would. An early revision
advertised two channels per direction with the sample duplicated;
mono both ways replaced it (§10 step 11) once WSJT-X bring-up showed
mono is the normal case for a digital-mode interface, as on a SignaLink.

The Kenwood CAT surface exists for FLRig, which speaks CAT only over a
serial port; the gadget's CDC-ACM port saved building a TCP-to-serial
bridge. The TS-480 subset was chosen because it is what the QRP Labs
QMX emulates, for the same reasons: widely supported, and a command set
that leaves out what such a radio can't do. Under FLRig's generic
Kenwood profile, its split "VFO B" box was mistaken for a RIT readout;
the QMX profile sends only the implemented commands and was confirmed
to move maxibitx's RIT end to end. FLRig polls every get several times
a second, which flooded the console until gets were logged only when
their answer changed.

## 4. The WM8731 codec, learned the hard way

- **`Line` is a switch, `Capture` is the gain.** `Line` looked like a
  gain control, but `amixer -c 0 sget 'Line'` shows a plain on/off
  switch. The analog gain ahead of the ADC is `Capture`
  ([`rx_gain_and_level_calibration.md`](rx_gain_and_level_calibration.md)
  §3).
- **`sound_mixer()` must set every capability an element has.** It was
  an else-if chain (switch, or volume, or enum). `Master` has a mute
  switch and a volume under one name, so only its switch was ever set,
  and its volume stayed at the codec's power-on value whatever was
  asked. Each capability is now set independently, a missing element
  name is logged, and `sound_mixer_dump()` prints what an element
  supports.
- **`Master` left and right go to different places.** Left drives the
  local speaker and headphones; right drives the exciter. They once
  shared one value, so every T/R transition clobbered whichever wasn't
  in use. Left is now set once at startup, and right raised only during
  transmit ([`rx_audio_demod_design.md`](rx_audio_demod_design.md) §9).
- **Left runs at 100%.** It started at 70 and was raised after an
  on-air report that 100% volume was still too quiet. The headroom lives
  upstream in `rx_audio.c`, and listening volume is its digital
  `rx_volume`, which was rescaled once 100% here made it too loud.
- **The mic comes in on `Capture` right, probably not through `Mic`.**
  With `Input Mux` on `Line`, the mic most likely reaches the ADC on the
  line input's right channel, and `Mic` may do nothing on this board
  (unconfirmed). That is why the transmit capture mute touches the left
  channel only: muting both silenced USB and LSB transmit audio on the
  first on-air SSB test (`ARCHITECTURE.md` §10 step 8). Why it is the
  `Line` switch and not `Capture`'s gain:
  [`rx_gain_and_level_calibration.md`](rx_gain_and_level_calibration.md)
  §8.

## 5. Threads and the console

**The TX worker thread.** `radio_set_tx()` once ran the T/R sequence
inline: PTT and relay settling `usleep()`s and an ALSA mixer call on the
caller's thread. Harmless from a network thread, but `cw.c` calls it from
the audio thread, where blocking 20 ms or more guaranteed a missed
capture period (`sound: xrun, recovering` on every key transition) and
made the key feel sluggish. `radio_set_tx()` still updates `in_tx` at
once, since every guard that reads it needs the new state immediately,
and hands the slow sequence to a worker thread through a mutex,
condition variable and pending flag, so no caller blocks. What the
sequence costs, and which of its waits are real:
[`cw_breakin_and_i2c_costs.md`](cw_breakin_and_i2c_costs.md).

**No status line.** A single-line, redraw-in-place frequency and TX
display (`status.c`) was removed. The console instead reports each
change as it happens, from the surface that made it (`rigctl:`, `cat:`,
`tci:`, `hpsdr:`, `knobs:`), and later stopped reporting reads, so it
shows what happens to the radio rather than what clients ask.

**The INA260.** `radio_hw_ina260_configure()` writes `0x6127`
(continuous mode, default averaging) to the monitor at I2C `0x40`, and
`read_voltage_current()` reads voltage and current at 1.25 mV and
1.25 mA per bit, treating an all-ones current as invalid. Nothing calls
it yet.
