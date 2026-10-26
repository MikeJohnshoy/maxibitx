#!/usr/bin/env python3
"""capture_check.py - look at a raw-capture dump from maxibitx.

maxibitx writes one second of the codec's capture, both channels, before
any processing, to /tmp/maxibitx_capture_<date>_<time>.wav when it gets
SIGUSR1 (`pkill -USR1 maxibitx`; src/capture_dump.h). This reads one and
prints, for each channel and between them, the measures that tell a
codec or I2S fault from a quiet band:

  level       RMS and peak (dBFS), DC, how many low bits are always zero
              (8 for a 24-bit codec in 32-bit slots), and the share of
              positive samples (about half for any real signal)
  even/odd    RMS of the even samples against the odd ones, and how often
              a sample repeats its neighbour - a gain step between
              alternate samples, or a codec at half rate repeating each
              sample, folds the spectrum about fs/4
  mirror      (L, the receiver's IF at fs/4 = 24 kHz) how far the spectrum
              above 24 kHz matches the spectrum below it, mirrored: about
              0 for a band of independent stations, near 1 when every
              signal has an image on the other side; and the strongest
              signals with the level of the spectrum at their mirror
  L and R     correlation between the channels, at no lag and one sample
              either way - a channel slip shows as one channel's audio in
              the other
  timing      (R, the quiet mic line) the codec's 128-sample timing
              pattern, measured as maxibitx's start-up check measures it
              (src/codec_check.h): under 0.01 clean, about 0.9 when the
              WM8731 has started patterned and every signal has copies
              every 750 Hz (strongest at 3750 Hz)

Usage:
    tools/capture_check.py FILE.wav [FILE.wav ...]

Python 3 with numpy.
"""

import struct
import sys

import numpy as np

IF_HZ_OF_RATE = 0.25  # the RX IF sits at fs/4
MIRROR_SPAN_HZ = 20000  # each side of the IF compared
MIRROR_SKIP_HZ = 300  # around the IF itself (the carrier, the LO's leak)
PEAKS = 8
TIMING_PERIOD = 128  # codec_check.h's CODEC_CHECK_PERIOD, _HARMONIC and _BAD
TIMING_HARMONIC = 5
TIMING_BAD = 0.3
SIGNAL_DB = 20  # above the noise: a signal, for the mirror measures


def read_wav(path):
    """The rate and an (n, channels) int64 array from a PCM WAV file."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        tag, size = data[pos : pos + 4], struct.unpack("<I", data[pos + 4 : pos + 8])[0]
        body = data[pos + 8 : pos + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif tag == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        raise ValueError("no fmt or data chunk")
    kind, channels, rate, _, _, bits = fmt
    if kind != 1 or bits not in (16, 24, 32):
        raise ValueError(f"PCM format {kind}, {bits} bits: not read here")
    width = bits // 8
    n = len(pcm) // (width * channels)
    raw = np.frombuffer(pcm[: n * width * channels], dtype=np.uint8).reshape(-1, width)
    # little-endian, sign-extended into int64
    x = np.zeros(len(raw), dtype=np.int64)
    for b in range(width):
        x |= raw[:, b].astype(np.int64) << (8 * b)
    x = np.where(x >= 1 << (bits - 1), x - (1 << bits), x)
    return rate, bits, x.reshape(n, channels)


def db(v):
    return 20 * np.log10(max(v, 1e-30))


def level(x, bits):
    full = float(1 << (bits - 1))
    rms = np.sqrt(np.mean((x / full) ** 2))
    peak = np.max(np.abs(x)) / full
    dc = np.mean(x) / full
    nz = np.bitwise_or.reduce(x.astype(np.int64) & ((1 << bits) - 1)) if len(x) else 0
    zero_bits = 0
    while zero_bits < bits and not (nz >> zero_bits) & 1:
        zero_bits += 1
    positive = np.mean(x > 0)
    return rms, peak, dc, zero_bits, positive


def even_odd(x):
    xf = x.astype(np.float64) - np.mean(x)
    e, o = np.sqrt(np.mean(xf[0::2] ** 2)), np.sqrt(np.mean(xf[1::2] ** 2))
    ratio_db = db(e / o) if o > 0 else float("inf")
    nonzero = x != 0
    rep_even = np.mean((x[0:-1:2] == x[1::2])[nonzero[0:-1:2]]) if nonzero.any() else 0.0
    rep_odd = np.mean((x[1:-1:2] == x[2::2])[nonzero[1:-1:2]]) if nonzero.any() else 0.0
    return ratio_db, rep_even, rep_odd


def spectrum_dbfs(x, full):
    """Hann-windowed power per bin, in dB relative to a full-scale sine."""
    xf = x.astype(np.float64) / full
    xf -= np.mean(xf)
    w = np.hanning(len(xf))
    s = np.abs(np.fft.rfft(xf * w) / (np.sum(w) / 2)) ** 2
    return 10 * np.log10(s + 1e-30)


def sliding_max(v, k):
    """Each point's largest neighbour within k points."""
    pad = np.concatenate([np.full(k, -np.inf), v, np.full(k, -np.inf)])
    return np.max(np.lib.stride_tricks.sliding_window_view(pad, 2 * k + 1), axis=1)


def mirror(x, rate, bits):
    """How far the spectrum about the IF mirrors itself, the noise level
    (dBFS per bin), and the strongest signals with the spectrum at their
    mirror."""
    s = spectrum_dbfs(x, float(1 << (bits - 1)))
    hz = rate / len(x)  # per bin
    centre = int(round(rate * IF_HZ_OF_RATE / hz))
    lo, hi = int(MIRROR_SKIP_HZ / hz), int(MIRROR_SPAN_HZ / hz)
    hi = min(hi, centre - 1, len(s) - 1 - centre)
    d = np.arange(lo, hi)
    # each bin's strongest neighbour within ~5 Hz, so a signal and an image
    # a bin or two away still meet
    k = max(1, int(5 / hz))
    up, down = sliding_max(s[centre + d], k), sliding_max(s[centre - d], k)
    floor = float(np.median(np.concatenate([s[centre + d], s[centre - d]])))
    # only what stands SIGNAL_DB above the noise counts
    a = np.maximum(up - floor - SIGNAL_DB, 0)
    b = np.maximum(down - floor - SIGNAL_DB, 0)
    den = np.sqrt(np.dot(a, a) * np.dot(b, b))
    corr = float(np.dot(a, b) / den) if den > 0 else 0.0
    # the strongest signals, either side; a pair - a signal and its mirror -
    # is listed once, at the stronger of the two
    both = np.concatenate([up, down])
    taken, peaks = np.zeros(len(both), bool), []
    guard = max(1, int(200 / hz))
    for i in np.argsort(both)[::-1]:
        if len(peaks) >= PEAKS or both[i] - floor < SIGNAL_DB:
            break
        if taken[i]:
            continue
        j = i % len(d)
        for at in (j, j + len(d)):  # it and its mirror
            taken[max(0, at - guard) : at + guard] = True
        side = 1 if i < len(d) else -1
        img = (down if side == 1 else up)[j]
        peaks.append((side * d[j] * hz, both[i], img))
    return corr, floor, peaks


def timing_pattern(x):
    """The 5th harmonic of the power of successive-sample differences,
    folded over a 128-sample cycle, over its mean (codec_check_score())."""
    m = (len(x) - 1) // TIMING_PERIOD * TIMING_PERIOD
    d = np.diff(x[: m + 1].astype(np.float64))
    power = (d**2).reshape(-1, TIMING_PERIOD).sum(0)
    k = np.arange(TIMING_PERIOD)
    h = np.sum(power * np.exp(-2j * np.pi * TIMING_HARMONIC * k / TIMING_PERIOD))
    return float(2 * abs(h) / TIMING_PERIOD / np.mean(power)) if power.any() else 0.0


def lag_corr(a, b, lag):
    a = a.astype(np.float64) - np.mean(a)
    b = b.astype(np.float64) - np.mean(b)
    if lag > 0:
        a, b = a[lag:], b[:-lag]
    elif lag < 0:
        a, b = a[:lag], b[-lag:]
    den = np.sqrt(np.dot(a, a) * np.dot(b, b))
    return float(np.dot(a, b) / den) if den > 0 else 0.0


def check(path):
    rate, bits, x = read_wav(path)
    n, channels = x.shape
    print(f"{path}: {n} frames, {rate} Hz, {channels} channels, {bits} bits")
    names = ["L (RX IF)", "R (mic)"] + [f"ch{c}" for c in range(2, channels)]
    for c in range(channels):
        rms, peak, dc, zero_bits, positive = level(x[:, c], bits)
        ratio_db, rep_e, rep_o = even_odd(x[:, c])
        print(f"  {names[c]}")
        print(
            f"    level     RMS {db(rms):6.1f} dBFS, peak {db(peak):6.1f} dBFS, DC {dc:+.2e},"
            f" {zero_bits} low bits always 0, {positive:.0%} positive"
        )
        print(
            f"    even/odd  even RMS {ratio_db:+.2f} dB from odd; a sample repeats the one"
            f" before it {rep_e:.1%} (even) / {rep_o:.1%} (odd) of the time"
        )
    corr, floor, peaks = mirror(x[:, 0], rate, bits)
    print(f"  mirror about {rate * IF_HZ_OF_RATE / 1000:g} kHz on L: {corr:.2f}"
          f" (0 independent, 1 every signal imaged); noise {floor:.0f} dBFS per {rate / n:g} Hz")
    for off, lvl, img in peaks:
        print(f"    {off:+8.0f} Hz from the IF  {lvl:6.1f} dBFS, {img:6.1f} at its mirror"
              f" ({lvl - img:5.1f} dB below)")
    if channels >= 2:
        print(
            "  L and R    correlation {:+.3f} at no lag, {:+.3f} with R a sample late,"
            " {:+.3f} a sample early".format(
                lag_corr(x[:, 0], x[:, 1], 0), lag_corr(x[:, 0], x[:, 1], 1),
                lag_corr(x[:, 0], x[:, 1], -1)
            )
        )
        t = timing_pattern(x[:, 1])
        verdict = "patterned: ghosts every 750 Hz" if t >= TIMING_BAD else "clean"
        print(f"  timing     the codec's 128-sample pattern on R: {t:.3f} ({verdict})")


def main():
    if len(sys.argv) < 2:
        print(__doc__.split("Usage:")[1].split("Python 3")[0].strip())
        return 2
    status = 0
    for path in sys.argv[1:]:
        try:
            check(path)
        except (OSError, ValueError) as e:
            print(f"{path}: {e}")
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
