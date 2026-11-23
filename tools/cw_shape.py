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

It records --seconds of I/Q (30 by default), finds the signal (the
strongest at its peak, so a keyed signal rather than a steady carrier),
brings it to zero and decimates it to 6 kHz - centring on the strongest
peak within 150 Hz, if that is not where it was first found - then
measures:

  - Keying sidebands: the strongest level, over the recording, beyond each
    offset from the carrier, each side, within 1500 Hz, relative to the
    carrier, from frames of 1024 samples at 6 kHz (5.9 Hz bins,
    Blackman-Harris) held at their peak, with where it is. The CSI mask as
    DL1YCF reads it - the mask's 180, 300 and 675 Hz are bandwidths, so
    these are offsets - is -20 dBc beyond 90 Hz, -40 beyond 150 and -60
    beyond 338; DL1YCF's own -80, -100 and -120 dBc beyond 600, 900 and
    1200 Hz are shown for comparison.
  - Speed and weighting, from the dots' length between the 50% points of
    an envelope within 100 Hz of the carrier, which noise doesn't break up:
    a mark starts at 60% and ends at 40% of the key-down level, so fading
    doesn't split it, and the dots are found by clustering the marks'
    lengths, so a stray fragment isn't taken for one.
  - Where the sidebands come from: 43 ms frames inside long elements,
    centred on the edges, and in the gaps, compared at 150, 338 and 600
    Hz. Keying sidebands are only on the edges; phase noise or hum, of
    the transmitter or the receiver, is there in steady key-down too.
  - Rise and fall times, 10% to 90% of the key-down level, the median over
    the elements, and chirp: the frequency in the first 3 ms after each
    element reaches 90%, against the rest of it. Measured on the widest
    envelope - within 1200, 600, 300 or 150 Hz - that is at least 26 dB
    above the noise and clear of any other station; the filter's own rise
    time is printed beside them, and a rise near it isn't resolved. Too
    weak for 150 Hz, they are left out.

What it can see. Off the air, a signal is measured only as far down as
the noise lets it. The noise is measured from 1.6 to 2.3 kHz from the
carrier, on whichever side is quieter; held at its peak it reaches about
6 dB above its median, and that level is printed and drawn. Each mask
point says PASS, FAIL, "can't tell" when the level above the limit is
within 6 dB of the noise (a sideband and the noise in the same bin add),
or "another signal?" when something stronger
than -20 dBc lies past 338 Hz - a neighbouring station, most likely;
check the plot (--plot, needs matplotlib). A peak beyond 150 Hz that
stands 10 dB above the same offset on the carrier's other side is taken
to be another station whatever its level - keying sidebands come in
mirrored pairs - named, and left out of the mask. Fading changes the carrier's
level between frames and smears the result; stronger and steadier is
better. A measurement of real hardware ahead of ARRL's lab numbers, not
in place of them.

Your own transmitter: the sBitx can't hear itself (receive is muted in
transmit). Key it into a dummy load, couple a small sample of its output
into a second radio running maxibitx (the zBitx), and point this at that
radio. Keep the sample well below the receiver's overload: the peak level
is printed, and above -6 dBFS the receiver itself may be adding the
splatter you are trying to measure.

--simulate SHAPE makes a 96 kHz recording instead: bursts of dots at
--wpm with gaps, 500 Hz above the dial, in noise --snr dB below the
carrier in 2500 Hz, quantised as iq_stream.c sends it; --neighbour HZ
adds a steady carrier that far from it, --neighbour-dbc (-6) dB from it. SHAPE is dl1ycf
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
SEARCH_HZ = 1500                   # the mask is checked within this of the carrier
RECENTRE_HZ = 150                  # the analysis centres on the strongest peak within this
NOISE_HZ = (1600, 2300)            # the noise is measured here, the quieter side
NOISE_PEAK_DB = 6                  # peak-held noise's peaks above its median
CLEAR_DB = 6                       # a FAIL must stand this far above the noise
NEIGHBOUR_DBC = -20                # stronger than this past 338 Hz: another station?
ASYMMETRY_DB = 10                  # a peak this far above its mirror image: another station
NEIGHBOUR_FROM_HZ = 150            # another station is looked for this far out and beyond
MARK_HZ = 100                      # the envelope that finds the elements, either side
MARK_ON, MARK_OFF = 0.6, 0.4       # a mark starts above, and ends below, these of key-down
TIMING_HZ = (1200, 600, 300, 150)  # rise and fall: the widest the noise allows
TIMING_SNR_DB = 26                 # the 10% point is -20 dB, plus 6 dB of margin
CHIRP_EARLY_MS = 3.0
SPLIT_FRAME = 256                  # steady key-down against the edges: 43 ms, 23 Hz bins
SPLIT_MARGIN_S = 0.010             # steady frames keep this clear of either edge
SPLIT_OFFSETS_HZ = (150, 338, 600)
SPLIT_CLEAR_DB = 3                 # this close to the gaps: nothing there
SPLIT_KEYING_DB = 10               # the edges this far above steady key-down: keying

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


def simulate(shape, wpm, snr_db, seconds, neighbour_hz=None, neighbour_dbc=-6.0, seed=1):
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
    amp = 10 ** (-30 / 20)                      # carrier at -30 dBFS
    sig = amp * env * np.exp(2j * np.pi * 500.0 * n / IQ_RATE_HZ)
    if neighbour_hz:                            # a steady carrier, neighbour_dbc from it
        sig += 10 ** (neighbour_dbc / 20) * amp * np.exp(
            2j * np.pi * (500.0 + neighbour_hz) * n / IQ_RATE_HZ)
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
    else the strongest within search_hz, refined between bins. Strongest at
    its peak, not on average, so a keyed signal wins over a steady carrier
    of the same strength."""
    n = LOCATE_NFFT
    w = np.hanning(n)
    acc = np.zeros(n)
    for s in range(0, len(x) - n, n // 2):
        acc = np.maximum(acc, np.abs(np.fft.fft(x[s:s + n] * w)) ** 2)
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


def lowpass(b, hz):
    """b kept within +-hz of the carrier: a zero-phase filter, flat to hz and
    falling to nothing at 1.25 hz."""
    F = np.fft.fft(b)
    f = np.abs(np.fft.fftfreq(len(b), 1 / RATE))
    t = np.clip((1.25 * hz - f) / (0.25 * hz), 0, 1)
    return np.fft.ifft(F * (0.5 - 0.5 * np.cos(np.pi * t)))


def filter_rise_ms(hz):
    """lowpass(hz)'s own 10-90% rise for a hard step: what it can resolve."""
    step = np.concatenate([np.zeros(3000), np.ones(3000)]).astype(complex)
    y = np.abs(lowpass(step, hz))[1500:4500]
    t10 = np.argmax(y >= 0.1)
    t90 = np.argmax(y >= 0.9)
    return (t90 - t10) / RATE * 1e3


def bh4(n):
    x = 2 * np.pi * np.arange(n) / (n - 1)
    return 0.35875 - 0.48829 * np.cos(x) + 0.14128 * np.cos(2 * x) - 0.01168 * np.cos(3 * x)


def key_down_level(env):
    top = np.percentile(env, 99.5)
    return float(np.median(env[env > 0.5 * top]))


def spectra(b):
    """(freqs, peak-held dBc, average power, the carrier's peak-held power,
    the window's sum of squares)"""
    w = bh4(FRAME)
    peak = np.full(FRAME, 1e-30)
    avg = np.zeros(FRAME)
    n = 0
    for s in range(0, len(b) - FRAME, HOP):
        p = np.abs(np.fft.fftshift(np.fft.fft(b[s:s + FRAME] * w))) ** 2
        peak = np.maximum(peak, p)
        avg += p
        n += 1
    f = np.fft.fftshift(np.fft.fftfreq(FRAME, 1 / RATE))
    ref = peak[np.abs(f) <= RECENTRE_HZ].max()   # the carrier, not a stronger neighbour
    return f, 10 * np.log10(peak / ref + 1e-30), avg / max(n, 1), ref, float(np.sum(w * w))


def noise_estimate(f, held, avg):
    """The noise's median, peak-held (dBc) and average power, from NOISE_HZ on
    whichever side is quieter - so another station on one side is skipped."""
    best = None
    for side in (-1, 1):
        sel = (side * f >= NOISE_HZ[0]) & (side * f <= NOISE_HZ[1])
        m = (float(np.median(held[sel])), float(np.median(avg[sel])))
        if best is None or m[0] < best[0]:
            best = m
    return best


def neighbours(f, held, noise_peak):
    """Other stations: a peak beyond NEIGHBOUR_FROM_HZ that stands
    ASYMMETRY_DB above the strongest level at its mirror image, the same
    offset on the carrier's other side, and clear of the noise. Keying
    sidebands are an amplitude modulation, so they come in mirrored pairs,
    lobes and nulls alike; a peak on one side only is something else.
    Returns (excluded, peaks): a mask of the bins it covers - out to where
    it is within 3 dB of its mirror - and each one's (offset, dBc)."""
    n = len(f)
    mirror = np.full(n, -np.inf)
    for k in range(1, n):
        m = n - k                                      # the bin at -f[k]
        mirror[k] = held[max(m - 2, 1):min(m + 3, n)].max()
    far = np.abs(f) >= NEIGHBOUR_FROM_HZ
    lead = far & (held - mirror > 3)
    excluded = np.zeros(n, dtype=bool)
    peaks = []
    k = 0
    while k < n:
        if not lead[k]:
            k += 1
            continue
        end = k
        while end + 1 < n and lead[end + 1]:
            end += 1
        span = np.arange(k, end + 1)
        top = span[np.argmax(held[span])]
        if held[top] - mirror[top] >= ASYMMETRY_DB and held[top] > noise_peak + CLEAR_DB:
            excluded[span] = True
            peaks.append((float(f[top]), float(held[top])))
        k = end + 1
    return excluded, peaks


def beyond(f, v, hz, side, excluded=None):
    """The strongest level more than hz from the carrier on one side, within
    SEARCH_HZ and outside excluded, and where it is."""
    sel = (f > hz) & (f <= SEARCH_HZ) if side > 0 else (f < -hz) & (f >= -SEARCH_HZ)
    if excluded is not None:
        sel &= ~excluded
    i = np.flatnonzero(sel)[np.argmax(v[sel])]
    return float(v[i]), float(f[i])


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
    dicts of up50, down50, up10, up90, down90, down10 (fractional indices).
    A mark starts when env rises through MARK_ON of level and ends when it
    falls through MARK_OFF: without that hysteresis a fading signal or the
    noise hovering about 50% splits elements into fragments of a few ms."""
    on, off = MARK_ON * level, MARK_OFF * level
    pairs, start = [], None
    for i in np.flatnonzero((env > on) | (env < off)):
        if start is None and env[i] > on:
            start = i
        elif start is not None and env[i] < off:
            pairs.append((start, i))
            start = None
    marks = []
    for u, d in pairs:
        m = {"up50": cross(env, u, 0.5 * level, -1), "down50": cross(env, d, 0.5 * level, -1)}
        if m["up50"] is None or m["down50"] is None or (m["down50"] - m["up50"]) / RATE < 0.003:
            continue
        m["up10"] = cross(env, u, 0.1 * level, -1)
        m["up90"] = cross(env, u, 0.9 * level, 1)
        m["down90"] = cross(env, d, 0.9 * level, -1)
        m["down10"] = cross(env, d, 0.1 * level, 1)
        marks.append(m)
    return marks


def cluster(lens, centre, ratio):
    """The lengths within ratio of centre, either way."""
    return lens[np.abs(np.log(lens / centre)) < math.log(ratio)]


def speed(marks):
    """(WPM from the dots, dot:space), either None when there aren't enough
    marks. The dot is found by clustering the marks' lengths, not from the
    shortest: the most common length is the dots or the dashes, and if a
    group about a third as long is there too, that is the dots. A stray
    fragment of a few ms then can't be taken for one."""
    lens = np.array([(m["down50"] - m["up50"]) / RATE for m in marks])
    spaces = np.array([(b["up50"] - a["down50"]) / RATE for a, b in zip(marks, marks[1:])])
    if len(lens) < 5:
        return None, None
    counts = np.array([len(cluster(lens, v, 1.25)) for v in lens])
    mode = float(np.median(cluster(lens, lens[np.argmax(counts)], 1.25)))
    third = cluster(lens, mode / 3, 1.4)
    dot = float(np.median(third)) if len(third) >= max(3, 0.2 * counts.max()) else mode
    el_sp = spaces[(spaces > 0.5 * dot) & (spaces < 2 * dot)]
    return 1.2 / dot, (dot / float(np.median(el_sp)) if len(el_sp) else None)


def edges(marks):
    rises = [(m["up90"] - m["up10"]) / RATE * 1e3 for m in marks
             if m["up10"] is not None and m["up90"] is not None and m["up90"] < m["down50"]]
    falls = [(m["down10"] - m["down90"]) / RATE * 1e3 for m in marks
             if m["down10"] is not None and m["down90"] is not None and m["down90"] > m["up50"]]
    return rises, falls


def confirmed(marks, guide):
    """The marks whose middle falls inside one of guide's marks: the
    narrowband envelope's, which noise doesn't break up."""
    out = []
    for m in marks:
        c = 0.5 * (m["up50"] + m["down50"])
        if any(g["up50"] <= c <= g["down50"] for g in guide):
            out.append(m)
    return out


def steady_vs_edges(b, marks):
    """Where the sidebands come from. Short frames (SPLIT_FRAME samples)
    taken inside long elements, at least SPLIT_MARGIN_S from either edge;
    centred on each rising edge; and inside gaps, as the noise. Keying
    sidebands are only on the edges; phase noise or hum, the transmitter's
    or the receiver's, are there in steady key-down too. Returns
    {offset: (steady, edge, gap)} in dBc of the median frame's power, both
    sides averaged, and how many frames of each; None without steady
    frames."""
    n, margin = SPLIT_FRAME, int(SPLIT_MARGIN_S * RATE)
    w = bh4(n)
    f = np.fft.fftshift(np.fft.fftfreq(n, 1 / RATE))
    spec = lambda s: np.abs(np.fft.fftshift(np.fft.fft(b[s:s + n] * w))) ** 2
    steady, edge, gap = [], [], []
    for m in marks:
        u, d = int(m["up50"]), int(m["down50"])
        if d - u - 2 * margin >= n:
            steady.append(spec(u + margin))
        if u - n // 2 >= 0 and u + n // 2 <= len(b):
            edge.append(spec(u - n // 2))
    for a, z in zip(marks, marks[1:]):
        g0, g1 = int(a["down50"]) + margin, int(z["up50"]) - margin
        if g1 - g0 >= n:
            gap.append(spec(g0))
    if not steady or not edge or not gap:
        return None, (len(steady), len(edge), len(gap))
    # Medians over the frames: a burst of interference in one frame
    # mustn't decide it, and fading moves every class alike.
    st, ed, gp = (np.median(np.array(v), axis=0) for v in (steady, edge, gap))
    ref = st[np.abs(f) <= 30].max()
    out = {}
    for hz in SPLIT_OFFSETS_HZ:
        sel = np.abs(np.abs(f) - hz) <= 12
        out[hz] = tuple(10 * math.log10(v[sel].mean() / ref + 1e-30) for v in (st, ed, gp))
    return out, (len(steady), len(edge), len(gap))


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


def judge(worst, at_hz, limit, noise_peak):
    """PASS below the limit; "can't tell" when what is above it is within
    CLEAR_DB of the noise, since a sideband and the noise in the same bin
    add (a -62 dBc line read -57 with the noise's peaks at -59); otherwise
    FAIL - or, well away from the carrier and strong, more likely another
    station."""
    if worst <= limit:
        return "PASS"
    if worst <= noise_peak + CLEAR_DB:
        return "can't tell"
    if worst > NEIGHBOUR_DBC and abs(at_hz) > 338:
        return "another signal?"
    return "FAIL"


def report(x, f0, dial, label):
    peak_dbfs = 20 * math.log10(max(np.abs(x.real).max(), np.abs(x.imag).max(), 1e-12))
    b = baseband(x, f0)
    f, held, avg, ref, wsum2 = spectra(b)
    # Every offset is measured from the carrier, so centre on the peak the
    # mask is drawn around: within RECENTRE_HZ, where it can differ from the
    # first find when two signals are close or the signal moved.
    near = np.flatnonzero(np.abs(f) <= RECENTRE_HZ)
    shift = float(f[near[np.argmax(held[near])]])
    moved = None
    if abs(shift) > f[1] - f[0]:
        moved = (f"  re-centred {shift:+.0f} Hz, on the strongest peak - another signal that "
                 f"close, or a drifting one, can move it")
        f0 += shift
        b = baseband(x, f0)
        f, held, avg, ref, wsum2 = spectra(b)
    narrow = np.abs(lowpass(b, MARK_HZ))
    level = key_down_level(narrow)
    held_med, avg_med = noise_estimate(f, held, avg)
    noise_peak = held_med + NOISE_PEAK_DB
    psd = avg_med / (wsum2 * RATE)                     # noise power per Hz
    snr = lambda width_hz: 10 * math.log10(level ** 2 / (psd * width_hz))
    where = f"{(dial + f0) / 1e6:.6f} MHz" if dial else f"{f0:+.1f} Hz from the dial"
    print(f"\n{label}: signal at {where}")
    print(f"  stream peak {peak_dbfs:.1f} dBFS" +
          ("  - above -6 dBFS: the receiver may be overloaded" if peak_dbfs > -6 else ""))
    if moved:
        print(moved)
    print(f"  carrier {snr(2500):.1f} dB above the noise in 2500 Hz; the noise, held at its "
          f"peak, reaches about {noise_peak:.1f} dBc, so nothing below that can be seen")

    excluded, others_found = neighbours(f, held, noise_peak)
    for at, v in others_found:
        if abs(at) <= SEARCH_HZ:
            print(f"  another signal at {at:+.0f} Hz ({v:.1f} dBc), with nothing like it the "
                  f"other side of the carrier: left out of the mask (check the plot)")
    print(f"  keying sidebands, strongest beyond each offset, within {SEARCH_HZ} Hz:")
    print(f"  {'':22s}  {'below the carrier':>24s}   {'above the carrier':>24s}")
    for name, pts in (("CSI", CSI_MASK), ("DL1YCF", DL1YCF_EXTRA)):
        for hz, limit in pts:
            cols = []
            for side in (-1, 1):
                v, at = beyond(f, held, hz, side, excluded)
                cols.append(f"{v:6.1f} at {at:+6.0f} Hz {judge(v, at, limit, noise_peak):15s}")
            print(f"    {name:7s}{hz:5d} Hz {limit:5d} dBc   " + "  ".join(cols))

    guide = elements(narrow, level)
    wpm, weight = speed(guide)
    if wpm:
        w = f", dot:space {weight:.2f}" if weight else ""
        print(f"  speed {wpm:.1f} WPM from the dots{w} (from the envelope within "
              f"{MARK_HZ} Hz, {len(guide)} elements)")
    else:
        print(f"  speed: too few elements found ({len(guide)})")

    split, counts = steady_vs_edges(b, guide)
    if split is None:
        print(f"  steady key-down against the edges: not enough frames ({counts[0]} inside "
              f"elements, {counts[1]} on edges, {counts[2]} in gaps; elements need to be "
              f"{(SPLIT_FRAME / RATE + 2 * SPLIT_MARGIN_S) * 1000:.0f} ms long)")
    else:
        print(f"  steady key-down against the edges, median frame in 23 Hz bins ({counts[0]} "
              f"frames inside elements, {counts[1]} on edges, {counts[2]} in gaps):")
        for hz, (st, ed, gp) in split.items():
            if st <= gp + SPLIT_CLEAR_DB:
                verdict = ("keying: only on the edges" if ed > gp + SPLIT_CLEAR_DB
                           else "nothing above the noise")
            elif ed >= st + SPLIT_KEYING_DB:
                verdict = f"mostly keying: {ed - st:.0f} dB stronger on the edges"
            else:
                verdict = "there in steady key-down too: phase noise or hum, either end"
            print(f"    {hz:4d} Hz  steady {st:6.1f}  edges {ed:6.1f}  gaps {gp:6.1f} dBc   {verdict}")

    # The widest band the noise allows, and that leaves out any other
    # station: its filter reaches to 1.25 times the width.
    others = np.abs(f[(np.abs(f) > 338) & (np.abs(f) <= KEEP_HZ) & (held > NEIGHBOUR_DBC)])
    others = np.concatenate([others, [abs(at) for at, _ in others_found]])
    nearest = float(others.min()) if len(others) else None
    if nearest is not None:
        print(f"  another signal {nearest:.0f} Hz away: the timing below keeps clear of it")
    width = next((w for w in TIMING_HZ if snr(2 * w) >= TIMING_SNR_DB
                  and (nearest is None or 1.25 * w < nearest)), None)
    if width is None:
        print(f"  rise, fall and chirp: too weak or too crowded to measure - "
              f"{snr(2 * TIMING_HZ[-1]):.1f} dB above the noise within {TIMING_HZ[-1]} Hz, "
              f"and they need {TIMING_SNR_DB}")
    else:
        bw = lowpass(b, width)
        env = np.abs(bw)
        marks = confirmed(elements(env, key_down_level(env)), guide)
        rises, falls = edges(marks)
        print(f"  rise and fall measured within {width} Hz ({snr(2 * width):.1f} dB above the "
              f"noise), which itself rises in {filter_rise_ms(width):.2f} ms:")
        print(f"    rise 10-90%: " + (pct(rises) if rises else "none measured"))
        print(f"    fall 90-10%: " + (pct(falls) if falls else "none measured"))
        c, n = chirp(bw, marks)
        if c is not None:
            print(f"    chirp: {c:+.1f} Hz in the first {CHIRP_EARLY_MS:g} ms against the rest "
                  f"(median of {n} elements)")
    return f, held, 10 * np.log10(avg / ref + 1e-30), noise_peak


def write_csv(path, f, held, avg):
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["offset_hz", "peak_held_dbc", "average_dbc"])
        for i in range(len(f)):
            w.writerow([f"{f[i]:.2f}", f"{held[i]:.2f}", f"{avg[i]:.2f}"])
    print(f"  spectrum written to {path}")


def plot(path, f, held, noise_peak, title):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("  --plot needs matplotlib (sudo apt install python3-matplotlib)")
        return
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(f, held, lw=0.9, label="signal, peak held")
    ax.axhline(noise_peak, color="0.5", lw=0.9, ls=":", label="noise reaches here")
    for pts, style, name in ((CSI_MASK, "-", "CSI mask"), (DL1YCF_EXTRA, "--", "DL1YCF")):
        for side in (-1, 1):
            xs, ys = [], []
            for i, (hz, lim) in enumerate(pts):
                end = pts[i + 1][0] if i + 1 < len(pts) else SEARCH_HZ
                xs += [side * hz, side * end]
                ys += [lim, lim]
            ax.plot(xs, ys, style, color="C3", lw=1.2, label=name if side > 0 else None)
    ax.set_xlim(-SEARCH_HZ, SEARCH_HZ)
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
    ap.add_argument("--neighbour", type=float,
                    help="--simulate: add a steady carrier this far from the signal, Hz")
    ap.add_argument("--neighbour-dbc", type=float, default=-6.0,
                    help="--simulate: the added carrier's level against the signal, dB")
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
        x = simulate(a.simulate, a.wpm, a.snr, a.seconds, a.neighbour, a.neighbour_dbc)
        label = f"simulated {a.simulate}, {a.wpm:g} WPM, {a.snr:g} dB SNR"
    if a.save:
        np.save(a.save, x.astype(np.complex64))
        print(f"  recording saved to {a.save}")
    # A simulated signal is where simulate() put it, 500 Hz above the dial.
    offset = a.offset if a.offset is not None or not a.simulate else 500.0
    f0 = locate(x, offset, a.search)
    f, held, avg, noise_peak = report(x, f0, dial, label)
    if a.csv:
        write_csv(a.csv, f, held, avg)
    if a.plot:
        plot(a.plot, f, held, noise_peak, label)


if __name__ == "__main__":
    main()
