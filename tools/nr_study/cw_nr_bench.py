#!/usr/bin/env python3
"""
cw_nr_bench.py - the bench behind
docs/dsp_design_notes/rx_cw_noise_reduction_study.md.

Keyed CW in white noise, at a range of signal-to-noise ratios, through a
model of the receiver's stage 3 (the FFT narrow filter: overlap-save,
N = 4096, L = 1024, a Kaiser-windowed bandpass of M = N - L + 1 taps at
96 kHz, as rx_filter.c builds it, but linear phase), then through each
candidate noise reduction:

  none      stage 3 alone
  narrow150 stage 3 at 150 Hz instead of 300, no NR - the baseline NR has
            to beat
  in_voice  spectral NR inside stage 3's own frames (per-bin gains on the
            4096-point spectrum before the inverse FFT), tuned as for voice
  in_cw     the same, tuned for CW (faster a-priori SNR, shallower floor)
  in_cw_sm  in_cw with the gains smoothed across five bins
  stft_cw   spectral NR in its own short-time FFT after stage 3, at 6 kHz
            (decimated by 16): 256-point sqrt-Hann frames, hop 64, the
            same 23.4 Hz bins and 10.7 ms hop as stage 3
  stft128   the same with 128-point frames, hop 32: 46.9 Hz bins, 21 ms
            frames
  n150+stft stft128 after stage 3 at 150 Hz
  lms       an adaptive line enhancer (the classic "ANR"), at 6 kHz
  n150+lms  lms after stage 3 at 150 Hz
  sbitx_flt the sbitx's own receive FFT (sbitx.c rx_process(): overlap-save,
            N = 2048, L = 1024, a 1025-tap filter), the same 300 Hz, no NR -
            the baseline for the two below
  sbitx_dsp that FFT with sbitx's "DSP" on (v5.401, W2JON/W4WHL): a
            slowly updated per-bin noise estimate, sigmoid spectral
            subtraction, a 10% noise residual, a 0.9/0.1 blend with the
            last frame
  sbitx_anr that FFT with sbitx's "ANR" on: a relaxed Wiener gain on
            smoothed magnitudes, then 0.8/0.1/0.1 smoothing across bins

The spectral NR is minimum-statistics noise tracking (Martin) with an
MMSE log-spectral-amplitude gain (Ephraim-Malah) and a gain floor.

For each SNR (dB in 2500 Hz, the WSJT-X convention) it prints:

  errors    dit-slot decision errors with the levels known: each slot's
            energy against the best single threshold for that output -
            a generous stand-in for copy, the same for every variant
  contrast  mean mark-slot energy over mean gap-slot energy, dB - how
            much quieter the gaps are than the elements
  gap       gap energy relative to stage 3 alone, dB (the noise removed)
  mark      mark energy relative to stage 3 alone, dB (signal lost)
  onset     at +10 dB, time for an element to reach -3 dB of its settled
            level, ms, after the variant's own delay is removed

and, for in_*, how much of the per-frame filter-times-gain impulse
response falls outside overlap-save's M taps (the time aliasing that
gains applied inside the filter's frames add).

  python3 tools/nr_study/cw_nr_bench.py              # the tables, about 8 min
  python3 tools/nr_study/cw_nr_bench.py --trials 1   # one noise draw, 2 min
  python3 tools/nr_study/cw_nr_bench.py --wav DIR    # also WAVs to listen to

Needs numpy and scipy.
"""
import argparse
import os
import wave

import numpy as np
import scipy.signal as ss
import scipy.special as sp

FS = 96000
L = 1024
N = 4096
M = N - L + 1
PITCH = 700.0
WIDTH = 300.0
WPM = 20
SNRS = (-21, -18, -15, -12, -9, -6, 0)
SECONDS = 24.0
DEC = 16                      # to 6 kHz for stft_cw and lms
FS_LO = FS // DEC

WAV_VARIANTS = ("none", "narrow150", "in_voice", "in_cw", "stft128", "n150+stft",
                "lms", "n150+lms", "sbitx_dsp", "sbitx_anr")

rng = np.random.default_rng(12345)


# ---- the signal --------------------------------------------------------------

MORSE = {"A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.", "G": "--.",
         "H": "....", "I": "..", "J": ".---", "K": "-.-", "L": ".-..", "M": "--", "N": "-.",
         "O": "---", "P": ".--.", "Q": "--.-", "R": ".-.", "S": "...", "T": "-", "U": "..-",
         "V": "...-", "W": ".--", "X": "-..-", "Y": "-.--", "Z": "--..", "0": "-----",
         "1": ".----", "2": "..---", "3": "...--", "4": "....-", "5": ".....",
         "6": "-....", "7": "--...", "8": "---..", "9": "----.", "/": "-..-."}


def keying(text, wpm):
    """Dit-unit slots, 1 = key down, for the text."""
    slots = []
    for word in text.split():
        for ch in word:
            for sym in MORSE[ch]:
                slots += [1] * (1 if sym == "." else 3) + [0]
            slots += [0, 0]
        slots += [0, 0, 0, 0]
    return np.array(slots)


def cw_signal(slots, wpm, n, rise_ms=5.0):
    """The keyed tone at PITCH, raised-cosine edges, amplitude 1."""
    unit = int(round(1.2 / wpm * FS))
    env = np.repeat(slots, unit).astype(float)
    k = int(rise_ms * 1e-3 * FS)
    ramp = np.sin(np.linspace(0, np.pi / 2, k)) ** 2
    env = np.convolve(env, np.ones(k) / k, mode="same")  # smooth, then shape
    env = np.clip(env, 0, 1)
    env = np.concatenate([env, np.zeros(max(0, n - len(env)))])[:n]
    t = np.arange(n) / FS
    del ramp
    return env * np.sin(2 * np.pi * PITCH * t), unit


def noise_for_snr(n, snr_db):
    """White noise whose power in 2500 Hz is snr_db below a unit tone's."""
    tone_power = 0.5
    p2500 = tone_power / 10 ** (snr_db / 10)
    sigma = np.sqrt(p2500 * (FS / 2) / 2500)
    return rng.standard_normal(n) * sigma


# ---- stage 3 ---------------------------------------------------------------------

def stage3_response(pitch=PITCH, width=WIDTH):
    """rx_filter.c's response: ideal bandpass, Kaiser beta 5, M taps."""
    lo, hi = (pitch - width / 2) / FS, (pitch + width / 2) / FS
    m = np.arange(M) - (M - 1) / 2
    h = 2 * hi * np.sinc(2 * hi * m) - 2 * lo * np.sinc(2 * lo * m)
    h *= np.kaiser(M, 5.0)
    return np.fft.fft(h, N), (M - 1) // 2


def overlap_save(x, H, frame_gain=None):
    """Stage 3. frame_gain(Y) -> per-bin gains for bins 0..N/2, applied to
    the filtered spectrum before the inverse FFT (the in_* variants)."""
    out = np.zeros(len(x))
    hist = np.zeros(M - 1)
    alias = []
    for b in range(len(x) // L):
        blk = x[b * L:(b + 1) * L]
        frame = np.concatenate([hist, blk])
        hist = frame[-(M - 1):]
        Y = np.fft.fft(frame) * H
        if frame_gain is not None:
            g = frame_gain(Y[: N // 2 + 1])
            G = np.concatenate([g, g[-2:0:-1]])
            Y *= G
            k = np.fft.ifft(G * H).real
            e = k ** 2
            alias.append(e[M:].sum() / e.sum())
        out[b * L:(b + 1) * L] = np.fft.ifft(Y).real[M - 1:]
    return out, alias


# ---- spectral NR -------------------------------------------------------------------

class SpectralNR:
    """Minimum-statistics noise tracking and an MMSE-LSA gain, on the bins
    given (the rest get the floor). One frame at a time."""

    def __init__(self, bins, hop_s, alpha_dd, floor_db, window_s=1.5, alpha_s=0.85, smooth=1):
        self.bins = bins
        self.smooth = smooth
        self.alpha_dd = alpha_dd
        self.gmin = 10 ** (floor_db / 20)
        self.alpha_s = alpha_s
        frames = max(8, int(window_s / hop_s))
        self.U, self.V = 8, max(1, frames // 8)
        self.P = None
        self.sub = []          # minima of finished subwindows
        self.cur = None
        self.count = 0
        self.prev = None       # |G * Y|^2 / noise of the last frame
        self.bias = 1.0

    def noise(self):
        mins = self.sub + ([self.cur] if self.cur is not None else [])
        return np.min(mins, axis=0) * self.bias

    def __call__(self, Yhalf):
        p = np.abs(Yhalf[self.bins]) ** 2
        self.P = p if self.P is None else self.alpha_s * self.P + (1 - self.alpha_s) * p
        self.cur = self.P.copy() if self.cur is None else np.minimum(self.cur, self.P)
        self.count += 1
        if self.count == self.V:
            self.sub = (self.sub + [self.cur])[-self.U:]
            self.cur, self.count = None, 0
        lam = np.maximum(self.noise(), 1e-30)
        gamma = p / lam
        ml = np.maximum(gamma - 1, 0)
        xi = ml if self.prev is None else self.alpha_dd * self.prev + (1 - self.alpha_dd) * ml
        xi = np.maximum(xi, 10 ** (-25 / 10))
        v = np.maximum(xi * gamma / (1 + xi), 1e-8)
        g = xi / (1 + xi) * np.exp(0.5 * sp.exp1(v))
        g = np.clip(g, self.gmin, 1.0)
        self.prev = g * g * gamma
        if self.smooth > 1:
            # Across frequency, after the a-priori update: a smoother gain
            # curve has a shorter impulse response (less time aliasing
            # inside overlap-save) and fewer isolated "musical" bins.
            k = np.ones(self.smooth) / self.smooth
            g = np.convolve(np.pad(g, self.smooth // 2, mode="edge"), k, mode="valid")
        full = np.full(len(Yhalf), self.gmin)
        full[self.bins] = g
        return full


def passband_bins(nfft, fs, margin_hz=150):
    f = np.arange(nfft // 2 + 1) * fs / nfft
    return np.where((f > PITCH - WIDTH / 2 - margin_hz) & (f < PITCH + WIDTH / 2 + margin_hz))[0]


def calibrate_bias(make, run):
    """Sets the minimum-statistics bias so a noise-only input is estimated
    at its true mean (Martin's bias, found numerically for these windows)."""
    nr = make()
    nr.bias = 1.0
    x = rng.standard_normal(int(8 * FS))
    true_p, est = [], []
    orig = nr.__call__

    def probe(Yhalf):
        out = orig(Yhalf)
        true_p.append(np.mean(np.abs(Yhalf[nr.bins]) ** 2))
        est.append(np.mean(nr.noise()))
        return out
    run(x, probe)
    return np.mean(true_p[200:]) / np.mean(est[200:])


def stft_nr(x, nr, nfft=256, hop=64):
    # Offline, the frames are put back where they came from, so the delay
    # measured here is zero; run live, each output sample waits for the
    # last frame covering it: about nfft samples at FS_LO, plus the
    # decimation and interpolation filters (study §6).
    """Decimate by DEC, WOLA with sqrt-Hann (75% overlap), interpolate."""
    lo = ss.resample_poly(x, 1, DEC)
    w = np.sqrt(ss.windows.hann(nfft, sym=False))
    norm = np.sum(w ** 2) / hop
    y = np.zeros(len(lo) + nfft)
    for s in range(0, len(lo) - nfft, hop):
        Xf = np.fft.rfft(lo[s:s + nfft] * w)
        y[s:s + nfft] += np.fft.irfft(Xf * nr(Xf), nfft) * w / norm
    return ss.resample_poly(y[:len(lo)], DEC, 1)[:len(x)]


# ---- the sbitx's NR, as v5.401's sbitx.c has it -----------------------------

NS, LS = 2048, 1024
MS = LS + 1                   # filter_new(1024, 1025)


def sbitx_response(width=WIDTH):
    lo, hi = (PITCH - width / 2) / FS, (PITCH + width / 2) / FS
    m = np.arange(MS) - (MS - 1) / 2
    h = (2 * hi * np.sinc(2 * hi * m) - 2 * lo * np.sinc(2 * lo * m)) * np.kaiser(MS, 5.0)
    return np.fft.fft(h, NS)


class SbitxNR:
    """rx_process()'s per-bin DSP and ANR, line for line, on the
    positive-frequency half (mirrored for this real signal)."""

    def __init__(self, dsp, anr, update_interval=50):
        self.dsp, self.anr, self.interval = dsp, anr, update_interval
        self.noise = np.zeros(NS // 2 + 1)
        self.signal = np.zeros(NS // 2 + 1)
        self.prev = np.zeros(NS // 2 + 1)
        self.counter, self.initialized = 0, False

    def __call__(self, Yfull):
        Y = Yfull[: NS // 2 + 1].copy()
        if not self.initialized or self.counter >= self.interval:
            mag = np.abs(Y)
            a = np.where(mag > self.noise, 0.95, 0.75)
            self.noise = np.maximum(1e-6, a * self.noise + (1 - a) * mag)
            self.counter, self.initialized = 0, True
        else:
            self.counter += 1
        if self.dsp:
            mag = np.abs(Y)
            snr = mag / (self.noise + 1e-6)
            rf = 1 / (1 + np.exp(-5 * (snr - 0.5)))
            new = np.maximum(0.10 * self.noise, mag - rf * self.noise)
            new = 0.9 * new + 0.1 * self.prev
            self.prev = new
            Y = new * np.exp(1j * np.angle(Y))
        out = np.concatenate([Y, np.conj(Y[-2:0:-1])])
        if self.anr:
            mag = np.abs(out[: NS // 2 + 1])
            self.signal = 0.9 * self.signal + 0.1 * mag
            sp_, np_ = np.maximum(1e-6, self.signal ** 2), np.maximum(1e-6, self.noise ** 2)
            w = np.maximum(0.2, (sp_ + 0.2 * np_) / (sp_ + np_))
            out *= np.concatenate([w, w[-2:0:-1]])
            # "Bin smoothing": a convolution across bins, so a multiplication
            # in time - by 0.8 + 0.2 cos(2 pi n / N) over the frame.
            sm = out.copy()
            sm[1:-1] = 0.8 * out[1:-1] + 0.1 * out[:-2] + 0.1 * out[2:]
            out = sm
        return out


def sbitx_rx(x, H, nr=None, warm=None):
    """Overlap-save at N = 2048, L = 1024: the NR on the input's spectrum,
    then the filter, then the second half of the inverse FFT, as
    rx_process() does. warm: noise run through first, then dropped, so the
    noise estimate starts settled, as it is on a radio that has been on."""
    if warm is not None:
        x = np.concatenate([warm, x])
    out = np.zeros(len(x))
    hist = np.zeros(LS)
    for b in range(len(x) // LS):
        blk = x[b * LS:(b + 1) * LS]
        X = np.fft.fft(np.concatenate([hist, blk]))
        hist = blk
        if nr is not None:
            X = nr(X)
        out[b * LS:(b + 1) * LS] = np.fft.ifft(X * H).real[LS:]
    return out[len(warm):] if warm is not None else out


def lms(x, delay=48, taps=64, mu=0.01, leak=0.1):
    """Adaptive line enhancer: predicts x from its own past (NLMS); the
    prediction keeps what repeats - a steady tone - and loses the noise.
    Leaky (WDSP's ANR leaks too): after a narrow filter the input's correlation
    matrix is nearly singular, and plain LMS lets the weights drift in the
    directions it can't see until the out-of-band residue is amplified."""
    lo = ss.resample_poly(x, 1, DEC)
    w = np.zeros(taps)
    y = np.zeros(len(lo))
    buf = np.zeros(taps + delay)
    for n in range(len(lo)):
        buf = np.roll(buf, 1)
        buf[0] = lo[n]
        ref = buf[delay:delay + taps]
        y[n] = w @ ref
        e = lo[n] - y[n]
        w = (1 - mu * leak) * w + mu * e * ref / (ref @ ref + 1e-12)
    return ss.resample_poly(y, DEC, 1)[:len(x)]


# ---- measuring -----------------------------------------------------------------------

def delay_of(y, ref):
    """Samples y lags ref by, from the envelope cross-correlation."""
    a = np.abs(ss.hilbert(ref))[::8]
    b = np.abs(ss.hilbert(y))[::8]
    c = ss.correlate(b - b.mean(), a - a.mean(), mode="full", method="fft")
    return (np.argmax(c) - (len(a) - 1)) * 8


def slot_energies(y, slots, unit, lag):
    e = []
    for i in range(len(slots)):
        a = lag + i * unit + unit // 6
        b = lag + (i + 1) * unit - unit // 6
        e.append(np.mean(y[a:b] ** 2) if 0 <= a < b <= len(y) else np.nan)
    return np.array(e)


def slot_errors(e, slots):
    ok = ~np.isnan(e)
    e, s = e[ok], slots[ok]
    best = len(s)
    for th in np.quantile(e, np.linspace(0.01, 0.99, 197)):
        best = min(best, np.sum((e > th) != (s == 1)))
    return best / len(s)


def onset_ms(y, slots, unit, lag):
    """Mean time from an element's start to -3 dB of its settled level."""
    env = np.abs(ss.hilbert(y))
    times = []
    starts = [i for i in range(1, len(slots)) if slots[i] == 1 and slots[i - 1] == 0]
    for i in starts:
        a = lag + i * unit
        if a + unit > len(env):
            break
        seg = env[a - unit // 2: a + unit]
        settled = np.median(env[a + unit // 2: a + unit])
        idx = np.argmax(seg[unit // 2:] >= settled * 10 ** (-3 / 20))
        times.append(idx / FS * 1e3)
    return float(np.median(times))


def run(args):
    global WPM
    WPM = args.wpm
    text = "CQ POTA DE KB2ML KB2ML K " * 4
    slots = keying(text, WPM)
    n = int(SECONDS * FS)
    sig, unit = cw_signal(slots, WPM, n)
    slots = slots[: n // unit]
    H, h_delay = stage3_response()
    bins_in = passband_bins(N, FS)
    bins_lo = passband_bins(256, FS_LO)

    def make_in(alpha, floor):
        return lambda: SpectralNR(bins_in, L / FS, alpha, floor)

    def make_lo():
        return SpectralNR(bins_lo, 64 / FS_LO, 0.80, -15)

    def make_lo128():
        return SpectralNR(passband_bins(128, FS_LO), 32 / FS_LO, 0.80, -15)

    bias_in = calibrate_bias(make_in(0.98, -20), lambda x, f: overlap_save(x, H, f))
    filt_noise, _ = overlap_save(rng.standard_normal(int(8 * FS)), H)
    bias_lo = calibrate_bias(make_lo, lambda x, f: stft_nr(x, f))
    bias_lo128 = calibrate_bias(make_lo128, lambda x, f: stft_nr(x, f, 128, 32))
    print(f"minimum-statistics bias, found numerically: in-filter {bias_in:.2f}, "
          f"short-time FFT {bias_lo:.2f}")
    del filt_noise

    variants = ("none", "narrow150", "in_voice", "in_cw", "in_cw_sm", "stft_cw", "stft128",
                "n150+stft", "lms", "n150+lms", "sbitx_flt", "sbitx_dsp", "sbitx_anr")
    HS = sbitx_response()
    H150, _ = stage3_response(width=150.0)
    rows = {v: {} for v in variants}
    aliases = {"in_voice": [], "in_cw": [], "in_cw_sm": []}
    delays = {}
    onset = {}
    wavs = {}
    for snr in SNRS + (10,):
      for trial in range(1 if snr == 10 else args.trials):
        x = sig + noise_for_snr(n, snr)
        y0, _ = overlap_save(x, H)
        outs = {"none": y0, "narrow150": overlap_save(x, H150)[0]}
        for name, alpha, floor, sm in (("in_voice", 0.98, -20, 1), ("in_cw", 0.80, -15, 1),
                                       ("in_cw_sm", 0.80, -15, 5)):
            nr = make_in(alpha, floor)()
            nr.bias = bias_in
            nr.smooth = sm
            outs[name], al = overlap_save(x, H, nr)
            aliases[name] += al[100:]
        nr = make_lo()
        nr.bias = bias_lo
        outs["stft_cw"] = stft_nr(y0, nr)
        nr = make_lo128()
        nr.bias = bias_lo128
        outs["stft128"] = stft_nr(y0, nr, 128, 32)
        nr = make_lo128()
        nr.bias = bias_lo128
        outs["n150+stft"] = stft_nr(outs["narrow150"], nr, 128, 32)
        outs["lms"] = lms(y0)
        outs["n150+lms"] = lms(outs["narrow150"])
        warm = noise_for_snr(int(40 * FS), snr)
        outs["sbitx_flt"] = sbitx_rx(x, HS)
        outs["sbitx_dsp"] = sbitx_rx(x, HS, SbitxNR(True, False), warm)
        outs["sbitx_anr"] = sbitx_rx(x, HS, SbitxNR(False, True), warm)
        clean0, _ = overlap_save(sig, H)
        e0 = slot_energies(y0, slots, unit, h_delay)
        mark, gap = slots == 1, slots == 0
        for v in variants:
            y = outs[v]
            lag = delay_of(y, clean0) + h_delay if v not in ("none", "narrow150") else h_delay
            if snr == 10:
                delays[v] = (lag - h_delay) / FS * 1e3
                onset[v] = onset_ms(y, slots, unit, lag)
                continue
            e = slot_energies(y, slots, unit, lag)
            rows[v].setdefault(snr, []).append((
                slot_errors(e, slots),
                10 * np.log10(np.nanmean(e[mark]) / np.nanmean(e[gap])),
                10 * np.log10(np.nanmean(e[gap]) / np.nanmean(e0[gap])),
                10 * np.log10(np.nanmean(e[mark]) / np.nanmean(e0[mark]))))
            if args.wav and trial == 0 and snr in (-15, -9) and v in WAV_VARIANTS:
                wavs[(v, snr)] = y

    print(f"\nKeyed CW, {WPM} WPM, pitch {PITCH:.0f} Hz, stage 3 {WIDTH:.0f} Hz wide; "
          f"SNR in 2500 Hz; mean of {args.trials} noise draws (errors: lowest-highest)\n")
    print("variant    SNR  errors  (range)       contrast   gap dB  mark dB")
    for v in variants:
        for snr in SNRS:
            r = np.array(rows[v][snr])
            err, con, gp, mk = r.mean(axis=0)
            rng_ = f"({100 * r[:, 0].min():4.1f}-{100 * r[:, 0].max():4.1f})"
            print(f"{v:9s} {snr:4d}  {100 * err:5.1f}%  {rng_:12s}  {con:7.1f}  {gp:7.1f}  {mk:7.1f}")
        print()
    print("added delay and element onset at +10 dB:")
    for v in variants:
        print(f"  {v:9s} delay {delays[v]:6.1f} ms   onset to -3 dB {onset[v]:5.1f} ms")
    for name, al in aliases.items():
        a = 10 * np.log10(np.array(al) + 1e-20)
        print(f"time aliasing, {name}: filter x gain response outside M taps, "
              f"median {np.median(a):.1f} dB, 95th percentile {np.percentile(a, 95):.1f} dB")

    # The ANR's bin smoothing, on a steady tone: the output envelope
    # averaged at each position within the 1024-sample block.
    tone = np.sin(2 * np.pi * PITCH * np.arange(int(8 * FS)) / FS)
    tx = tone + noise_for_snr(len(tone), 20)
    for name, nr in (("sbitx_flt", None), ("sbitx_anr", SbitxNR(False, True))):
        y = sbitx_rx(tx, HS, nr, noise_for_snr(int(40 * FS), 20) if nr else None)
        env = np.abs(ss.hilbert(y))[2 * FS:]
        env = env[: len(env) // LS * LS].reshape(-1, LS).mean(axis=0)
        print(f"steady tone, {name}: envelope across each 1024-sample block "
              f"{20 * np.log10(env.min() / env.max()):.1f} dB min to max")

    if args.wav:
        os.makedirs(args.wav, exist_ok=True)
        for snr in (-15, -9):
            ref = np.sqrt(np.mean(wavs[("none", snr)] ** 2))
            for v in WAV_VARIANTS:
                y = wavs[(v, snr)] / ref * 0.1
                # 12 kHz: everything after stage 3 is below 1.5 kHz.
                y12 = ss.resample_poly(y, 1, 8)
                pcm = np.clip(y12 * 32767, -32767, 32767).astype("<i2")
                path = os.path.join(args.wav, f"cw_{snr:+d}dB_{v}.wav")
                with wave.open(path, "wb") as f:
                    f.setnchannels(1)
                    f.setsampwidth(2)
                    f.setframerate(FS // 8)
                    f.writeframes(pcm.tobytes())
        print(f"\nWAVs in {args.wav}: the same 24 s at -15 and -9 dB, each variant at "
              f"the same scale as stage 3 alone")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--wav", metavar="DIR", help="also write WAVs to listen to")
    ap.add_argument("--trials", type=int, default=4, help="noise draws per SNR (default 4)")
    ap.add_argument("--wpm", type=int, default=WPM, help=f"keying speed (default {WPM})")
    run(ap.parse_args())
