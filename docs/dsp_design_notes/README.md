# DSP design notes

Home for DSP design notes — measured data, derivations,
and concrete specs to build against, kept separate from the
"how the shipped code works" docs elsewhere in `docs/`.

Each doc in this folder should open with a `Status:` line stating whether
it's proposed, in progress, or implemented, following the convention
already used in `antialias_filter_design.md`.

## Contents

- [`antialias_filter_design.md`](antialias_filter_design.md) — measured
  crystal filter response and the FIR anti-alias filter spec derived
  from it for the RX chain. Status: implemented (`antialias.c`).
- [`tx_power_calibration.md`](tx_power_calibration.md) — why sbitx's
  per-band TX scale table doesn't transfer to minibitx's pipeline
  unchanged, and a step-by-step wattmeter procedure (one QRP CW
  frequency per band) to re-derive per-band values for a flat 5W.
  Status: done - all 9 bands bench-confirmed in the 4.7-5.5W target
  window.
- [`tx_test_tones_and_alc.md`](tx_test_tones_and_alc.md) — the TX
  test-tone generator (single and two-tone), the carrier-offset check
  and two-tone calibration it enables, and the digital ALC plan.
  Status: generator built and bench-tested; the rest proposed.
- [`tx_direct_to_dac_cw_history.md`](tx_direct_to_dac_cw_history.md)
  — the original CW-only TX scheme (a second, IF-shifted oscillator in
  `cw.c` and a clk2 correction in `radio.c`), formerly
  `03_tx_processing_pipeline.md`. Status: historical - replaced by
  `tx_pipeline.c`.
- [`usb_uac_decimation_design.md`](usb_uac_decimation_design.md) — the
  96kHz->48kHz decimating lowpass that makes `usb_gadget.c`'s UAC2
  gadget actually deliver the 48kHz it advertises, cascaded after
  `antialias_filter_design.md`'s own filter. Status: implemented
  (`decim48k.c`), bench-verified numerically and now against a real
  UAC2 host too (WSJT-X decoding FT8).
- [`rx_gain_and_level_calibration.md`](rx_gain_and_level_calibration.md)
  — how to approach checking/tuning RX gain now that IQ audio actually
  reaches a real UAC2 host: why raw peak levels are relative-only
  without a separate calibrated-signal-generator step, why that has to
  wait on confirming the WM8731 `Line` input level (currently an
  undocumented fixed 80%) is itself set correctly, and the recommended
  noise-floor/strong-signal checks to do first. Status: proposed - a
  design discussion recorded ahead of any code.
- [`rx_audio_demod_design.md`](rx_audio_demod_design.md) — the local CW
  audio monitor (`rx_audio.c`): the product-detector-plus-BFO design, why
  a fixed output gain couldn't work once real signal levels were
  measured on the bench (and the AGC that replaced it), the WM8731
  `Master` L/R independence fix that came out of the same debugging
  session, the two-stage (v3) filtering that replaced v1/v2's single
  combined filter - a wide complex image-reject bandpass decoupled from
  a separate, narrow post-demod selectivity filter - and, following an
  on-air report of a 3kHz-away CW signal still being clearly audible, the
  two-step chase to fix it: first a 4-section cascaded biquad (an
  improvement, but a resonator cascade's skirt never gets truly steep no
  matter how many sections), then replacing that outright with a fixed
  8-pole elliptic (Cauer) design - the same math a real crystal ladder
  filter's synthesis uses, landing in a real CW crystal filter's shape-
  factor range - deliberately not runtime-adjustable, plus the group-
  delay/ring-time cost that bought, and a settling-time fix to the test
  harness needed to measure the improvement correctly. Then a second
  on-air report - the sharp elliptic filter's own edges weren't
  audible - traced to the AGC measuring stage 3's output and canceling
  out its own selectivity before it reached the codec; fixed in two
  steps, first by moving the AGC's envelope to stage 2 (fixed the
  reported problem, but surfaced a further, more severe case of the
  real-audio folding effect), then by moving it again to the raw input
  I/Q itself, before stage 1 - frequency-independent by construction, so
  no downstream stage's selectivity can leak into the AGC's gain at all,
  which recovered the filter's full ~99dB selectivity in actual heard
  output and reproduced the folding artifact at its original, honest
  depth instead of hiding or exaggerating it.
  Status: implemented; first on-air CW copy confirmed (2026-09) against
  v1's filter, v2's image-reject filter also on-air confirmed (signals
  fade below ~500Hz sidetone pitch, matching the bench prediction), v3's
  stage split and the 4-section cascade are both bench-verified
  numerically but not yet re-confirmed on air; the raw-input AGC fix is
  bench-verified (including against the originally-reported problem) and
  now on-air confirmed for the single-signal case too (2026-09, W1AW code
  practice - the filter and AGC "work very nice"), but still not tested
  with two simultaneous signals in one buffer, on the bench or on air
  (see the doc's §8.8/§10).
- [`rx_narrow_filter_fft_vs_elliptic.md`](rx_narrow_filter_fft_vs_elliptic.md)
  — a measured comparison of stage 3's two implementations (the fixed
  8-pole elliptic IIR above, and `rx_filter.c`'s FFT overlap-save
  filter), prompted by an on-air report that the FFT one "lets a lot of
  wideband hiss and far-off-center signals get through." The headline
  result is that the reported symptom is **not reproduced**: the FFT
  filter is narrower at -3dB, 30-55dB deeper across the stopband, passes
  0.8dB less broadband noise, and sits 13.6dB quieter between CW
  elements. The real difference is time-domain and confined to keying
  transitions — 16.0ms of group delay against the elliptic's 2.6ms, and
  8.5ms of *pre*-ringing that a causal IIR cannot have at all. Also
  records a measurement error made and corrected along the way (a
  missing guard band around key transitions produced a
  plausible-looking +4.5dB result in the opposite direction), what was
  ruled out and how, the impulse-length tradeoff table if shortening
  `RX_FILTER_IMPULSE_LEN` turns out to be the answer, and why that
  constant is an accident of the FFT and ALSA period sizes rather than
  anything CW asked for. The note's §11 then acts on the time-domain
  finding: a minimum-phase realization of that same magnitude response
  (`filter_min_phase()`, rigctld `u`/`U MINPHASE`, on by default when the
  FFT filter is selected) cuts group delay to 4.5ms and a keyed element's
  settling time to 8.7ms, agrees with the linear-phase filter to within
  0.06dB across the passband, and has no pre-ringing at all — which also
  removes the mechanism behind the leading hypothesis for the "hiss"
  report. Then §12 takes the other route entirely: since the FFT filter
  existed to make pitch and width adjustable, and elliptic coefficients
  merely can't be designed *at runtime*, stage 3 now carries a bank of
  24 elliptic filters pre-designed offline (six pitches x four
  widths, `tools/gen_narrow_filters.py` -> `src/narrow_filter_bank.h`) and
  switches between them - quantized pitch/width at no CPU cost and no
  time-domain penalty, with the switching transient measured at 2dB
  settling in 14ms. Records why the pitch axis needs the software BFO to
  move with the filter (38dB of self-attenuation at the narrowest width if
  it doesn't). §13 then closes the gap §12 left: the TX sidetone has to move
  with the pitch too, because the pitch is the beat note meaning "he is on
  my dial," so zero-beating against a stale sidetone transmits off frequency
  by the difference - `radio_set_cw_pitch()` moves the RX BFO, the filter,
  the sidetone and the CW IF shift together. Records the measurement that
  the carrier stays on the dial to within half a rotate bin at every pitch,
  and the pre-existing ~9Hz low placement that measurement exposed. Still
  open: the choice of rungs, and which implementation should be the default.
- [`iq_stream_design.md`](iq_stream_design.md) — a third, minimal I/Q
  export path (`iq_stream.c`), independent of both `hpsdr_p1.c` (single-
  client - a second HPSDR client would silently steal its stream) and
  `usb_gadget.c`'s UAC2 gadget - built so `tools/rigctl_panel.py`'s
  spectrum display can get real I/Q without implementing either
  protocol, and so several such clients can be subscribed at once.
  Status: implemented, bench-verified end to end (wire format,
  multi-subscriber fan-out, subscriber timeout, the Python client's FFT
  decode/scaling) against synthetic test tones over localhost; not yet
  confirmed over a real LAN or against real RF.
- [`rx_uac_out_digital_mode_bandwidth.md`](rx_uac_out_digital_mode_bandwidth.md)
  — chases an on-air report that SparkSDR on `hpsdr_p1` I/Q decodes ~10x
  more FT8 than WSJT-X on the UAC2 gadget's audio (`rx_audio.c`'s
  `uac_out` tap). The root cause came from a simultaneous A/B, with
  both consumers fed the identical I/Q: `rx_audio.c` was a CW-only demod
  in every mode, adding a 700Hz pitch offset and - because maxibitx's
  raw I/Q is spectrally inverted - receiving the *lower* sideband.
  FT8 above dial only leaked through stage 1's -40..-75dB stopband,
  folded around 700Hz, while FT8 below dial arrived mirrored and
  undecodable. Along the way: passband sweeps with the narrow filter on
  and off, and a real but secondary clipping bug (zero headroom in
  `UAC_RX_AUDIO_SCALE`, now 15dB). The note keeps, and explicitly
  corrects, the earlier conclusions that stage 1 was fine and clipping
  was dominant, along with a misdesigned test that briefly got the
  right hypothesis dismissed. Status: **implemented** - a mode-aware
  demod (`rx_audio_set_demod()`, `DIGITAL` → USB, no BFO offset,
  sideband chosen by one input conjugation), CW bit-identical to
  before. **On-air confirmed (2026-09-21):** the inversion constant's
  sign was checked directly (+100Hz dial step moves signals left), and
  a simultaneous A/B against SparkSDR now matches it (~40 decodes
  each in one 20m interval, SNRs typically within 1dB) - the ~10x
  deficit is closed.
