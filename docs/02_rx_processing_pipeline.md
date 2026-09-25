# 02 — RX processing pipeline

This documents the receive signal path from antenna to baseband I/Q —
the part that's easy to get wrong, because most of it lives in analog
hardware and a fixed relationship between two si5351 clocks — and then
the two things that happen to that I/Q inside maxibitx: it's handed to
the network I/Q consumers, and it's demodulated into audio.

By this point in the process's life, GPIO, the si5351/I2C bus, and the
WM8731 codec's ALSA capture stream are already up and configured — see
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md).
This document picks up from there: what happens to samples once they're
flowing, and what tuning changes.

maxibitx has no waterfall and does no demodulation for the I/Q it
streams — an SDR app connected over HPSDR does all of that itself. It
does have one onboard demodulator, `rx_audio.c`, which taps the same I/Q
and produces CW, USB or LSB audio (DIGITAL demodulates as USB) for two
outputs: the box's own speaker/headphone jack, and the USB audio gadget
that WSJT-X and similar programs listen to. That demodulator is covered
at the end of this document and in
[`dsp_design_notes/rx_audio_demod_design.md`](dsp_design_notes/rx_audio_demod_design.md).
Where the I/Q and the USB audio go once they leave `sound.c` is
[`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md).

## The chain, end to end

```
  Antenna
     |
     v
  Low Pass Filter (LPF) bank (select one)
     |
     v
  Mixer 1  <---  clk2, si5351 RX LO (varies with tuning and RIT)
     |            mixes received signal to xtal_filter_center - the
     |            crystal filter's own real, measured center
     v
  Crystal filter centered at xtal_filter_center (40,012,400 Hz as
     |            measured on this board, from data/hw_settings.ini)
     v
  Mixer 2  <---  clk1, si5351 (fixed while receiving - xtal_filter_center
     |           + RX_IF_FREQ_HZ; switches to bfo_freq only for the
     |           duration of TX - see 03_tx_processing_pipeline.md),
     |           shifts output of crystal filter to a 24kHz IF
     v
  Low IF, centered at RX_IF_FREQ_HZ (24000 Hz)
     |
     v
  ADC / wm8731 audio codec, left channel (sound.c, 96 kHz sample rate),
     |  capture gain RX_CAPTURE_GAIN_PERCENT, set experimentally
     v
  Software VFO (vfo.c, "lo" in radio.c) <--- FIXED at RX_IF_FREQ_HZ
     |            sound.c: sound_process() calls vfo_read_iq() per sample
     v            converts real value A/D output to analytic I&Q at baseband
  Baseband I/Q (centered at 0 Hz, spectrally inverted - see below)
     |
     v
  Anti-alias FIR (antialias.c, 21 taps, applied separately to I and Q)
     |
     +---> hpsdr_p1.c - baseband I/Q to an external SDR app over HPSDR
     |       Protocol 1 (04_remote_control_and_iq_output.md)
     |
     +---> iq_stream.c - the same I/Q to any subscriber on UDP 4536,
     |       e.g. tools/rigctl_panel.py's spectrum display
     |
     +---> rx_audio.c - CW/USB/LSB demodulation
               |
               +---> local speaker/headphones (WM8731 left output,
               |       after rx_volume)
               |
               +---> decim48k.c (96 -> 48kHz) -> usb_gadget.c's UAC2
                       audio to the USB host (before rx_volume)
```

## Stage by stage

At each stage we can look at an example following a single CW signal
at 7030000 as it flows from the antenna through to I&Q output. We'll
follow a second signal alongside it, 500 Hz higher at 7030500, to show
where each one lands relative to the dial.

**LPF bank.** `radio_hw.c`'s `set_lpf_40mhz(frequency)` selects one of
four low-pass filter relays (`LPF_A`–`LPF_D`) based on the tuned
frequency — under 5.5 MHz, under 10.5 MHz, under 18.5 MHz, or under 30
MHz — and is a no-op if the frequency falls in the same band as the last
call. At 30 MHz and above none is selected. Pure analog front-end
filtering; nothing here talks to either si5351 clock. (Full relay
init/idle-state details are in
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md).)

**Mixer 1 — the RX LO (clk2), which sweeps with tuning.**
`radio_tune_to(f)` in `radio.c` sets it with
`si5351bx_setfreq(2, f + xtal_filter_center)` — mixing the desired RF
frequency `f` up to `xtal_filter_center`, the crystal filter's own
real, measured passband center. That value is a board-specific
calibration loaded from `data/hw_settings.ini` at startup (40,012,400 Hz
on this board, which is also the compiled-in default) - see
[`dsp_design_notes/antialias_filter_design.md`](dsp_design_notes/antialias_filter_design.md)
§2–3 for where that number comes from and why RX gets to aim at it
directly rather than sharing a value with TX. RIT also moves this
clock: while receiving, `radio_set_rit()` and `radio_set_rit_enabled()`
set it to `freq_hdr + RIT offset + xtal_filter_center` (during TX the
offset is stored and applied when RX resumes). Whatever `f` you tune
to, the signal at the dial always lands at that same fixed point;
that's the whole point of a superheterodyne front end, and it's also
*why* nothing downstream of this stage needs to know the current
operating frequency.

For our example cw signal at 7030000,
clk2 = 7,030,000 + 40,012,400 = 47,042,400 Hz

The signal at 7030000 lands at 47,042,400 − 7,030,000 = 40,012,400 Hz,
the filter center. The one at 7030500 lands at 40,011,900 Hz: because
clk2 is above the signal, this mixer inverts the spectrum, and a
station above the dial ends up below the filter center.

**Crystal filter.** A fixed bandpass filter centered at
`xtal_filter_center`. This is the receiver's analog selectivity —
everything outside its passband is rejected before the signal ever
reaches Mixer 2. The I/Q sent to external apps gets no further
filtering beyond the anti-alias filter below; the app does its own
channel filtering, and `rx_audio.c` does its own for the audio it
produces. (Measured filter response and the anti-alias filter designed
against it live in
[`dsp_design_notes/antialias_filter_design.md`](dsp_design_notes/antialias_filter_design.md).)

**Mixer 2 — clk1, fixed while receiving.** This mixer brings the
crystal-filter output (centered at `xtal_filter_center`) down to a low
IF of `RX_IF_FREQ_HZ` (24000 Hz) that the audio codec can actually
sample. Its LO is si5351 `clk1`, set to
`xtal_filter_center + RX_IF_FREQ_HZ` (40,036,400 Hz by default) in
`maxibitx.c` at startup, and restored to that same value every time RX
resumes after a TX burst (`radio_tx_apply()`, `radio.c`). It does not
sweep with tuning for the same reason Mixer 1's output doesn't need to:
whatever `f` you're tuned to, Mixer 1 already brought it to the same
fixed `xtal_filter_center` point, so Mixer 2 only ever has to undo that
one fixed offset. It is, however, *not* fixed for the entire life of
the process: while transmitting, `radio_tx_apply()` retunes it to
`bfo_freq` instead - a deliberately different, off-center value used
only for TX image suppression - and restores this RX value the moment
TX ends. See
[`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md) for why
TX needs its own value here rather than reusing this one.

clk1 is also above the signal, so this mixer inverts the spectrum a
second time: 40,036,400 − 40,012,400 = 24,000 Hz for the signal at
7030000, and 40,036,400 − 40,011,900 = 24,500 Hz for the one at
7030500. At the IF, a station above the dial is above 24 kHz again.

**ADC / audio codec.** `sound.c`'s audio thread (`audio_loop()`) reads
~10.7 ms blocks from the already-open ALSA capture device (opened and
configured per
[`01_hardware_init_and_control.md`](01_hardware_init_and_control.md)) at
96 kHz and hands the left channel - the RX IF; the right is the mic -
to `sound_process()`. What arrives here is the low IF signal —
real-valued, centered around `RX_IF_FREQ_HZ`, not yet I/Q. The WM8731's
capture gain is `RX_CAPTURE_GAIN_PERCENT`, a bench value rather than a
final calibration; `rx_clip_check()` logs `*** CLIPPING ***` if a
sample reaches full scale. During TX, `radio_tx_apply()` turns RX
capture off (`sound_set_rx_capture(0)`) before any RF exists and back
on once the relay has settled, so the receive chain never sees TX
energy or relay transients.

**Software VFO — fixed at RX_IF_FREQ_HZ, never swept.** `vfo.c`
implements a digital NCO (`struct vfo`, the global `lo` in `radio.c`)
that generates quadrature (cos/sin) mixing signals. `sound_process()`
calls `vfo_read_iq()` once per sample and multiplies the incoming real
IF sample by both the cosine and sine outputs, producing the I and Q
channels — a digital quadrature downconversion, taking the fixed 24 kHz
IF down to baseband (0 Hz). Like clk1, this oscillator's frequency is
fixed at `RX_IF_FREQ_HZ` and does not change when you retune;
`radio_tune_to()` restarts it at the same frequency, keeping its phase.
See `RX_IF_FREQ_HZ` in `radio.h` for the single place this constant is
defined.

**The I/Q is spectrally inverted.** The mix above maps an IF frequency
`24000 + d` to baseband `−d`, so a station `d` Hz above the dial arrives
at `−d` Hz. In the example, the signal at 7030000 lands at 0 Hz and the
one at 7030500 lands at −500 Hz. `sound.c` deliberately leaves the I/Q
this way: SparkSDR over `hpsdr_p1.c` displays and decodes it correctly
as-is, and changing it at the source would flip every I/Q consumer at
once. Consumers that need the other orientation correct for it
themselves: `rx_audio.c` (`RX_IQ_SPECTRUM_INVERTED`, below) and
`tools/rigctl_panel.py`'s spectrum display (which conjugates the I/Q).
This was confirmed on air, and was the root cause of weak FT8 decoding
over the USB audio gadget - see
[`dsp_design_notes/rx_uac_out_digital_mode_bandwidth.md`](dsp_design_notes/rx_uac_out_digital_mode_bandwidth.md)
§10.

**Anti-alias FIR filter.** Right after mixing to I/Q, `sound_process()`
runs each rail through `antialias_apply()` (`antialias.c`) — a 21-tap,
symmetric (linear-phase) FIR lowpass, independently on I and
independently on Q, sharing one coefficient table but each with its own
history state (`struct antialias_state`). The mix also produces a
self-image at `48000 + d` Hz, which the 96 kHz sample rate folds back to
just inside the ±48 kHz Nyquist edge; this filter removes it — see
[`dsp_design_notes/antialias_filter_design.md`](dsp_design_notes/antialias_filter_design.md)
for the measured crystal-filter data it was designed against. The
chosen design (32 kHz passband edge, 47.5 kHz stopband edge) preserves
essentially all of the usable spectrum the crystal filter itself already
delivers, adding roughly −81 dB of stopband rejection on top of the
crystal filter's own real (but more gradual) rolloff — comfortably over
−100 dB combined right where aliasing would actually occur. It's cheap:
21 multiply-adds per output sample, and a double-length history buffer
avoids any wraparound branch in the inner loop.

**Baseband I/Q → three consumers.** `sound_process()` concludes by
handing its (now anti-aliased) I/Q arrays to whatever's listening; any
subset may be active. Two are network I/Q streams:
`hpsdr_send_iq()` (HPSDR Protocol 1, for SDR apps) and
`iq_stream_send()` (a lightweight multi-subscriber UDP stream, used by
the control panel's spectrum display) — see
[`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md).
The third is `rx_audio.c`'s `rx_audio_process()`, called on the very
same `i_samples[]`/`q_samples[]` arrays. It's a straight function call,
not a separate thread or process — same real-time audio callback, same
block, no extra latency or a second capture path fighting for the same
ALSA device.

## The onboard demodulator (`rx_audio.c`)

`rx_audio.c` turns the I/Q into audio in four stages:

1. A wide complex bandpass (327-tap FIR) keeps 0 to about +3000 Hz of
   baseband and rejects negative frequencies.
2. Demodulation. For USB and LSB the input is conjugated or not before
   stage 1, which is what selects the sideband, and the output is the
   real part with no BFO - so audio frequency equals the distance from
   the dial. With the I/Q inverted, USB (above the dial) sits at
   negative baseband, so USB and DIGITAL conjugate and LSB doesn't. CW
   keeps the upper side too, so it conjugates like USB, then mixes up
   to `CW_PITCH_HZ` (700 Hz): a station on the dial is heard at 700 Hz,
   one 200 Hz above it at 900 Hz, and tuning up lowers the pitch, as on
   most rigs. CWR (CW-reverse) is that same BFO with LSB's
   conjugation - the lower side instead, so it is a station 200 Hz
   *below* the dial that's heard at 900 Hz. Transmit is unaffected by
   the choice, since a key-down carrier lands on the dial either way,
   which is why CWR keys and transmits exactly as CW does
   ([`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md)).
   `rx_audio_test.c` case E measures both, each rejecting the other's
   side by about 40 dB.
3. An optional narrow filter (an elliptic IIR by default, or
   `rx_filter.c`'s FFT filter), switched by rigctld `U NARROW` and
   `U FFTFILT` or the control panel. Its pitch and width are selectable at
   runtime — `L CWPITCH` picks 500 to 1000 Hz in 100 Hz steps and
   `L CWWIDTH` picks
   150, 300, 450 or 600 Hz, defaulting to 700/300. Changing the pitch moves
   the stage-2 BFO with it, so the tone you hear moves rather than the
   filter just detuning off it — and it moves the TX sidetone and CW IF
   shift too, so the pitch you hear is the pitch you send against while the
   transmitted frequency stays where it is
   ([`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md)).

   The elliptic can't design coefficients at runtime, so those twelve
   combinations are pre-designed offline into `src/narrow_filter_bank.h` by
   `tools/gen_narrow_filters.py` and switched by loading a set; the FFT
   filter is retuned to the same passband so the two stay comparable. The
   FFT one runs a minimum-phase realization by default, which keeps a keyed
   CW element's attack close to the elliptic's while measuring 30-55 dB
   deeper in the stopband; `U MINPHASE 0` returns the linear-phase
   realization for comparison
   ([`dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md`](dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md)).
4. AGC, which measures the raw input, not any filtered stage.

The mode is set by `radio_set_mode()` in `radio.c`, which calls
`rx_audio_set_demod()`; every mode change (rigctld `M`, Kenwood `MD`,
the control panel) goes through there.

After AGC the audio goes two ways. The local speaker output applies
`rx_volume`, a log taper (0-100% maps onto −50..0 dB below
`RX_VOLUME_MAX`; 0% mutes; the default is 67%). The USB audio output
(`uac_out`) is taken before the volume control, so the level WSJT-X
sees doesn't depend on the speaker volume. `sound.c` decimates it to
48 kHz (`decim48k.c`) and queues it for `usb_gadget.c` with
`uac_push_audio_rx()`. That path has 15 dB of headroom
(`UAC_RX_AUDIO_HEADROOM`) so strong signals don't clip.

Full design, bench data and history:
[`dsp_design_notes/rx_audio_demod_design.md`](dsp_design_notes/rx_audio_demod_design.md)
(the stages and AGC) and
[`dsp_design_notes/rx_uac_out_digital_mode_bandwidth.md`](dsp_design_notes/rx_uac_out_digital_mode_bandwidth.md)
(USB/LSB demodulation, the I/Q inversion, and the USB audio level).
USB/LSB voice reception has not been tested on air yet; DIGITAL (FT8)
has, and decodes on par with SparkSDR on the same I/Q.

## Retuning

`radio_tune_to(f)` in `radio.c` is how the dial frequency changes. It
sets `freq_hdr`, clears RIT, moves `clk2` (Mixer 1's LO), restarts the
software VFO at its same fixed frequency, and selects the LPF. It does
**not** touch `clk1` and does **not** change the software VFO's
frequency — the software VFO stays at `RX_IF_FREQ_HZ` for the life of
the process. The only other thing that moves the receive frequency is
RIT: `radio_set_rit()` and `radio_set_rit_enabled()` move `clk2` alone,
by the RIT offset, without changing `freq_hdr`. `radio_tune_to()` has
no notion of TX; the one place `clk1` *does* move is
`radio_tx_apply()` - see
[`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md) - which
also sets `clk2` without the RIT offset during TX and restores it with
RIT afterwards. As long as no TX burst is in progress, `clk1` sits at
its RX value (`xtal_filter_center + RX_IF_FREQ_HZ`) and `radio_tune_to()`
alone is enough to retune the receiver. Who's allowed to call
`radio_tune_to()`, and how, is covered in
[`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md).
