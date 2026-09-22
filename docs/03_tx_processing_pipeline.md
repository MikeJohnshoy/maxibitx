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
test-tone generator on 2026-09-22 and corrected
([`dsp_design_notes/tx_test_tones_and_alc.md`](dsp_design_notes/tx_test_tones_and_alc.md));
the corrected placement hasn't been re-checked on air yet. DIGITAL
(WSJT-X over the USB gadget) is code-complete and not yet tested on
air.

The earlier CW-only scheme this pipeline replaced (a second, IF-shifted
oscillator in `cw.c` plus a matching clk2 correction in `radio.c`) is
kept for reference in
[`dsp_design_notes/tx_direct_to_dac_cw_history.md`](dsp_design_notes/tx_direct_to_dac_cw_history.md).
The derivations and bench numbers behind the current design are
[`ARCHITECTURE.md`](ARCHITECTURE.md) §10 steps 4, 5, 8 and 9.

## The chain, end to end

```
  TX audio source, by mode (sound.c's audio_loop())
     CW:      cw.c's 700 Hz sidetone x keying envelope
     USB/LSB: mic (WM8731 right capture channel) x mic_tx_gain
     DIGITAL: WSJT-X audio from the USB gadget, 48 -> 96 kHz (upsample48k.c)
     |
     v
  tx_pipeline.c, one 1024-sample block at a time (96 kHz):
     FFT -> 300-3000 Hz bandpass -> zero one sideband -> rotate bins
     (the IF shift) -> inverse FFT -> real part
     |            result: a real IF near 22.6 kHz
     v
  WM8731 DAC, right channel = exciter feed (at the calibrated TX level),
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
below). Output power is set entirely by the amplitude of that
waveform - see "Adjusting power levels".

## Keying and PTT

Every TX path ends up in `radio_set_tx()` (`radio.c`). It sets `in_tx`
immediately, so every other thread sees the new state at once, and
hands the slow hardware sequence to a dedicated worker thread (see
[`05_process_and_threading_model.md`](05_process_and_threading_model.md)
for why). Who calls it:

- **The key/PTT line** (`CW_KEY`, BCM4), polled once per audio block by
  `cw_poll_key()` (`cw.c`). In CW it's a straight key with semi
  break-in: a hang timer (`CW_HANG_POLLS`, ~300 ms) holds TX through
  the gaps between elements so the relay doesn't chatter. In USB and
  LSB the same line is a mic PTT switch - TX follows the switch, no hang
  timer. In DIGITAL the line is ignored.
- **Remote PTT/MOX:** rigctld `T` (`hamlib.c`), Kenwood `TX`/`RX`/`TQ`
  on the gadget's serial port (`usb_gadget.c`), and HPSDR MOX
  (`hpsdr_p1.c`, which first retunes to the client's TX frequency if
  it differs). While the local key/PTT line holds TX, remote
  requests to change it are ignored - the local key wins.

Which audio actually reaches the exciter is decided in `audio_loop()`:
it transmits when the key/PTT line has TX asserted
(`cw_tx_active()`), when `in_tx` is set in DIGITAL, or when `in_tx` is
set with the test-tone generator on (below). Otherwise remote PTT in
CW, USB or LSB keys the radio but sends silence - there's no remote
audio source in those modes, so a CAT or HPSDR MOX in CW doesn't
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
46.875 Hz per bin) in five steps:

1. **Bandpass**, 300-3000 Hz (`filter_tune_real()`, Kaiser β = 5). It
   keeps both the positive- and negative-frequency images of that band,
   so either sideband is available to the next step.
2. **Sideband zero.** A real audio tone has energy at both +f and −f;
   zeroing one half of the spectrum is what makes the result
   single-sideband. CW, USB and DIGITAL keep the upper half, LSB the
   lower.
3. **Bin rotate** - the IF shift, done in the frequency domain instead
   of with an oscillator. What it aims at depends on the signal: CW
   rotates by 467 bins (21,890.6 Hz, `TX_IF_SHIFT_CW_BINS`), putting its
   700 Hz tone on the dial; USB, LSB and DIGITAL rotate by 482 bins
   (22,593.8 Hz, `TX_IF_SHIFT_SSB_BINS`), putting the suppressed carrier
   there, so audio at `a` Hz goes out at dial ± `a`. Both sidebands use
   the same SSB rotation and extend from that carrier point in opposite
   directions.
4. **Inverse FFT, real part, ×2.** Taking the real part of the
   one-sided spectrum produces the real IF waveform the DAC needs; the
   ×2 restores the half of the tone's amplitude that the sideband zero
   discarded, so the pipeline has unity gain for a steady tone.
5. **Phase correction.** Rotating by an odd number of bins flips the
   carrier's sign on every other block; the output is negated on those
   blocks to keep it continuous.

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

## Adjusting power levels

Only the amplitude written to the DAC's right channel sets output
power; everything downstream is fixed analog gain. Per sample, the
pipeline output is multiplied by
`TX_SAMPLE_HEADROOM × TX_DRIVE × band scale × TX_GAIN_CORRECTION` and
clamped. The constants (`sound.c` unless noted):

- **`TX_DRIVE`** — mirrors real sbitx's 0-100 "drive" setting. Fixed at
  50; there's no command to change it.
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
  volume, 95 during TX and 0 otherwise (`sound_set_tx_drive()`). It
  gates the exciter feed; lowering it would undo the calibration
  above. The left (local speaker) channel is never touched here.
- **`mic_tx_gain`** — USB/LSB only. It changes the level going into
  the pipeline, so it changes power directly. There's no ALC yet, and
  voice power hasn't been calibrated on a wattmeter
  (`ARCHITECTURE.md` §10 step 10).
- **`SIDETONE_PEAK_AMPLITUDE`** — the local monitor level on the left
  channel. It has no effect on transmitted power.

`tx_pipeline.c` has unity gain for a steady tone, which is why CW's
calibration carries over from the old scheme. Speech and FT8 audio have
different peak-to-average ratios, so their power at a given setting
still needs checking with a wattmeter.

## Known limitations

- **Carrier placement is ~6 Hz low, and this board reads ~12 Hz low
  on top of that.** Whole-bin rotation leaves the SSB carrier 6.25 Hz
  below the dial and CW's tone 9.4 Hz below. Separately, a 2026-09-22
  on-air check read both sidebands about 10-14 Hz lower still (about
  1.5-2 ppm at 7.2 MHz), which looks like the si5351's reference rather
  than anything in the DSP - there's no frequency calibration for it
  yet. Both are well inside an SSB or FT8 channel.
- **The IF shift doesn't follow `hw_settings.ini`.** `tx_pipeline.h`
  computes its rotations from compiled-in copies of `bfo_freq` and
  `xtal_filter_center` (`TX_PIPELINE_BENCH_BFO_FREQ_HZ`/
  `_XTAL_CENTER_HZ`), not from the values loaded at startup. They match
  this board's `hw_settings.ini` today; on a board with different
  values, TX frequency would be off by the difference.
- **No band limits.** Nothing refuses PTT outside the `[tx_band]`
  ranges; rigctld's `dump_state` advertises them, but only as
  information.
- **CW-reverse isn't a TX mode yet**, matching RX. For a pure CW tone
  the kept sideband doesn't change the carrier frequency anyway.
- **Remote PTT in CW sends no carrier** (see "Keying and PTT") unless
  the test-tone generator is on. Keying CW from a computer needs a real
  key or a future remote keying path.
- **Voice levels uncalibrated.** No ALC, no wattmeter check of real
  speech; `mic_tx_gain`'s best default isn't settled
  (`ARCHITECTURE.md` §9, §10 step 10).
