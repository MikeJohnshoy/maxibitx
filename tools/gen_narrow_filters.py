#!/usr/bin/env python3
"""Designs stage 3's elliptic narrow-filter bank and writes src/narrow_filter_bank.h.

rx_audio.c's stage 3 used to carry one hand-pasted 4x5 coefficient table
for a single 700Hz/300Hz filter, with the scipy call that produced it
recorded only in a comment. This script replaces that arrangement: it
designs every (pitch, width) combination the radio offers, verifies each
one numerically, and emits the header rx_audio.c includes.

Why a script rather than more pasted numbers. Twelve sets is 240
coefficients, which is past the point where a transcription error would be
caught by eye - and an elliptic section's poles sit close enough to the
unit circle that a wrong digit can mean an unstable filter rather than a
slightly wrong one. Everything below the design call is therefore a check,
and nothing is written unless all twelve pass.

Run it from anywhere:

    python3 tools/gen_narrow_filters.py            # rewrite the header
    python3 tools/gen_narrow_filters.py --check    # verify it is current

Design point, recovered by matching the coefficients rx_audio.c shipped
before this script existed (they agree to 0.0e+00, so the bank's 700Hz /
300Hz entry is bit-for-bit what the radio ran):

    scipy.signal.ellip(4, 0.5, 50, [pitch - width/2, pitch + width/2],
                       btype='bandpass', output='sos', fs=96000)

4th-order elliptic, 0.5dB passband ripple, 50dB stopband - 8 poles as 4
direct-form biquad sections, rows {b0, b1, b2, a1, a2} with a0 == 1
(scipy's sos convention, minus the constant column).
"""

import argparse
import datetime
import pathlib
import sys

import numpy as np
from scipy import signal

FS = 96000.0
PITCHES_HZ = [500, 600, 700, 800, 900, 1000]
WIDTHS_HZ = [150, 300, 450, 600]
SECTIONS = 4

ORDER = 4
PASSBAND_RIPPLE_DB = 0.5
STOPBAND_DB = 50

# The coefficients rx_audio.c carried before this script existed. Checked,
# not trusted: if the design call below ever stops reproducing these, the
# 700/300 filter has silently changed and the radio's default behavior
# with it, which is exactly the kind of drift a generated file invites.
SHIPPED_700_300 = np.array([
    [0.0031347125317966271, -0.0062262711904984037, 0.003134712531796628,
     -1.9880750263609597, 0.99051500563718153],
    [1, -1.9997094875296553, 1, -1.9906166255419793, 0.99224567397195218],
    [1, -1.9948828058283654, 0.99999999999999989, -1.9932472877766807,
     0.99636250549261574],
    [1, -1.9992168539672679, 0.99999999999999978, -1.9963811875913184,
     0.99766425853740737],
])

# Acceptance thresholds. Each one is set with margin against what the
# twelve sets actually measure (see the numbers this script prints), so a
# failure means something changed, not that a limit was drawn too fine.
MAX_POLE_RADIUS = 0.9999      # measured worst: 0.99933 (150Hz width)
MAX_PEAK_GAIN_DB = 0.5        # measured: within 0.01dB of unity
MAX_PITCH_DROOP_DB = 1.0      # measured worst: 0.49dB (ripple, not error)
WIDTH_TOLERANCE = 0.15        # measured: -3dB width is 1.08x nominal
MAX_STOPBAND_DB = -45.0       # measured: -50.0dB, the design's own floor
MAX_ATTACK_MS = 15.0          # measured worst: 9.2ms (150Hz width)
MAX_RESIDUAL = 1e-6           # measured worst: 7.1e-14 after 500ms


def design(pitch_hz, width_hz):
    """The one design call. Everything else in this file checks its output."""
    sos = signal.ellip(ORDER, PASSBAND_RIPPLE_DB, STOPBAND_DB,
                       [pitch_hz - width_hz / 2.0, pitch_hz + width_hz / 2.0],
                       btype='bandpass', output='sos', fs=FS)
    return sos[:, [0, 1, 2, 4, 5]]  # drop scipy's constant a0 column


def as_sos(coeffs):
    """Back to scipy's 6-column sos, for its analysis helpers."""
    return np.insert(coeffs, 3, 1.0, axis=1)


def run_direct_form_1(coeffs, x):
    """Bit-for-bit what rx_audio.c's biquad_apply() does, in the same order.

    Not sosfilt: the point is to exercise the arithmetic the radio actually
    performs, in double, including the state recursion that a high-Q
    section stresses. A design that only behaves under scipy's own
    implementation would be no use here.
    """
    y = np.asarray(x, dtype=np.float64).copy()
    for b0, b1, b2, a1, a2 in coeffs:
        x1 = x2 = y1 = y2 = 0.0
        out = np.empty_like(y)
        for n, v in enumerate(y):
            r = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1 = x1, v
            y2, y1 = y1, r
            out[n] = r
        y = out
    return y


def keyed_tone(pitch_hz, seconds, ramp_ms=5.0):
    n = int(seconds * FS)
    ramp = int(ramp_ms / 1000.0 * FS)
    env = np.ones(n)
    env[:ramp] = 0.5 - 0.5 * np.cos(np.pi * np.arange(ramp) / ramp)
    return env * np.cos(2 * np.pi * pitch_hz * np.arange(n) / FS)


def running_rms(y, window_ms=2.0):
    w = int(window_ms / 1000.0 * FS)
    c = np.cumsum(np.concatenate([[0.0], y * y]))
    return np.sqrt((c[w:] - c[:-w]) / w)


def measure(pitch_hz, width_hz, coeffs):
    """Everything the acceptance gate below needs, all measured."""
    sos = as_sos(coeffs)
    _, poles, _ = signal.sos2zpk(sos)
    pole_radius = float(np.abs(poles).max())

    f, h = signal.sosfreqz(sos, worN=200000, fs=FS)
    mag = 20 * np.log10(np.abs(h) + 1e-300)
    peak_db = float(mag.max())
    peak_i = int(mag.argmax())
    pitch_db = float(np.interp(pitch_hz, f, mag)) - peak_db

    # Contiguous -3dB region around the peak, so a stopband null can't be
    # mistaken for a passband edge.
    lo = peak_i
    while lo > 0 and mag[lo] >= peak_db - 3.0:
        lo -= 1
    hi = peak_i
    while hi < len(mag) - 1 and mag[hi] >= peak_db - 3.0:
        hi += 1
    width_3db = float(f[hi] - f[lo])

    # The equiripple floor, well clear of the transition region.
    far = ((f > pitch_hz + 3 * width_hz) | (f < pitch_hz - 3 * width_hz)) & (f < 20000)
    stopband_db = float(mag[far].max()) - peak_db

    # Attack: key-down to -3dB of the filter's own settled level.
    y = run_direct_form_1(coeffs, keyed_tone(pitch_hz, 0.25))
    env = running_rms(y)
    settled = env[-1]
    attack_ms = float(np.argmax(env >= settled * 0.70710678) / FS * 1000.0)

    # Decay: excite, then feed silence. A high-Q section that rings for
    # half a second, or a limit cycle that never decays at all, shows here.
    excite = np.concatenate([keyed_tone(pitch_hz, 0.05), np.zeros(int(0.5 * FS))])
    residual = float(np.abs(run_direct_form_1(coeffs, excite)[-int(0.05 * FS):]).max())

    return dict(pole_radius=pole_radius, peak_db=peak_db, pitch_db=pitch_db,
                width_3db=width_3db, stopband_db=stopband_db,
                attack_ms=attack_ms, residual=residual)


def check(pitch_hz, width_hz, m):
    """Returns a list of human-readable failures - empty means acceptable."""
    bad = []
    if not m['pole_radius'] < MAX_POLE_RADIUS:
        bad.append(f"pole radius {m['pole_radius']:.7f} >= {MAX_POLE_RADIUS}")
    if abs(m['peak_db']) > MAX_PEAK_GAIN_DB:
        bad.append(f"passband peak {m['peak_db']:+.2f}dB is not near unity")
    if abs(m['pitch_db']) > MAX_PITCH_DROOP_DB:
        bad.append(f"gain at {pitch_hz}Hz is {m['pitch_db']:+.2f}dB below peak")
    if abs(m['width_3db'] / width_hz - 1.0) > WIDTH_TOLERANCE:
        bad.append(f"-3dB width {m['width_3db']:.0f}Hz vs {width_hz}Hz nominal")
    if m['stopband_db'] > MAX_STOPBAND_DB:
        bad.append(f"stopband floor {m['stopband_db']:+.1f}dB")
    if m['attack_ms'] > MAX_ATTACK_MS:
        bad.append(f"attack {m['attack_ms']:.1f}ms > {MAX_ATTACK_MS}ms")
    if m['residual'] > MAX_RESIDUAL:
        bad.append(f"still ringing at {m['residual']:.2e} after 500ms of silence")
    return bad


def c_double(v):
    """A C literal that reads back as this exact double.

    repr() gives the shortest string that round-trips, which is what makes
    the emitted header reproduce the design rather than approximate it.
    """
    s = repr(float(v))
    return s + ('.0' if ('.' not in s and 'e' not in s and 'inf' not in s) else '')


def emit(bank, metrics):
    L = []
    a = L.append
    a("// narrow_filter_bank.h")
    a("//")
    a("// GENERATED FILE - do not edit by hand. Rewrite it with:")
    a("//")
    a("//     python3 tools/gen_narrow_filters.py")
    a("//")
    a("// See that script for the design call, why this is generated rather")
    a("// than pasted, and the checks every set below had to pass. See")
    a("// docs/dsp_design_notes/rx_narrow_filter_fft_vs_elliptic.md for why")
    a("// stage 3 offers a bank of fixed elliptic filters at all, rather than")
    a("// the continuously tunable FFT filter that sits alongside it.")
    a("//")
    a("// Each entry is an 8-pole elliptic (Cauer) bandpass as 4 direct-form")
    a("// biquad sections, rows {b0, b1, b2, a1, a2} with a0 == 1. Measured,")
    a("// not claimed - these are this script's own numbers for the")
    a("// coefficients actually below:")
    a("//")
    a("//   pitch  width   pole r   -3dB w   stopband   attack")
    for (p, w), m in metrics.items():
        a(f"//   {p:5d}  {w:5d}  {m['pole_radius']:.6f}  {m['width_3db']:6.0f}Hz"
          f"  {m['stopband_db']:7.1f}dB  {m['attack_ms']:5.1f}ms")
    a("//")
    a("// The stopband figure is the equiripple floor, which is what an")
    a("// elliptic design buys and also all it ever buys - it does not keep")
    a("// descending with distance the way the FFT filter's does.")
    a("")
    a("#ifndef NARROW_FILTER_BANK_H")
    a("#define NARROW_FILTER_BANK_H")
    a("")
    a(f"#define NARROW_BANK_PITCHES {len(PITCHES_HZ)}")
    a(f"#define NARROW_BANK_WIDTHS {len(WIDTHS_HZ)}")
    a(f"#define NARROW_BANK_SECTIONS {SECTIONS}")
    a("")
    a("// Selectable pitches and widths, ascending - rx_audio.c snaps a")
    a("// requested value to the nearest entry and reports back which one it")
    a("// landed on.")
    a("static const int narrow_bank_pitch_hz[NARROW_BANK_PITCHES] = {")
    a("\t" + ", ".join(str(p) for p in PITCHES_HZ) + ",")
    a("};")
    a("")
    a("static const int narrow_bank_width_hz[NARROW_BANK_WIDTHS] = {")
    a("\t" + ", ".join(str(w) for w in WIDTHS_HZ) + ",")
    a("};")
    a("")
    a("static const double")
    a("narrow_bank_coeffs[NARROW_BANK_PITCHES][NARROW_BANK_WIDTHS][NARROW_BANK_SECTIONS][5] = {")
    for pi, p in enumerate(PITCHES_HZ):
        a(f"\t{{ // pitch {p} Hz")
        for wi, w in enumerate(WIDTHS_HZ):
            m = metrics[(p, w)]
            a(f"\t\t{{ // {p} Hz, {w} Hz wide"
              f" - measured {m['width_3db']:.0f}Hz at -3dB, {m['attack_ms']:.1f}ms attack")
            for row in bank[pi][wi]:
                a("\t\t\t{ " + ", ".join(c_double(v) for v in row) + " },")
            a("\t\t},")
        a("\t},")
    a("};")
    a("")
    a("#endif /* NARROW_FILTER_BANK_H */")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=pathlib.Path, default=None,
                    help="where to write the header (default: src/narrow_filter_bank.h "
                         "next to this script's repository)")
    ap.add_argument('--check', action='store_true',
                    help="verify the existing header is current; write nothing")
    args = ap.parse_args()

    out = args.out or (pathlib.Path(__file__).resolve().parent.parent
                       / 'src' / 'narrow_filter_bank.h')

    bank = [[design(p, w) for w in WIDTHS_HZ] for p in PITCHES_HZ]

    # The regression gate: the default filter must still be the one the
    # radio already ran.
    drift = np.abs(bank[PITCHES_HZ.index(700)][WIDTHS_HZ.index(300)]
                   - SHIPPED_700_300).max()
    print(f"700Hz/300Hz vs the coefficients rx_audio.c shipped: "
          f"max difference {drift:.1e}")
    if drift != 0.0:
        sys.exit("FAIL: the default filter is no longer bit-for-bit what the radio ran")

    print(f"\nDesigning {len(PITCHES_HZ)}x{len(WIDTHS_HZ)} sets at Fs={FS:.0f} Hz, "
          f"order {ORDER}, {PASSBAND_RIPPLE_DB}dB ripple, {STOPBAND_DB}dB stopband\n")
    print("pitch width   pole r     peak   @pitch   -3dB w   stopband  attack   residual")

    metrics, failures = {}, []
    for pi, p in enumerate(PITCHES_HZ):
        for wi, w in enumerate(WIDTHS_HZ):
            m = measure(p, w, bank[pi][wi])
            metrics[(p, w)] = m
            bad = check(p, w, m)
            print(f"{p:5d} {w:5d}  {m['pole_radius']:.6f} {m['peak_db']:+7.2f} "
                  f"{m['pitch_db']:+7.2f} {m['width_3db']:8.0f} {m['stopband_db']:9.1f} "
                  f"{m['attack_ms']:7.1f} {m['residual']:10.1e}"
                  f"{'   FAIL' if bad else ''}")
            failures += [f"{p}Hz/{w}Hz: {b}" for b in bad]

    if failures:
        print("\n" + "\n".join("  FAIL " + f for f in failures))
        sys.exit(f"\n{len(failures)} check(s) failed - nothing written")

    text = emit(bank, metrics)

    if args.check:
        current = out.read_text() if out.exists() else None
        if current == text:
            print(f"\n{out} is current.")
            return
        sys.exit(f"\n{out} is stale - rerun without --check")

    out.write_text(text)
    print(f"\nAll {len(metrics)} sets pass. Wrote {out} "
          f"({len(text.splitlines())} lines).")


if __name__ == '__main__':
    main()
