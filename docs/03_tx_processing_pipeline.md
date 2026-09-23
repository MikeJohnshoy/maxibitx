# 03 — TX processing pipeline

This documents the transmit signal path, from the audio source for each
mode to the antenna, the same way
[`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md) covers
receive. TX runs through the same two si5351 clocks, the same crystal
filter and the same LPF relays as RX, with the signal flowing the other
way; the digital part in front of them is `tx_pipeline.c`, one shared
FFT pipeline for every mode.

Status: CW is on-air verified - on frequency, image suppression as
predicted, and a flat ~5 W across all nine bands. USB and LSB from the
mic have been on the air: both put out power and each lands on the
correct side of the dial. Their carrier placement was measured with the
test-tone generator on 2026-09-22, corrected, and the correction read
back on air the same day
([`dsp_design_notes/tx_test_tones_and_alc.md`](dsp_design_notes/tx_test_tones_and_alc.md)).
DIGITAL (WSJT-X over the USB gadget) is confirmed by a two-way FT8
contact on 2026-09-23 - see "What the FT8 contact proves" below.

The earlier CW-only scheme this pipeline replaced (a second, IF-shifted
oscillator in `cw.c` plus a matching clk2 correction in `radio.c`) is
kept for reference in
[`dsp_design_notes/tx_direct_to_dac_cw_history.md`](dsp_design_notes/tx_direct_to_dac_cw_history.md).
The derivations and bench numbers behind the current design are
[`ARCHITECTURE.md`](ARCHITECTURE.md) §10 steps 4, 5, 8 and 9.

## The chain, end to end

```
  TX audio source, by mode (sound.c's audio_loop())
     CW/CWR:  cw.c's 700 Hz sidetone x keying envelope (fixed full scale)
     USB/LSB: mic (WM8731 right capture channel) x mic_tx_gain
     DIGITAL: WSJT-X audio from the USB gadget, 48 -> 96 kHz
              (upsample48k.c), at whatever level the host sends
     |            mic_tx_gain and the host's level are THE DRIVE - how hard
     |            the signal is pushed into the ceiling below. They can
     v            exceed full scale by 20 dB, which is the limiter's work.
  tx_pipeline.c, one 1024-sample block at a time (96 kHz):
     FFT -> 300-3000 Hz bandpass -> zero one sideband -> rotate bins
     (the IF shift) -> inverse FFT -> peak limiter (ALC) -> real part
     |                                     ^
     |                                     |
     |    ceiling, per block from sound.c -+
     |      = sqrt(POWER x max_power / full_scale_power)
     |        POWER: the operator's control, rigctld l/L RFPOWER
     |        max_power, full_scale_power: watts, from hw_settings.ini
     |
     |    The limiter holds the envelope 2*|out_c| at or below that
     |    ceiling by applying a GAIN - min(1, ceiling/envelope) - never
     |    by clipping. It only reduces, so quiet audio stays quiet. A
     |    constant envelope at or below the ceiling (CW key-down, a
     |    steady tone) doesn't move the gain at all. Costs 2 ms of
     |    look-ahead delay; reports its gain reduction as l ALC.
     |
     |            result: a real IF near 22.6 kHz, envelope <= the ceiling
     v
  WM8731 DAC, right channel = exciter feed
     |          x band scale x TX_GAIN_CORRECTION  <--- the calibration
     |            that turns an amplitude into watts. Fixed, and the one
     |            thing downstream of the limiter, so the ceiling holds.
     |        left channel = local sidetone/mic monitor (never reaches the PA)
     v
  Mixer 2  <---  clk1 = bfo_freq (40,035,000 Hz) while transmitting
     |           difference product lands on the crystal filter center;
     |           the sum product lands ~45 kHz above it, in the stopband
     v
  Crystal filter, centered at xtal_filter_center (40,012,400 Hz)
     |
     v
  Mixer 1  <---  clk2 = freq_hdr + xtal_filter_center (no RIT during TX)
     |
     v
  PA (fixed gain)
     |
     v
  LPF bank (radio_hw.c: set_lpf_40mhz, same relays RX uses)
     |
     v
  Antenna
```

Everything from the DAC onward is analog. maxibitx's code sets two
things: the waveform written to the DAC's right channel, and the
sequencing that switches the path in and out (`radio_tx_apply()`,
below). Output power is set entirely by the amplitude of that waveform.

The limiter is the dividing line in that chain. Everything above it —
mic gain, the host's audio level — decides how hard the signal is
driven; everything below it is fixed calibration that turns an
amplitude into watts. Nothing an operator or a CAT client can change
may sit below the limiter, or the ceiling stops being a ceiling, which
is why `POWER` moves the threshold rather than scaling the output and
why `TX_MASTER_VOL` stays a mute rather than becoming a level control.
"Setting power" has the whole account.

## Keying and PTT

Every TX path ends up in `radio_set_tx()` (`radio.c`). It sets `in_tx`
immediately, so every other thread sees the new state at once, and
hands the slow hardware sequence to a dedicated worker thread (see
[`05_process_and_threading_model.md`](05_process_and_threading_model.md)
for why). Who calls it:

- **The key/PTT line** (`CW_KEY`, BCM4), polled once per audio block by
  `cw_poll_key()` (`cw.c`). In CW and CWR it's a straight key with semi
  break-in: a hang timer (`CW_HANG_POLLS`, ~300 ms) holds TX through
  the gaps between elements so the relay doesn't chatter. In USB and
  LSB the same line is a mic PTT switch - TX follows the switch, no hang
  timer. In DIGITAL the line is ignored.
- **Remote PTT/MOX:** rigctld `T` (`hamlib.c`), Kenwood `TX`/`RX`/`TQ`
  on the gadget's serial port (`usb_gadget.c`), and HPSDR MOX
  (`hpsdr_p1.c`, which first retunes to the client's TX frequency if
  it differs). While the local key/PTT line holds TX, remote
  requests to change it are ignored - the local key wins.

**Band limits.** `radio_set_tx()` refuses to transmit when the dial
sits outside every `[tx_band]` range in `data/hw_settings.ini`, which
covers all of those sources at once rather than each having to check
for itself. Returning to receive is never refused. rigctld's `T`
answers `RPRT -1`; the other surfaces have no error reply, so the
refusal is reported on the console instead — by `maxibitx.c`'s idle
loop rather than at the point of refusal, because the straight key's
path runs on the real-time audio thread and may not do I/O. A key held
down out of band therefore logs once a second, not once per poll.

If no `[tx_band]` entries were loaded at all — no `hw_settings.ini`, or
one without them — nothing is refused. An empty table means nothing is
calibrated, not that nothing is allowed; refusing everything would
leave an uncalibrated board unable to transmit at all, which is the
worse failure.

Which audio actually reaches the exciter is decided in `audio_loop()`:
it transmits when the key/PTT line has TX asserted
(`cw_tx_active()`), when `in_tx` is set in DIGITAL, or when `in_tx` is
set with the test-tone generator on (below). Otherwise remote PTT in
CW, CWR, USB or LSB keys the radio but sends silence - there's no
remote audio source in those modes, so a CAT or HPSDR MOX in CW doesn't
produce a carrier.

`radio_tx_apply(1)` then runs, in order: mute RX capture
(`sound_set_rx_capture(0)`, before any TX RF exists), set clk1 to
`bfo_freq` and clk2 to `freq_hdr + xtal_filter_center`, raise PTT
(`EXT_PTT`), wait 20 ms, switch the T/R relay (`TX_LINE`), and open the
exciter feed (`sound_set_tx_drive(TX_MASTER_VOL)`). Returning to RX is
the reverse: exciter feed to 0, PTT off, 5 ms, relay off, clk1 back to
`xtal_filter_center + RX_IF_FREQ_HZ`, clk2 back to its RX value (with
RIT), and RX capture unmuted last, once the relay has settled.

## Stage by stage

Following one CW key-down at a dial frequency of 7,030,000 Hz, with
`bfo_freq` and `xtal_filter_center` at this board's values (40,035,000
and 40,012,400 Hz).

**Audio source.** `sound.c` fills one block of TX audio per ~10.7 ms
period:

- CW: `cw_get_sample()`, a 700 Hz tone (`CW_PITCH_HZ`) times a 5 ms
  Blackman-Harris attack/decay envelope, so key transitions don't
  click. The same tone is the local sidetone, so what you hear is
  exactly what's being sent.
- USB/LSB: the mic, from the WM8731's right capture channel, converted
  to about ±1 full scale (`MIC_TX_INPUT_SCALE`) and multiplied by
  `mic_tx_gain`. That gain is live-adjustable with rigctld `L MICGAIN`
  or the control panel's Mic Gain slider; on air the right setting was
  found somewhere under ~5.
- DIGITAL: WSJT-X's audio, pulled from the USB gadget at 48 kHz
  (`uac_pull_audio_tx()`) and upsampled 2x (`upsample48k.c`). Gaps are
  filled with silence so the filter history stays continuous.
- Test tones, in any mode, while the generator is on (`tone_gen.c`,
  rigctld `U TONE 1|2`): a full-scale 1000 Hz tone, or 700 + 1900 Hz at
  half amplitude each. Both peak at full scale, so both reach the same
  PEP as CW; the two-tone's average power is half. Any PTT source keys
  them, and the sideband still follows the mode. After 30 s in transmit
  the generator turns itself off and PTT drops. Design and measurement
  procedure:
  [`dsp_design_notes/tx_test_tones_and_alc.md`](dsp_design_notes/tx_test_tones_and_alc.md).

For the example, the input is a 700 Hz tone.

**`tx_pipeline.c`.** One shared instance serves every mode. Each
1024-sample block goes through an overlap-save FFT filter (N = 2048,
46.875 Hz per bin) in six steps:

1. **Bandpass**, 300-3000 Hz (`filter_tune_real()`, Kaiser β = 5). It
   keeps both the positive- and negative-frequency images of that band,
   so either sideband is available to the next step.
2. **Sideband zero.** A real audio tone has energy at both +f and −f;
   zeroing one half of the spectrum is what makes the result
   single-sideband. CW, CWR, USB and DIGITAL keep the upper half, LSB
   the lower.
3. **Bin rotate** - the IF shift, done in the frequency domain instead
   of with an oscillator. What it aims at depends on the signal: CW and
   CWR rotate by 467 bins (21,890.6 Hz, `TX_IF_SHIFT_CW_BINS`), putting
   the 700 Hz tone on the dial; USB, LSB and DIGITAL rotate by 482 bins
   (22,593.8 Hz, `TX_IF_SHIFT_SSB_BINS`), putting the suppressed carrier
   there, so audio at `a` Hz goes out at dial ± `a`. Both sidebands use
   the same SSB rotation and extend from that carrier point in opposite
   directions. Both rotations are derived at startup from the
   `bfo_freq` and `xtal_filter_center` actually loaded from
   `data/hw_settings.ini` - `sound.c` calls
   `tx_pipeline_set_if_placement()` once, after `hw_settings_load()`,
   so a board whose IF differs transmits on frequency rather than off
   by the difference. `tx_pipeline.h`'s `TX_IF_SHIFT_*_BINS` remain as
   the starting values and as what the bench harness uses, since that
   links no hardware code and has no settings file to read. A placement
   that isn't physical (a non-positive difference, or one past Nyquist)
   is rejected, leaving the defaults and logging it: transmitting at a
   wrong IF is worse than transmitting at the default one.
4. **Inverse FFT.** Back to the time domain, still complex.
5. **Peak limiter (ALC).** The signal is still complex at this point,
   so `2*|out_c[i]|` is the envelope the exciter will radiate - the
   right quantity, where the input audio's own peak is not, because the
   bandpass and the sideband zero both change it. The limiter applies
   `min(1, ceiling/envelope)`, a gain rather than a clip, and only ever
   reduces. See "Setting power" for the ceiling, the 2 ms look-ahead
   and the `ALC` reading.
6. **Real part, ×2, and phase correction.** Taking the real part of the
   one-sided spectrum produces the real IF waveform the DAC needs; the
   ×2 restores the half of the tone's amplitude that the sideband zero
   discarded, so the pipeline has unity gain for a steady tone. Rotating
   by an odd number of bins also flips the carrier's sign on every other
   block, so the output is negated on those blocks to keep it
   continuous.

For the example: the 700 Hz tone comes out at 700 + 21,890.6 =
22,590.6 Hz. The other half of the tone, which step 2 zeroed, would
have landed at 21,190.6 Hz; on the bench it's 70 dB down
(`test-tx-pipeline`).

**WM8731 DAC.** The right channel carries the pipeline output at the
calibrated TX amplitude - this is the exciter feed, and the only place
in the chain that sets output power. The left channel carries the
unprocessed TX audio (sidetone or mic) at a fixed, low level
(`SIDETONE_PEAK_AMPLITUDE`) for the local speaker only.

**Mixer 2 — clk1 at `bfo_freq`.** While transmitting, clk1 is
`bfo_freq`, not its RX value. The DAC output drives a single balanced
modulator, so both products appear: 40,035,000 − 22,590.6 =
40,012,409.4 Hz, 9.4 Hz from the crystal filter center, and
40,035,000 + 22,590.6 = 40,057,590.6 Hz, about 45 kHz above it. There's
no phasing network; placing `bfo_freq` ~22.6 kHz above the filter is
what lets the filter keep one product and reject the other.

**Crystal filter.** The same filter RX uses (~35 kHz wide, centered at
`xtal_filter_center`). The wanted product sits at its center; the sum
product is deep in its stopband.

**Mixer 1 — clk2.** clk2 is `freq_hdr + xtal_filter_center` =
47,042,400 Hz, the same as RX without RIT. The output is
47,042,400 − 40,012,409.4 = 7,029,990.6 Hz: the carrier lands 9.4 Hz
below the dial. That residual comes from the bin rotate moving in
whole 46.875 Hz steps. The zeroed image, if any of it survives, would
land 1,400 Hz below that (~7,028,590 Hz); on the air a remote receiver
tuned there heard nothing.

**PA and LPF.** The PA has fixed gain. The LPF relay was already
selected for the band by `radio_tune_to()`; on TX it removes harmonics.

## Setting power

Only the amplitude written to the DAC's right channel sets output
power; everything downstream is fixed analog gain. The chain divides
cleanly at the limiter:

```
audio → mic gain → pipeline → LIMITER → band scale × calibration → DAC → PA
        └──── how hard you push ────┘    └── fixed: amplitude into watts ──┘
```

Everything left of the limiter decides how hard the signal is driven;
everything to its right is calibration that turns an amplitude into
watts. Nothing that an operator or a CAT client can change may sit on
the right — a gain there would walk straight past the ceiling the
limiter exists to hold.

### The ceiling

Two values in `data/hw_settings.ini`, both in watts:

- **`full_scale_power`** — the PEP a full-scale (amplitude 1.0) signal
  produces, once the `[tx_band]` scales are calibrated to give the same
  power on every band. This board's rated output. A measurement.
- **`max_power`** — the ceiling the limiter holds. A choice.

`sound.c` turns them into the limiter's amplitude ceiling,
`sqrt(POWER × max_power / full_scale_power)` — the square root because
power goes as amplitude squared — and hands it to
`tx_pipeline_set_ceiling()` once per block.

**Equal values are the normal setting**, not a degenerate one. The
ceiling is then 1.0, the radio makes rated power in every mode, and the
limiter holds it there. Setting `max_power` below `full_scale_power`
means running below rated power — Mike's 5 W on a board capable of more
— and is the same thing as turning `POWER` down, just persistently.

The limiter's working range comes from the **drive** side, not from
reserving output power. `mic_tx_gain` goes to 64×, and a host's audio
level in DIGITAL is outside this daemon's control entirely, so the
signal arriving at the limiter can exceed full scale by a long way. That
is what the limiter is for, and it's what a real rig's ALC does: the
radio is rated at the same power on CW and on SSB, and the ALC exists to
stop the drive from pushing past it, not to reserve half the PA. CW
never moves the gain at all, because its envelope is constant at exactly
full scale.

### The limiter

`tx_pipeline.c` applies `min(1, ceiling/envelope)`, where the envelope
is `2*|out_c[i]|` — the magnitude of the still-complex signal just
before the real part is taken. That is the right quantity and the input
audio's peak is not: the 300–3000 Hz bandpass and the sideband zero both
change the peak, and an SSB envelope peak can exceed the audio peak that
produced it.

It is a gain, not a clipper. A sine wave times a constant is still a
sine wave, so a tone held at a lower ceiling comes out clean, just
smaller; `tx_pipeline_test.c` Case F checks exactly this, comparing a
limited run against an unlimited one scaled by the expected gain and
reporting what truncating at the ceiling would have cost instead.

The gain only reduces, never exceeds unity, so quiet audio simply
transmits quietly. Raising the average to meet the ceiling would be a
speech compressor — a different thing, belonging upstream of the
pipeline, and deliberately not built here.

Output is delayed by `TX_ALC_LOOKAHEAD_SAMPLES` (192, 2 ms) while the
gain is computed from the undelayed signal, and attack is rate-limited
to cross the whole gain range in exactly that window, so the gain has
arrived before the peak that needs it. Release is slow
(`TX_ALC_RELEASE_S`, 250 ms) so the gain doesn't pump inside a syllable.
The 2 ms shifts the whole transmitted envelope, CW element timing
included, without changing its shape.

A constant-envelope signal at or below the ceiling — CW key-down, a
steady test tone — never moves the gain at all, so `cw.c`'s 5 ms
Blackman-Harris rise and fall come through untouched.

### The controls

`POWER` (rigctld `l/L RFPOWER`, 0.0–1.0 of `max_power`) *moves the
limiter's threshold*. It is not a gain applied after the limiter: with
the threshold moving, the ceiling is enforced at every power setting and
the ALC reading stays meaningful because it is always measured against
the ceiling actually in force.

`MICGAIN` is the drive — how hard the signal is pushed into whatever
ceiling is in force. Turning it up does not raise peak power; it raises
the average relative to the peak, and it moves the ALC meter rather than
the wattmeter.

`ALC` (rigctld `l ALC`) reports gain reduction in dB, peak-held over
`TX_ALC_METER_DECAY_S` so it reads as a meter rather than flickering at
block rate. It is the instrument for setting `MICGAIN`: advance mic gain
until speech peaks read a couple of dB and no further. Without it the
limiter is invisible and "set correctly" can't be told from "10 dB in
and squashed."

DIGITAL has no radio-side drive at all — `mic_tx_gain` touches only the
mic branch, and CW and the test-tone generator are fixed at full scale
because they are the calibration references. For FT8 the drive control
is the host's output level, set by watching this ALC reading, the same
as with a commercial rig.

### The constants

Per sample, the pipeline output is multiplied by
`TX_SAMPLE_HEADROOM × TX_DRIVE × band scale × TX_GAIN_CORRECTION` and
clamped. The constants (`sound.c` unless noted):

- **`TX_DRIVE`** — mirrors real sbitx's 0-100 "drive" setting. Fixed at
  50, and algebraically inert: `TX_SAMPLE_HEADROOM` divides by it and
  the expression multiplies it back, so it cancels exactly and carries
  no power information. It is kept for legibility against sbitx's
  `tx_amp = tx_drive * band.scale`. The operator's power control is
  `POWER` above, not this.
- **`hw_settings_tx_scale(freq)`** (`hw_settings.c`) — the current
  band's `scale` from the `[tx_band]` entries in
  `data/hw_settings.ini`, compensating for the PA's gain varying across
  bands. The values were re-derived with a wattmeter for this pipeline;
  procedure and results are in
  [`dsp_design_notes/tx_power_calibration.md`](dsp_design_notes/tx_power_calibration.md).
- **`TX_SAMPLE_HEADROOM`** — anchors the scale table to a PCM amplitude,
  defined against the 40 m value (`HW_DEFAULT_TX_SCALE`).
- **`TX_GAIN_CORRECTION`** — a flat multiplier, 0.045, set with a
  wattmeter (5.1 W on 40 m). With the scale table this gives ~5-6 W on
  every band. Re-derive it on any board whose filter center or
  `bfo_freq` differ; it depends on where the wanted product sits on
  the filter.
- **`TX_SAMPLE_CLAMP`** — keeps the sample inside the 32-bit range. At
  the current calibration no band reaches it.
- **`TX_MASTER_VOL`** (`radio.c`) — the WM8731 `Master` right-channel
  volume, 95 during TX and 0 otherwise (`sound_set_tx_drive()`). Despite
  the function's name this is mute sequencing around the relay, not a
  power control: it sits downstream of the limiter, so making it
  adjustable would break the ceiling. The left (local speaker) channel
  is never touched here.
- **`mic_tx_gain`** — USB/LSB only, and the operator's drive control.
  It changes the level going into the pipeline, so below the ceiling it
  changes power and above it moves the ALC reading instead. Voice power
  still hasn't been checked on a wattmeter (`ARCHITECTURE.md` §10
  step 10).
- **`SIDETONE_PEAK_AMPLITUDE`** — the local monitor level on the left
  channel. It has no effect on transmitted power.

`tx_pipeline.c` has unity gain for a steady tone, which is why CW's
calibration carries over from the old scheme. Speech and FT8 audio have
different peak-to-average ratios, so their average power at a given
setting still needs checking with a wattmeter — the limiter fixes the
peak, not the average.

## What the FT8 contact proves

On 2026-09-23 a two-way FT8 contact was completed with WSJT-X driving
maxibitx over the USB gadget. It is the strongest single piece of
evidence the transmit chain has, because a contact only happens when
every link works at once and a stranger's decoder is the judge.

It confirms, end to end: WSJT-X's generated audio crossing the gadget
(`uac_reader_thread()`), the 48 kHz to 96 kHz upsample
(`upsample48k.c`), the whole of `tx_pipeline.c` — bandpass, sideband
zero, the 482-bin carrier-anchored rotate, inverse FFT and the
per-block sign flip — the DAC, both mixers, the crystal filter keeping
the wanted product, the PA and the LPF. It also confirms that PTT
through the Kenwood CAT surface keys and unkeys inside WSJT-X's 15
second cycle, that the sideband is right (a mirrored signal decodes as
nothing), and that ~5 W is enough. The reply arriving and decoding
exercises the receive chain in the same cycle.

What it does *not* establish: carrier placement to within a few Hz.
FT8 decoders search a wide window and report the offset they found, so
a contact tolerates an error far larger than the 6.25 Hz of bin
quantization. That figure comes from the test-tone measurements above,
not from this. Nor does it say anything about DIGITAL's power being
calibrated — that is still an open item — or about whether
`UAC_RX_AUDIO_SCALE` puts the decoder in its best input range.

## Known limitations

- **Carrier placement is a few Hz low.** Whole-bin rotation leaves the
  SSB carrier 6.25 Hz below the dial and CW's tone 9.4 Hz below. It
  could be removed by nudging clk2 during TX, at the cost of making
  `radio_tx_apply()` mode-aware again; 6 Hz is well inside an SSB or
  FT8 channel, so it hasn't been. The further ~11 Hz those same
  measurements showed turned out not to be this radio at all: WWV on
  receive puts the si5351 reference at its nominal 25 MHz, and the
  remote receiver used for the transmit tests is itself about 1.5 ppm
  low - see
  [`01_hardware_init_and_control.md`](01_hardware_init_and_control.md).
- **Remote PTT in CW sends no carrier** (see "Keying and PTT") unless
  the test-tone generator is on. Keying CW from a computer needs a real
  key or a future remote keying path.
- **Voice levels uncalibrated.** The limiter holds the peak, but real
  speech hasn't been checked on a wattmeter and `mic_tx_gain`'s best
  default isn't settled (`ARCHITECTURE.md` §9, §10 step 10).
- **`POWER` and `ALC` are bench-tested, not radio-tested.** Case F
  proves the limiter's arithmetic without hardware, but the rigctld
  commands and the panel's slider and meter haven't been exercised
  against a running daemon.
- **The pipeline carries state between transmissions.** Overlap-save's
  filter history already held about a block of the previous burst, and
  the limiter's 192-sample delay line adds 2 ms to that. Both are
  near-silence in practice, because CW ramps down and PTT release
  follows the audio, but nothing explicitly resets the pipeline at the
  start of a transmission. A single `tx_pipeline_reset()` clearing both
  would be the tidy fix if it ever matters.
