#!/usr/bin/env python3
"""
cw_shape.py - measures the keying of a CW signal heard through maxibitx's
I/Q stream: its keying sidebands against ARRL's Clean Signal Initiative
mask, its rise and fall times, its speed and weighting, and its chirp.

    cw_shape.py --host sbitx.local                 # the strongest signal near the dial
    cw_shape.py --host sbitx.local --offset 420    # the one 420 Hz above the dial
    cw_shape.py --host sbitx.local --save sig.npy  # keep the recording
    cw_shape.py --replay sig.npy                   # measure a recording again
    cw_shape.py --simulate dl1ycf --snr 80         # a synthetic signal: check the tool

It records --seconds of I/Q (30 by default), finds the signal, brings it to
zero and decimates it to 6 kHz, then measures:

  - Keying sidebands: the strongest level, over the recording, beyond each
    offset from the carrier, each side, relative to the carrier, from
    frames of 1024 samples at 6 kHz (5.9 Hz bins, Blackman-Harris) held at
    their peak. The CSI mask as DL1YCF reads it - the mask's 180, 300 and
    675 Hz are bandwidths, so these are offsets - is -20 dBc beyond 90 Hz,
    -40 beyond 150 and -60 beyond 338; DL1YCF's own -80, -100 and -120 dBc
    beyond 600, 900 and 1200 Hz are shown for comparison.
  - Rise and fall times, 10% to 90% of the key-down level, the median over
    every element that starts and ends at the floor.
  - Speed and weighting from the dots' length between the 50% points.
  - Chirp: the frequency in the first 3 ms after each element reaches 90%,
    against the rest of the element.

What it can see. Off the air, a signal is measured only as far down as
the noise lets it: a signal 50 dB above the noise can't show -60 dBc.
Each mask point therefore says PASS, FAIL, or "can't tell" when what
rises above the limit is only the noise (held at its peak, from the
recording's silent frames, and within 3 dB of it). A neighbouring signal inside 2.4 kHz shows up as a FAIL on its
side - check the plot (--plot, needs matplotlib) before believing one.
Fading changes the carrier's level between frames and smears the result;
stronger and steadier is better. A measurement of real hardware ahead of
ARRL's lab numbers, not in place of them.

Your own transmitter: the sBitx can't hear itself (receive is muted in
transmit). Key it into a dummy load, couple a small sample of its output
into a second radio running maxibitx (the zBitx), and point this at that
radio. Keep the sample well below the receiver's overload: the peak level
is printed, and above -6 dBFS the receiver itself may be adding the
splatter you are trying to measure.

--simulate SHAPE makes a 96 kHz recording instead: bursts of dots at
--wpm with gaps, 500 Hz above the dial, in noise --snr dB below the
carrier in 2500 Hz, quantised as iq_stream.c sends it. SHAPE is dl1ycf
(cw.c's ramp, 9 ms), bh5 (cw.c's earlier ramp: the rising half of a
Blackman-Harris window, 5 ms, with its 1.56 ms weighting hold) or hard
(0.1 ms). docs/dsp_design_notes/tx_cw_keying_spectrum.md has the
background and the numbers; test-cw-shape checks the radio's own keying
on the bench.

Needs numpy; --plot needs matplotlib.
"""
import argparse
import csv
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xtal_sweep import IQ_RATE_HZ, IQ_STREAM_PORT, RIGCTL_PORT, IqStream, Rigctl, np  # noqa: E402

DECIM = 16                         # 96 kHz to 6 kHz
RATE = IQ_RATE_HZ / DECIM
KEEP_HZ = 2400                     # the decimated band, either side
FRAME = 1024                       # 5.86 Hz bins at 6 kHz
HOP = FRAME // 4
LOCATE_NFFT = 16384
DC_GUARD_HZ = 30                   # the dial's own zero is skipped when searching
CSI_MASK = ((90, -20), (150, -40), (338, -60))
DL1YCF_EXTRA = ((600, -80), (900, -100), (1200, -120))
CANT_TELL_DB = 3                   # noise within this of a limit: can't tell
CHIRP_EARLY_MS = 3.0

# cw.c's ramp: DL1YCF's coefficients for sin(2 pi k y), k = 1 to 5.
DL1YCF_COEFF = (-0.12182865361171612, -0.018557469249199286, -0.0009378783245428506,
                0.0008567571519403228, 0.00018706912431472442)


# ---- simulation ----

def ramp(shape):
    """(edge 0 to 1 at 96 kHz, weighting hold in samples)"""
    if shape == "dl1ycf":
        y = np.arange(865) / 864.0
        r = y.copy()
        for k, a in enumerate(DL1YCF_COEFF, 1):
            r += a * np.sin(2 * np.pi * k * y)
        return r, 0
    if shape == "bh5":
        n = np.arange(960)
        x = 2 * np.pi * n / 959
        w = 0.35875 - 0.48829 * np.cos(x) + 0.14128 * np.cos(2 * x) - 0.01168 * np.cos(3 * x)
        return w[:480] / w[479], 150
    return np.linspace(0, 1, 11), 0


def simulate(shape, wpm, snr_db, seconds, seed=1):
    edge, hold = ramp(shape)
    dot = int(round(1.2 / wpm * IQ_RATE_HZ))
    key = np.zeros(int(seconds * IQ_RATE_HZ), dtype=bool)
    burst, gap = int(2.0 * IQ_RATE_HZ), int(1.0 * IQ_RATE_HZ)
    t = int(0.5 * IQ_RATE_HZ)
    while t + burst < len(key):
        for s in range(t, t + burst - 2 * dot, 2 * dot):
            key[s:s + dot] = True
        t += burst + gap
    # cw.c's walk: one table step a sample toward the key, falls held off.
    env = np.empty(len(key))
    pos, held = 0, 0
    top = len(edge) - 1
    for i, k in enumerate(key):
        if k:
            held = hold
        elif held:
            held -= 1
            k = True
        pos = min(pos + 1, top) if k else max(pos - 1, 0)
        env[i] = edge[pos]
    n = np.arange(len(env))
    amp = 10 ** (-10 / 20)                      # carrier at -10 dBFS
    sig = amp * env * np.exp(2j * np.pi * 500.0 * n / IQ_RATE_HZ)
    rng = np.random.default_rng(seed)
    sigma = amp / math.sqrt(10 ** (snr_db / 10) * 2500.0 / IQ_RATE_HZ)
    sig += sigma / math.sqrt(2) * (rng.standard_normal(len(n)) + 1j * rng.standard_normal(len(n)))
    return np.round(sig * 32767.0) / 32767.0   # iq_stream.c's int16


# ---- recording ----

def record(host, seconds):
    stream = IqStream(host, IQ_STREAM_PORT)
    stream.flush()
    want = int(seconds * IQ_RATE_HZ)
    parts, got, gaps = [], 0, 0
    print(f"recording {seconds:g} s of I/Q from {host}...", flush=True)
    for contiguous, samples in stream.packets(want):
        if not contiguous and parts:
            gaps += 1
        parts.append(samples)
        got += len(samples)
    if got < want // 2:
        sys.exit("the I/Q stream stopped - is maxibitx running and reachable on UDP 4536?")
    if gaps:
        print(f"  {gaps} gaps in the stream (lost packets); each one is a small click "
              f"that can show as sideband energy")
    return np.concatenate(parts)


def dial_of(host):
    try:
        r = Rigctl(host, RIGCTL_PORT)
        f = r.get_int("f")
        r.close()
        return f
    except OSError:
        return None


# ---- analysis ----

def locate(x, offset, search_hz):
    """The signal's frequency in Hz from the dial: near --offset if given,
    else the strongest within search_hz, refined between bins."""
    n = LOCATE_NFFT
    w = np.hanning(n)
    acc = np.zeros(n)
    frames = 0
    for s in range(0, len(x) - n, n):
        acc += np.abs(np.fft.fft(x[s:s + n] * w)) ** 2
        frames += 1
    f = np.fft.fftfreq(n, 1 / IQ_RATE_HZ)
    if offset is not None:
        allowed = np.abs(f - offset) <= 100
    else:
        allowed = (np.abs(f) <= search_hz) & (np.abs(f) > DC_GUARD_HZ)
    k = int(np.flatnonzero(allowed)[np.argmax(acc[allowed])])
    a, b, c = (10 * np.log10(acc[(k + d) % n] + 1e-30) for d in (-1, 0, 1))
    frac = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
    return (k + frac) * IQ_RATE_HZ / n


def baseband(x, f0):
    """x mixed down by f0, band-limited to +-KEEP_HZ and decimated to RATE."""
    n = np.arange(len(x))
    y = x * np.exp(-2j * np.pi * f0 * n / IQ_RATE_HZ)
    Y = np.fft.fft(y)
    f = np.abs(np.fft.fftfreq(len(y), 1 / IQ_RATE_HZ))
    taper = np.clip((2900.0 - f) / (2900.0 - KEEP_HZ), 0, 1)
    taper = 0.5 - 0.5 * np.cos(np.pi * taper)
    return np.fft.ifft(Y * taper)[::DECIM]


def bh4(n):
    x = 2 * np.pi * np.arange(n) / (n - 1)
    return 0.35875 - 0.48829 * np.cos(x) + 0.14128 * np.cos(2 * x) - 0.01168 * np.cos(3 * x)


def key_down_level(env):
    top = np.percentile(env, 99.5)
    return float(np.median(env[env > 0.5 * top]))


def spectra(b, level):
    """(freqs, peak-held dBc, average dBc, noise peak-held dBc or None,
    silent frames)"""
    w = bh4(FRAME)
    env = np.abs(b)
    peak = np.full(FRAME, 1e-30)
    avg = np.zeros(FRAME)
    quiet = np.full(FRAME, 1e-30)
    n_frames = n_quiet = 0
    for s in range(0, len(b) - FRAME, HOP):
        p = np.abs(np.fft.fftshift(np.fft.fft(b[s:s + FRAME] * w))) ** 2
        if env[s:s + FRAME].max() < 0.1 * level:
            quiet = np.maximum(quiet, p)
            n_quiet += 1
        else:
            peak = np.maximum(peak, p)
            avg += p
            n_frames += 1
    f = np.fft.fftshift(np.fft.fftfreq(FRAME, 1 / RATE))
    ref = peak.max()
    db = lambda v: 10 * np.log10(v / ref + 1e-30)
    noise = db(quiet) if n_quiet >= 5 else None
    return f, db(peak), db(avg / max(n_frames, 1)), noise, n_quiet


def beyond(f, v, hz, side):
    sel = (f > hz) & (f <= KEEP_HZ) if side > 0 else (f < -hz) & (f >= -KEEP_HZ)
    return float(v[sel].max())


def cross(env, i, level, step):
    """The fractional index where env crosses level, walking from i by step
    (+1/-1), or None within 30 ms."""
    j = i
    for _ in range(int(0.03 * RATE)):
        k = j + step
        if k < 0 or k >= len(env):
            return None
        if (env[j] - level) * (env[k] - level) <= 0:
            a, b = env[j], env[k]
            return j + step * ((level - a) / (b - a) if b != a else 0.0)
        j = k
    return None


def elements(env, level):
    """Marks between the 50% points, with their 10% and 90% points:
    dicts of up50, down50, up10, up90, down90, down10 (fractional indices)."""
    above = env > 0.5 * level
    idx = np.flatnonzero(np.diff(above.astype(np.int8)))
    marks = []
    ups = idx[above[idx + 1]]
    downs = idx[~above[idx + 1]]
    di = 0
    for u in ups:
        while di < len(downs) and downs[di] <= u:
            di += 1
        if di >= len(downs):
            break
        d = downs[di]
        if (d - u) / RATE < 0.003:
            continue
        m = {"up50": cross(env, u, 0.5 * level, 1) or float(u),
             "down50": cross(env, d, 0.5 * level, 1) or float(d)}
        m["up10"] = cross(env, u, 0.1 * level, -1)
        m["up90"] = cross(env, u, 0.9 * level, 1)
        m["down90"] = cross(env, d + 1, 0.9 * level, -1)
        m["down10"] = cross(env, d, 0.1 * level, 1)
        marks.append(m)
    return marks


def timing(marks):
    rises = [(m["up90"] - m["up10"]) / RATE * 1e3 for m in marks
             if m["up10"] is not None and m["up90"] is not None and m["up90"] < m["down50"]]
    falls = [(m["down10"] - m["down90"]) / RATE * 1e3 for m in marks
             if m["down10"] is not None and m["down90"] is not None and m["down90"] > m["up50"]]
    lens = np.array([(m["down50"] - m["up50"]) / RATE for m in marks])
    spaces = np.array([(b["up50"] - a["down50"]) / RATE for a, b in zip(marks, marks[1:])])
    out = {"rises": rises, "falls": falls, "wpm": None, "weight": None}
    if len(lens) >= 5:
        dots = lens[lens < 2 * np.percentile(lens, 20)]
        if len(dots):
            dot = float(np.median(dots))
            out["wpm"] = 1.2 / dot
            el_sp = spaces[spaces < 2 * dot]
            if len(el_sp):
                out["weight"] = dot / float(np.median(el_sp))
    return out


def chirp(b, marks):
    """Median of (frequency in the first CHIRP_EARLY_MS after 90%) minus
    (the rest of the mark), over marks long enough, in Hz; and how many."""
    phase = np.unwrap(np.angle(b))
    inst = np.diff(phase) * RATE / (2 * np.pi)
    early_n = int(CHIRP_EARLY_MS * 1e-3 * RATE)
    diffs = []
    for m in marks:
        if m["up90"] is None or m["down90"] is None:
            continue
        a, z = int(math.ceil(m["up90"])), int(m["down90"])
        if z - a < 3 * early_n:
            continue
        diffs.append(float(np.mean(inst[a:a + early_n]) - np.mean(inst[a + early_n:z])))
    return (float(np.median(diffs)), len(diffs)) if diffs else (None, 0)


def pct(v):
    v = np.array(v)
    return f"{np.median(v):.2f} ms (10-90% of elements: {np.percentile(v, 10):.2f} to " \
           f"{np.percentile(v, 90):.2f}, {len(v)} elements)"


def judge(worst, limit, noise_at):
    """PASS below the limit; "can't tell" when what is above it is the
    noise (the worst level is within CANT_TELL_DB of the noise there)."""
    if worst <= limit:
        return "PASS"
    if noise_at is not None and worst <= noise_at + CANT_TELL_DB:
        return "can't tell"
    return "FAIL"


def report(x, f0, dial, label):
    peak_dbfs = 20 * math.log10(max(np.abs(x.real).max(), np.abs(x.imag).max(), 1e-12))
    b = baseband(x, f0)
    env = np.abs(b)
    level = key_down_level(env)
    f, held, avg, noise, n_quiet = spectra(b, level)
    where = f"{(dial + f0) / 1e6:.6f} MHz" if dial else f"{f0:+.1f} Hz from the dial"
    print(f"\n{label}: signal at {where}")
    print(f"  stream peak {peak_dbfs:.1f} dBFS" +
          ("  - above -6 dBFS: the receiver may be overloaded" if peak_dbfs > -6 else ""))
    if noise is None:
        far = np.abs(f) > 1800
        floor = float(np.median(held[far]))
        print(f"  fewer than 5 silent frames: noise taken as the median beyond 1.8 kHz, "
              f"{floor:.1f} dBc")
        noise_at = lambda hz, side: floor
    else:
        print(f"  noise held at its peak, from {n_quiet} silent frames: "
              f"{float(np.median(noise[np.abs(f) > 300])):.1f} dBc")
        noise_at = lambda hz, side: beyond(f, noise, hz, side)

    print("  keying sidebands, strongest beyond each offset (below / above the carrier):")
    for hz, limit in CSI_MASK:
        lo, hi = beyond(f, held, hz, -1), beyond(f, held, hz, 1)
        print(f"    CSI     {hz:4d} Hz  {limit:5d} dBc:  {lo:6.1f} {judge(lo, limit, noise_at(hz, -1)):10s}"
              f"  {hi:6.1f} {judge(hi, limit, noise_at(hz, 1))}")
    for hz, limit in DL1YCF_EXTRA:
        lo, hi = beyond(f, held, hz, -1), beyond(f, held, hz, 1)
        print(f"    DL1YCF  {hz:4d} Hz  {limit:5d} dBc:  {lo:6.1f} {judge(lo, limit, noise_at(hz, -1)):10s}"
              f"  {hi:6.1f} {judge(hi, limit, noise_at(hz, 1))}")

    marks = elements(env, level)
    t = timing(marks)
    print(f"  rise 10-90%: " + (pct(t["rises"]) if t["rises"] else "none measured"))
    print(f"  fall 90-10%: " + (pct(t["falls"]) if t["falls"] else "none measured"))
    if t["wpm"]:
        w = f", dot:space {t['weight']:.2f}" if t["weight"] else ""
        print(f"  speed {t['wpm']:.1f} WPM from the dots{w}")
    c, n = chirp(b, marks)
    if c is not None:
        print(f"  chirp: {c:+.1f} Hz in the first {CHIRP_EARLY_MS:g} ms against the rest "
              f"(median of {n} elements)")
    return f, held, avg, noise


def write_csv(path, f, held, avg, noise):
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["offset_hz", "peak_held_dbc", "average_dbc", "noise_peak_held_dbc"])
        for i in range(len(f)):
            w.writerow([f"{f[i]:.2f}", f"{held[i]:.2f}", f"{avg[i]:.2f}",
                        "" if noise is None else f"{noise[i]:.2f}"])
    print(f"  spectrum written to {path}")


def plot(path, f, held, noise, title):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("  --plot needs matplotlib (sudo apt install python3-matplotlib)")
        return
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(f, held, lw=0.9, label="signal, peak held")
    if noise is not None:
        ax.plot(f, noise, lw=0.7, color="0.6", label="noise, peak held")
    for pts, style, name in ((CSI_MASK, "-", "CSI mask"), (DL1YCF_EXTRA, "--", "DL1YCF")):
        for side in (-1, 1):
            xs, ys = [], []
            for i, (hz, lim) in enumerate(pts):
                end = pts[i + 1][0] if i + 1 < len(pts) else KEEP_HZ
                xs += [side * hz, side * end]
                ys += [lim, lim]
            ax.plot(xs, ys, style, color="C3", lw=1.2, label=name if side > 0 else None)
    ax.set_xlim(-1500, 1500)
    ax.set_ylim(-130, 5)
    ax.set_xlabel("offset from carrier, Hz")
    ax.set_ylabel("dBc")
    ax.set_title(title)
    ax.grid(alpha=0.3)
    ax.legend(loc="upper right")
    fig.tight_layout()
    fig.savefig(path, dpi=120)
    print(f"  plot written to {path}")


def main():
    ap = argparse.ArgumentParser(description="CW keying measurement from maxibitx's I/Q stream")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--host", help="maxibitx's host")
    src.add_argument("--replay", help="measure a recording made with --save")
    src.add_argument("--simulate", choices=("dl1ycf", "bh5", "hard"),
                     help="measure a synthetic signal with this keying")
    ap.add_argument("--offset", type=float, help="the signal's offset from the dial, Hz")
    ap.add_argument("--search", type=float, default=3000,
                    help="without --offset, search this far either side of the dial (Hz)")
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--save", help="save the recording (.npy)")
    ap.add_argument("--csv", help="write the spectrum to this CSV")
    ap.add_argument("--plot", help="draw the spectrum and the mask to this PNG")
    ap.add_argument("--wpm", type=float, default=40, help="--simulate's speed")
    ap.add_argument("--snr", type=float, default=80,
                    help="--simulate's carrier to noise in 2500 Hz, dB")
    a = ap.parse_args()

    dial = None
    if a.host:
        dial = dial_of(a.host)
        x = record(a.host, a.seconds)
        label = time.strftime("%Y-%m-%d %H:%M:%S")
    elif a.replay:
        x = np.load(a.replay)
        label = os.path.basename(a.replay)
    else:
        x = simulate(a.simulate, a.wpm, a.snr, a.seconds)
        label = f"simulated {a.simulate}, {a.wpm:g} WPM, {a.snr:g} dB SNR"
    if a.save:
        np.save(a.save, x.astype(np.complex64))
        print(f"  recording saved to {a.save}")
    f0 = locate(x, a.offset, a.search)
    f, held, avg, noise = report(x, f0, dial, label)
    if a.csv:
        write_csv(a.csv, f, held, avg, noise)
    if a.plot:
        plot(a.plot, f, held, noise, label)


if __name__ == "__main__":
    main()
