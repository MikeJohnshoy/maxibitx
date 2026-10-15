# zBitx RX pipeline (drexjj/zbitx, `hpsdr-iq`)

Status: analysis of
[drexjj/zbitx `hpsdr-iq`](https://github.com/drexjj/zbitx/tree/hpsdr-iq) at
commit `360fbfc`. Nothing was changed in either repo. Line numbers are at
that commit. Filter responses and the effects marked *simulated* come
from a numpy model of the code, not from the radio. Effects marked
*prediction* have not been measured.

This follows the receive signal from the antenna to the speaker and to
each external interface in the zBitx's own software, in the same way as
[`../02_rx_processing_pipeline.md`](../02_rx_processing_pipeline.md) does
for maxibitx. It then answers three questions: is the IF mixed to
baseband to make I and Q, is the filtering right at each stage, and what
in the chain costs performance.

## Short answers

- **The IF is 28,125 Hz on this branch, not 24 kHz.** It is
  `center_bin` × 96000 / 2048, and the default `center_bin` is 600. The
  old 24 kHz was `center_bin` 512.
- **The demodulator never mixes the IF to baseband in the time domain.**
  `rx_linear()` takes an FFT of the real IF, then rotates the bins by 600
  so that the IF lands on bin 0. A bin rotation is a complex mix to
  baseband, done in the frequency domain, and with an even `center_bin`
  it is exact. After the sideband is selected and the channel filter
  applied, the inverse FFT gives complex baseband samples, which are the
  I and Q of the channel. The audio is their imaginary part.
- **Wideband time-domain I/Q exists only in `iq_baseband.c`**, for HPSDR
  and TCI. It uses a numerically controlled oscillator (NCO) at
  −(IF + offset) and a 63-tap lowpass, and runs only while a client is
  taking I/Q.
- **Filtering is good where it matters.** The 1025-tap FFT channel filter
  has about ±0.01 dB ripple, sidelobes 61 dB down, and is 72 to 80 dB down
  beyond a few kHz. That one filter is why the later shortcuts work: the
  every-8th-sample FT8 decimation, the every-2nd loopback and the
  every-6th remote audio have no anti-alias filters of their own.
- **Three filtering problems are real:**
  - ANR's bin smoothing amplitude-modulates the audio at 93.75 Hz.
    Simulated, the sidebands are 24 dB down.
  - The RX EQ is broken in four ways (off by default).
  - An odd `center_bin` would wreck the audio, and nothing stops one.
- **The biggest performance costs:**
  - The default build is unoptimized (`-g`, so `-O0`).
  - The FFTs are double precision and complex, although the input is real.
  - The noise reduction runs over all 2048 bins when about 62 (SSB) survive
    the filter.
  - Every filter change re-plans FFTW and reads and writes the wisdom file.
    In SSB modes this races the audio thread; in CW it drops blocks to
    silence.

## The chain, end to end

```
  Antenna
     |      (no LPF in the receive path on the zBitx: set_lpf_40mhz()
     |       runs only when sbitx_version < 4; the LPFs are TX-only)
     v
  Mixer 1  <---  clk2 = f + bfo_freq - 28,125 (- pitch in CW, + in CWR)
     |            radio_tune_to(), sbitx.c:215. Inverts the spectrum.
     v
  Crystal filter, ~35 kHz roofing filter, -6 dB at 39,994.0-40,030.8 kHz
     |            The dial sits at 40,019,875 Hz, 7.5 kHz above its center
     v
  Mixer 2  <---  clk1 = bfo_freq = 40,048,000 Hz, fixed in RX and TX.
     |            Inverts again: the IF is upright
     v
  IF at 28,125 Hz (center_bin 600 x 46.875 Hz)
     |
     v
  WM8731 left channel, 96 kHz S32_LE, 1024-frame periods (10.67 ms)
     |   sbitx_sound.c: sample / 2; capture backlog > 3 periods is
     |   skipped down to 2 (a gap)
     v
  sound_process(), sbitx.c:2146
     |
     +---> iq_baseband.c: NCO at -(IF + offset), 63-tap lowpass
     |       |  (only while a client takes I/Q; runs even when the DSP
     |       |   lock is busy)
     |       +---> hpsdr_p1.c, UDP 1024, 96 kHz or every 2nd sample to 48 kHz
     |       +---> TCI iq_start stream (proper decimator)
     |
     +---> radio_dsp_trylock() fails? -> the whole block is zeroed
     |
     v
  rx_linear(), sbitx.c:1258  (overlap-save: frame of 2048, hop of 1024)
     |  real frame -> 2048-point complex double FFT
     |    +--> every 6th block: window + a second FFT -> spectrum/waterfall
     |  rotate bins by 600: the IF moves to DC  (the mix to baseband)
     |  CW zero-beat indicator
     |  notch, noise estimate, DSP, ANR  (not in DIGITAL/FT8/2TONE)
     |  zero the unwanted sideband (half the bins)
     |  x channel filter (filter_tune(): Kaiser beta 5, 1025 taps)
     |  inverse FFT -> complex baseband, keep the last 1024 samples
     |  agc2(), sbitx.c:930
     |  audio = imaginary part   (AM: magnitude)
     v
  output_speaker, 96 kHz
     |
     +---> modem_rx(): FT8 (every 8th sample -> 12 kHz), CW decoder,
     |       fldigi
     +---> RX EQ + tanh limiter, if enabled (not in digital modes)
     +---> qremote: every 6th sample -> 16 kHz, web/remote audio
     +---> tci_audio_rx(): decim48k -> TCI RX audio (48 kHz)
     |
     v
  sbitx_sound.c
     +---> WM8731 left output (speaker/headphones; volume = codec Master)
     +---> ALSA loopback hw:1,0: every 2nd sample -> 48 kHz, both channels
```

## Stage by stage

We follow a CW signal at 7,030,000 Hz with the radio in CW at the default
pitch of 600 Hz and a 500 Hz bandwidth. A second signal 500 Hz higher, at
7,030,500 Hz, shows where an off-channel station goes. Where USB differs,
the numbers are given too.

**Front end.** On the zBitx the low-pass filter relays are used only
while transmitting. `set_rx1()` (sbitx.c:727) calls `set_lpf_40mhz()`
only when `sbitx_version < 4`, so on the zBitx the antenna goes to Mixer 1
with no band filter. This is hardware and software can't change it. It
matters only for very strong out-of-band signals.

**Mixer 1, clk2.** `radio_tune_to()` sets
`clk2 = f + bfo_freq + bfo_freq_runtime_offset − shift_hz`, minus the
pitch in CW and plus it in CWR. `shift_hz` is `tx_shift` × 96000 / 2048,
rounded (28,125 Hz), and `TUNING_SHIFT` is 0.

- CW: clk2 = 7,030,000 + 40,048,000 − 28,125 − 600 = 47,049,275 Hz.
- USB: clk2 = 47,049,875 Hz.

clk2 is above the signal, so this mixer inverts the spectrum.

- The 7,030,000 signal lands at 47,049,275 − 7,030,000 = 40,019,275 Hz.
- The 7,030,500 signal lands at 40,018,775 Hz.

The zBitx folds the CW pitch into the LO, so a zero-beat CW signal lands
600 Hz off the IF center. maxibitx keeps clk2 fixed relative to the
dial and puts the pitch in software.

**Crystal filter.** This is a roofing filter about 35 kHz wide. Evan,
AC9TU, measured it on an analyzer: −6 dB at 39,994.0 and 40,030.8 kHz,
centered at 40,012.4 kHz. The maxibitx sweep agreed (see
[`zbitx_port_study.md`](zbitx_port_study.md) §11). The dial (USB) is at
40,019,875 Hz, 7.5 kHz above the center, so the filter passes more of
the band above the dial than below it:

- RF below the dial maps to crystal frequencies above 40,019,875, and
  the filter's upper edge is 10.9 kHz away.
- RF above the dial maps below, and the lower edge is 25.9 kHz away.

A 3 kHz SSB channel on either side is well inside the passband, so this
costs the audio nothing. It does shape the I/Q span (below).

**Mixer 2, clk1.** `setup_oscillators()` sets clk1 to `bfo_freq`,
40,048,000 Hz, and it stays there in receive and transmit. clk1 is above
the crystal frequency, so the spectrum inverts back and the IF is upright.

- The 7,030,000 signal: 40,048,000 − 40,019,275 = 28,725 Hz.
- The 7,030,500 signal: 29,225 Hz.
- USB: the dial is at 28,125 Hz and a station 500 Hz above it at 28,625 Hz.

The other mixer product, around 80 MHz, and the image at a crystal
frequency of 40,076 kHz are both far outside the crystal filter.

The crystal passband maps to an IF of 17.2 to 54.0 kHz. The part above
48 kHz (crystal frequencies below 40,000 kHz, RF more than 19.9 kHz above
the dial) is past the ADC's Nyquist frequency. Only the codec's own
decimation filter can stop it. If it gets through, it folds to an IF of
42 to 48 kHz, which shows as +14 to +20 kHz in the I/Q. That is far from
the audio channel, which the FFT filter protects by 80 dB. This is a
*prediction* about the I/Q edge and has not been measured.

**ADC.** The WM8731's left channel is captured at 96 kHz, S32_LE, in
periods of 1024 frames (`sbitx_sound.c`). Each sample is divided by 2,
and `rx_linear()` scales it by 1/10⁸. `setup_audio_codec()` selects the
Line input with the mic and mic boost off. The `rx_gain` setting scales
only the S-meter.

*Prediction:* the maxibitx sweep on this board found a roll-off fixed in
the IF domain, after the crystal filter. It starts around +5 to +9 kHz
from maxibitx's 24 kHz IF (so 29 to 33 kHz) and falls about 0.8 dB/kHz,
most likely the codec's digital filter. The zBitx puts USB audio at an
IF of 28.1 to 31.1 kHz, the start of that slope, so the top of the USB
passband could lose 1 to 2 dB. LSB (IF 25.1 to 28.1 kHz) and CW at 600 Hz
(28.7 kHz) are below it. A swept tone or flat noise in USB, compared
with LSB, would settle it.

**Capture backlog.** Before each read, if more than 3 periods are
waiting, the sound thread skips forward to 2 (sbitx_sound.c:697). That
keeps the latency bounded, but each skip is a gap of 10 to 20 ms or more
in the audio and the I/Q. A skip is a symptom: something upstream ran
late.

**`sound_process()`.** `iq_baseband_process()` runs first, outside the
DSP lock (see "External I/Q" below). Then `radio_dsp_trylock()`: if
another thread holds the lock (a mode change, any command in CW, a
filter change in CW), the block's speaker and TX output are zeroed and
`rx_linear()` doesn't run. That block is silence.

**Overlap-save gather (Steps 1 and 2).** The FFT frame is 2048 samples:
the previous block of 1024 and the new 1024, real, with the imaginary
part zero. The channel filter has 1025 taps, and 1024 + 1025 − 1 = 2048,
so the convolution fits the frame exactly and the last 1024 output
samples are clean. This is correct overlap-save.

**Forward FFT (Step 3).** A 2048-point complex-to-complex double FFTW
(`plan_fwd`, sbitx.c:325, planned with FFTW_MEASURE). Bins are
46.875 Hz apart.

- The input is real, so half of this FFT is spent producing the mirror
  image of the other half.
- The 28,725 Hz signal is at bin 612.8, and its mirror at −28,725 Hz is
  at bin 1435.2.

**Spectrum (Step 3B).** Every 6th block (15.6 Hz), the time frame is
windowed and a second 2048-point FFT computed. `spectrum_update()` shows
534 bins starting at `MAX_BINS − tx_shift − 267` (bin 1181): about
25 kHz around the IF's negative-frequency image at bin 1448.

**The mix to baseband (Step 4).** `r->fft_freq[i] = fft_out[i + 600]`
(mod 2048). Every frequency moves down by 28,125 Hz:

- The 7,030,000 signal: 28,725 → +600 Hz.
- The 7,030,500 signal: 29,225 → +1,100 Hz.
- USB: the dial goes to 0 Hz and the station 500 Hz up to +500 Hz.
- The mirror at −28,725 → −56,850 Hz, which wraps to +39,150 Hz. That is
  39 kHz from the channel, and the sideband zeroing and the filter remove it.

Rotating the DFT of a frame by k bins is the same as multiplying that
frame by e^(−j2πkn/2048). Each frame starts 1024 samples after the last,
so the mixer's phase from one frame to the next advances by πk. For an
even k (600) that is a whole number of turns, and the result is an exact,
phase-continuous complex mix.

For an odd k, every other block is multiplied by −1. That is a
46.875 Hz square-wave modulation. *Simulated* with k = 601, a 1 kHz tone
comes out as two equal tones at about 953 and 1047 Hz. `center_bin` comes
straight from `hw_settings.ini` (sbitx.c:2290) with no check that it is
even. It should be rejected or rounded when odd.

**Zero-beat (Step 4a, CW only).** `calculate_zero_beat()` looks at a few
bins around the pitch, rate-limited by its own clock. It is cheap.

**Notch, noise estimate, DSP, ANR.** These run in every mode except
DIGITAL, FT8 and 2TONE.

- **Notch.** It multiplies a range of bins by 0.001: a frequency-domain
  brick wall.
- **Noise estimate.** It is updated every `noise_update_interval` (50)
  blocks, on all 2048 bins, whether DSP is on or not.
- **DSP.** Spectral subtraction with a sigmoid of per-bin SNR, a 10%
  noise floor and per-bin smoothing across blocks.
- **ANR.** A per-bin Wiener-like gain (at least 0.2), then a smoothing of
  each bin with its neighbours, `0.8·X[i] + 0.1·X[i−1] + 0.1·X[i+1]`.

Two problems.

1. Per-bin gains that change from block to block break the overlap-save
   rule above. The gain curve is no longer a 1025-tap filter, so part of
   its effect wraps around the frame. The maxibitx NR study measured this
   as aliasing 13 dB down for a similar design
   ([`rx_cw_noise_reduction_study.md`](rx_cw_noise_reduction_study.md)).
   That is typical of spectral subtraction without windowed overlap-add,
   and it is part of why such NR sounds "musical".
2. ANR's bin smoothing is worse, because it is not adaptive. Convolving
   the bins with [0.1, 0.8, 0.1] is the same as multiplying the time
   frame by 0.8 + 0.2·cos(2πn/2048). The output samples kept from each
   frame come mostly from the middle of the frame, where that weight
   dips from 0.8 to 0.6 and back. The result is a 93.75 Hz amplitude
   modulation on everything heard with ANR on. *Simulated* on a steady
   1 kHz tone with the Wiener gains at 1, the strongest sideband is
   23.6 dB down, at 1,094 Hz. The fix is to drop the bin smoothing or
   apply it to the gains, not the bins.

All of this runs over all 2048 bins, with a `cabs()` (and in DSP an
`exp()`) per bin, when only the channel's bins survive the filter:
about 5 for a 200 Hz CW filter, 62 for SSB, 85 for FT8. Running it only
over the passband would cut that work by 20 times or more.

**Sideband select.** USB, CW and DIGITAL zero bins 1024 to 2047, the
negative frequencies. LSB and CWR zero 0 to 1023. AM keeps both. The
channel filter is already 61 to 80 dB down there (below), so this step is
nearly redundant. It is a brick wall applied after a well-designed
filter, so the little it adds is harmless.

**Channel filter.** `set_rx_filter()` (sbitx.c:2182) calls
`filter_tune()` (fft_filter.c:194):

- USB/CW/DIGITAL: `low..high`.
- LSB/CWR: `−high..−low`, as negative frequencies.
- AM: IF ± `high`, because AM isn't rotated.

`filter_tune()` sets a brick wall on 2048 bins, then `window_filter()`
takes it to the time domain, keeps 1025 taps, applies a Kaiser window
(β = 5) and returns to the frequency domain. The mode sets `low` and
`high`:

| Mode | Passband |
|---|---|
| CW | pitch ± bandwidth/2 |
| SSB | 100 to 100 + bandwidth |
| DIGITAL | 50 to bandwidth |
| FT8 | 50 to 4000 |
| all | `low` at least 50, `high` at most 5000 |

*Simulated* responses of the code's own design:

| Setting | −6 dB | −20 dB | −40 dB | Stopband |
|---|---|---|---|---|
| SSB 100–3000 | 117 to 3,023 Hz | 34 to 3,107 Hz | below 0 to 3,157 Hz | −61 dB at 3.5 kHz, −72 dB beyond 5 kHz, −80 dB beyond 10 kHz; opposite sideband −61 dB |
| CW 500 Hz (350–850) | 513 Hz wide | 680 Hz wide | 779 Hz wide | 1,100 Hz (our second signal) −58 dB; −600 Hz −70 dB |
| CW 200 Hz (500–700) | 211 Hz wide | 360 Hz wide | 457 Hz wide | −90 dB beyond 2.2 kHz |

Passband ripple is ±0.01 dB. The 7,030,500 signal, at +1,100 Hz, comes
out 58 dB down. The skirts are about 230 Hz from −1 to −60 dB, set by the
1025 taps. That is excellent for SSB and digital, and adequate but not
crystal-sharp for narrow CW. The edges snap to the 46.875 Hz bin grid
before windowing, so the real width differs from the setting by up to a
bin depending on pitch and bandwidth. The filter adds 512 samples
(5.3 ms) of delay, on top of the 10.7 ms block.

**Inverse FFT and output.** `plan_rev` takes the filtered bins to
`fft_time`, and the last 1024 samples are kept. These are complex
baseband samples: the channel's I and Q, holding only +350 to +850 Hz
(CW) or +100 to +3000 Hz (USB). Because the spectrum is one-sided, the
imaginary part is the Hilbert transform of the real part, and either
one is the demodulated audio. The code outputs `cimag()`. That is a
complete SSB/CW demodulator, equivalent to the phasing method, done
without forming wideband I/Q. For AM it outputs `cabs()`, an envelope
detector on the IF-centered filter.

The 7,030,000 CW signal comes out as a 600 Hz tone. In USB, the carrier
500 Hz above the dial comes out at 500 Hz.

**AGC, `agc2()`.**

- It is a channel AGC: it measures after the filter, so strong signals
  outside the passband can't pump it. That is the right place for it.
- Each block it finds the peak of the imaginary part, and if that needs
  less gain it cuts the gain for the whole block at once. That works
  like a one-block look-ahead, so there's no overshoot, but the gain
  steps at the block boundary.
- After the hang (`agc_speed` blocks: FAST 10, MED 33, SLOW 100, that is
  0.11, 0.35 and 1.07 s), it computes `ramp = (target − gain) / 1024`
  and adds it once per block, not once per sample. The gain therefore
  closes 1/1024 of the gap each block, a time constant of about 11 s
  whatever the speed setting. Recovering 20 dB to within 6 dB takes
  about 6 s. The divide by 1024 suggests the intent was a per-sample
  ramp that recovers within one block.
- Only positive peaks are measured.

The gain is set so that the peak output is about 10⁸ of a 2³¹ full
scale, about 27 dB of headroom.

**Muting, modems, EQ, remote.**

- `mute_count` zeroes blocks after a TX/RX change.
- `modem_rx()` (before the EQ):
  - FT8 keeps every 8th sample, so 12 kHz (modem_ft8.c:855). There is no
    anti-alias filter, but the channel filter ends at 4 kHz and is 72 dB
    down by 6 kHz, so the aliases are below FT8's noise floor.
  - The CW decoder.
  - fldigi.
- The RX EQ (`para_eq.c`, off by default, not in digital modes) has four
  bugs:
  - It is passed 48,000 Hz while the samples are at 96 kHz, so every band
    sits at twice its set frequency.
  - It builds new biquads with zero state on every 1024-sample block,
    which clicks at 93.75 Hz.
  - It runs five peaking filters in parallel and averages them. Each
    peaking filter passes everything, so a +10 dB band ends up as about
    +2 to +3 dB, with phase ripple where the bands overlap.
  - It sums into `int32_t` before dividing by 5, which can overflow at
    normal levels (AGC peak 10⁸, limiter 4×10⁸, gains above 1).
- The tanh limiter after it generates harmonics.
- `qremote` keeps every 6th sample (16 kHz) for the web/remote audio.
  Above 8 kHz the channel filter is 72 to 80 dB down, so that is
  acceptable, except for harmonics from the limiter.

**Speaker, loopback, TCI audio.**

- **Speaker.** `output_speaker` goes to the WM8731's left output at
  96 kHz; the right is TX output, zero in RX. Volume is the codec's
  `Master` control, so it doesn't change the loopback or TCI levels.
- **Loopback.** The ALSA loopback (`hw:1,0`) gets every 2nd sample on
  both channels, through `line_out[j++] = line_out[j++] = output_i[i];`
  (sbitx_sound.c:972). Two problems:
  - No filter. That is acceptable for the same reason as FT8.
  - The line is undefined behaviour: two unsequenced `j++` in one
    expression. GCC and clang on x86 happen to do the intended thing at
    `-O0` and `-O2` (a quick test), but no compiler has to. It should be
    `line_out[j] = line_out[j + 1] = output_i[i]; j += 2;`.
- **TCI audio.** `tci_audio_rx()` uses `decim48k`, a proper 96 to 48 kHz
  decimating lowpass.

## External I/Q (`iq_baseband.c`, HPSDR, TCI)

`iq_baseband_process()` runs at the top of `sound_process()`, before the
DSP lock, so the I/Q keeps flowing while the demodulator is busy. While
transmitting it sends silence so the stream keeps its timing.

1. **NCO.** A double-precision NCO at −(28,125 + offset), where the
   offset is the CW/CWR pitch folded into clk2, so the dial is always at
   0 Hz. In CW the 7,030,000 signal at 28,725 Hz goes to 0 Hz, and the
   7,030,500 signal to +500 Hz. The I/Q is not spectrally inverted.
2. **Lowpass.** A 63-tap Blackman windowed sinc, in float, with symmetric
   taps folded, on each rail. The cutoff is halfway between the band and
   the mixer image at −56,250 Hz (which wraps to +39,750 Hz): 19,875 Hz.
   *Simulated*:

   | Frequency | Response |
   |---|---|
   | to 15 kHz | flat |
   | 17 kHz | −0.2 dB |
   | 19.875 kHz | −6 dB |
   | 22 kHz | −23.5 dB |
   | 24 kHz | −69 dB |
   | 28 kHz | −82 dB |
   | 39.75 kHz (image) | −103 dB |

3. **HPSDR 48 kHz.** `hpsdr_p1.c` keeps every other sample. Content above
   24 kHz is at least 69 dB down and folds only into the outer 20 to
   24 kHz of the span. That is appropriate.
4. **TCI I/Q.** Uses a proper decimator.

The usable span follows the crystal filter. With the dial 7.5 kHz above
the filter's center, it runs from about −10.9 kHz (the filter's −6 dB
edge) to +19.9 kHz (the lowpass), not ±15 kHz. An app will show the low
side falling off first.

Cost: a complex multiply and NCO update per sample in double, and two
32-pair folded float filters. That is about 6 M multiply-adds a second,
paid only while a client is connected.

## Retuning

- **Frequency.** `set_rx1()` (sbitx.c:727) calls `radio_tune_to()` only
  when the frequency changes. On the zBitx that moves clk2 and sets
  `iq_baseband`'s offset, nothing else: no LPF, no change to clk1, the bin
  rotation or the filter.
- **Mode and CW pitch.** A mode change retunes (the pitch term changes)
  and re-runs `set_rx_filter()`. A CW pitch change retunes too
  (sbitx.c:2962), so the far station's dial position doesn't move.
- **BFO.** `set_bfo_offset()` moves clk1 and retunes, under both locks.
- **What is not reset.** No retune resets the overlap-save history, the
  AGC gain or the noise estimate. After a big jump the AGC starts from
  the old gain and, if it has to rise, takes seconds (see AGC above).
- **Filter changes.** Every bandwidth, low or high change runs a full
  `filter_tune()`:
  - `window_filter()` plans two 2048-point FFTW plans with FFTW_MEASURE
    and no time limit, imports the wisdom file from disk, and exports it
    back to disk, all on every call (fft_filter.c:96–108). With wisdom
    already in place, planning is quick, but it is still file I/O on the
    SD card and a plan build per change.
  - `sdr_request()` (sbitx.c:3100) takes the DSP lock only for mode
    changes and for any command while in CW/CWR. So:
    - **In CW**, the audio thread's `trylock` fails for as long as this
      takes, and those blocks go out as silence.
    - **In SSB and digital**, it isn't locked at all. `filter_tune()`
      writes the brick wall into `fir_coeff` first and transforms it in
      place, so `rx_linear()` can multiply a block by a half-built
      filter. That is a click or a burst of out-of-band audio, and
      dragging the bandwidth control makes many of them.
  - **The fix:** build the new coefficients into a second buffer with a
    plan made once at start-up, then swap a pointer.

## Performance, in order of likely cost

1. **The default build is `-O0`.** The Makefile's default `FLAGS` is
   `-g`. `OPT=o` gives `-march=native -O3 -flto`, but the update script
   runs plain `make`. Every per-bin and per-sample loop above runs
   unoptimized. FFTW itself is a prebuilt library and isn't affected.
   This is the largest and cheapest win. Fix the loopback line at the
   same time, since an optimizer is the thing most likely to expose its
   undefined behaviour.
2. **Double precision, complex FFTs of real data.**
   - The forward FFT (and the 1-in-6 spectrum FFT) could be real-to-complex,
     about half the work.
   - Single precision (`fftwf`) roughly doubles NEON throughput on the
     Pi's A53 cores, as it did for maxibitx.
   - The inverse has to stay complex, since its output is the analytic
     signal.
3. **NR over all 2048 bins.** The noise estimate, DSP and ANR each make
   at least one `cabs()` per bin, and DSP an `exp()` per bin, every
   block, although only the passband's bins survive the filter. With DSP
   and ANR on, this is comparable to the FFTs. Restricting it to the
   passband removes nearly all of it.
4. **Filter changes**, above: FFTW planning and wisdom file I/O in the
   control path, dropped blocks in CW, a race in SSB.
5. **The playback wait.** The playback PCM is opened blocking, yet the
   loop spins on `snd_pcm_avail()` until there's room (sbitx_sound.c:855).
   It does this on the SCHED_FIFO max-priority audio thread. Capture
   pacing usually leaves room, so the spin is rare, and the code logs any
   spin over 1 ms (`play_avail spin-wait`). When it happens it burns a
   core at top priority. A blocking `snd_pcm_writei()` alone waits
   without spinning.
6. **The capture backlog skip** costs nothing itself. It is the symptom
   of any of the above running late, and it is the first thing to watch
   for (`capture backlog: dropped N frames`).
7. **Small items:**
   - The bin rotation is a 2048-element copy. It could be folded into the
     filter multiply's indexing.
   - `iq_baseband` costs about 6 M multiply-adds a second, only with a
     client.
   - The EQ recomputes its coefficients every block. That is cheap; the
     problem is that it is wrong.

To measure sections on the radio, set `#define RX_TX_TIMING_DEBUG 1` at
sbitx.c:1240 (it is a plain `#define`, so `-D` on the command line won't
override it) and rebuild. Once a second, `rx_linear()` then prints
gather, forward FFT, spectrum, DSP, sideband+FIR, inverse FFT and tail
times, plus the worst call. Comparing those with DSP and ANR off and on,
and at `-O0` and `-O2`, puts numbers on items 1 to 3.

## Filtering, stage by stage

| Stage | Filter | Appropriate? |
|---|---|---|
| Front end | none in RX (LPFs TX-only) | Hardware choice; only very strong out-of-band signals matter |
| Crystal | ~35 kHz roofing | Yes as a roofing filter. Dial 7.5 kHz above center: I/Q span −10.9 to +19.9 kHz |
| IF/ADC | codec decimation filter | Possible slope over USB audio (prediction); IF above 48 kHz relies on the codec |
| Mix to baseband | exact bin rotation | Yes, if `center_bin` is even (unchecked) |
| Notch | bins × 0.001 | Brick wall; acceptable for a notch |
| DSP | per-bin spectral subtraction | Time-aliases (no windowed overlap-add) |
| ANR | per-bin gain + bin smoothing | Smoothing causes 93.75 Hz AM, −24 dB (simulated) |
| Sideband zero | brick wall | Redundant after the FIR; harmless |
| Channel FIR | Kaiser β 5, 1025 taps | Yes: ±0.01 dB, −61 to −80 dB, 230 Hz skirts |
| FT8 decimation | none, every 8th | Acceptable only because of the FIR |
| Remote audio | none, every 6th | Acceptable; limiter harmonics alias |
| Loopback | none, every 2nd | Acceptable; the line is undefined behaviour |
| TCI audio | decim48k | Yes |
| RX EQ | 5 parallel peaking biquads | No: wrong rate, state reset per block, parallel sum, int32 overflow |
| I/Q LPF | 63-tap Blackman | Yes, including for 48 kHz HPSDR |
