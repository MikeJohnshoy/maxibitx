#!/usr/bin/env python3
"""
xtal_sweep.py - measures a radio's crystal filter from a laptop, by
stepping maxibitx's xtal_filter_center with the dial held still and
recording the whole I/Q spectrum at each step.

    xtal_sweep.py --host zbitx.local [options]
    xtal_sweep.py --refit xtal_sweep_<time>_raw.csv

What a step measures. xtal_filter_center (the setting, S) is the crystal
frequency the dial is mixed onto: clk2 = dial + S, and the mixer's
spectrum flip puts a signal at dial + d at crystal frequency S - d. The
second mixer (clk1 = S + 24 kHz) then brings every crystal frequency to
the IF 24 kHz + d, whatever S is. So each offset d in the spectrum sees
the crystal filter at S - d, multiplied by a response that depends on d
alone: the codec, its digital filter and the anti-alias filter, plus any
steady signal on the band at that offset. In dB:

    level(S, d) = X(S - d) + C(d)

X is the crystal filter (with the noise added after it, which is the
same at every crystal frequency); C is everything after it. One sweep
gives a grid of levels over S and d, and a robust fit (alternating
medians) separates the two. Each crystal frequency is seen at up to
(2 * span / step + 1) offsets, so X comes out far smoother than the
level at the dial alone, and it covers the swept range widened by about
half the span on each side (a crystal frequency is reported once it is
seen at a quarter of the offsets) - edges outside the sweep are measured
too. Signals
that come and go (FT8, QSB) are outliers the medians ignore; steady ones
stay at one offset and end up in C.

The setting that puts the dial in the middle of the filter is the value
for hw_settings.ini's xtal_filter_center. The sweep changes it only until
the end of the run (it is restored, also on Ctrl-C) and never writes the
ini.

    --start HZ / --stop HZ / --step HZ
                    settings to sweep (default 39,975,000 to 40,050,000 in
                    1,000 Hz steps, about two minutes). The step is also
                    the resolution of the result.
    --dwell S       seconds of I/Q averaged per step (default 1.0)
    --settle S      seconds of I/Q discarded after each change (default
                    0.5), longer than the capture and stream queues
    --span HZ       largest offset from the dial used in the fit (default
                    22,000). Offsets past +-24 kHz are the codec's DC and
                    Nyquist regions, where the model does not hold.
    --bw HZ / --offset HZ
                    a window at the dial (default 1,000 Hz wide, centred
                    on it) measured the simple way as well, for comparison
                    and for a carrier: a steady carrier is part of C to
                    the fit, but this window follows it through the filter
    --dial HZ       tune here for the sweep, and back afterwards
    --out PREFIX    output name (default xtal_sweep_<time>)
    --refit FILE    no radio: fit and report a sweep's _raw.csv again

Outputs: PREFIX.csv (the filter against crystal frequency, with the
dial window's levels beside it), PREFIX_if.csv (C against offset),
PREFIX_raw.csv (every step's spectrum, for --refit), and PREFIX.png if
matplotlib is installed.

What to measure. The filter can only be drawn as far down as the noise
in front of it is above the noise added after it, so the depth of the
result is set by the stimulus. Band noise on an antenna gives several
dB to a few tens of dB, depending on the band and the time of day. A
broadband noise source at the antenna input gives the most. A dummy load
gives only the receiver's own noise, most of which enters after the
filter, so it shows the filter barely at all.

Needs a maxibitx with rigctld's XTALCENTER level (docs/06_api.md), the
I/Q stream on UDP 4536, and numpy; matplotlib only for the plot.
Transmit is refused by maxibitx while a setting leaves no usable TX IF,
and the sweep refuses to start while transmitting.

docs/dsp_design_notes/zbitx_port_study.md §11.
"""

import argparse
import csv
import math
import socket
import struct
import sys
import time

try:
    import numpy as np
except ImportError:
    sys.exit("xtal_sweep.py needs numpy: pip install numpy "
             "(or: sudo apt install python3-numpy)")

RIGCTL_PORT = 4532       # hamlib.c
IQ_STREAM_PORT = 4536    # src/interfaces/iq_stream.h
IQ_STREAM_MAGIC = b"IQS1"
IQ_RATE_HZ = 96000
KEEPALIVE_S = 1.0        # iq_stream.c drops a subscriber after 5 s of silence
NFFT = 2048              # 46.9 Hz bins; one block is 16 packets of 128
LEVELS_DB = (3, 6, 20)   # edges reported, below the peak
MIN_SEEN = 0.25          # a crystal frequency is reported only if seen at
                         # this fraction of the offsets: the extremes of the
                         # range are seen only at the largest offsets, where
                         # the codec's response is steepest
EDGE_HOLD_HZ = 3000      # an edge is where the response stays below the
                         # level for this long, so passband ripple isn't one


class Rigctl:
    """One rigctld connection. hamlib.c answers every command with exactly
    one line, either the value or RPRT n."""

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)

    def query(self, command):
        self.sock.sendall((command + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = self.sock.recv(256)
            if not chunk:
                raise OSError("rigctld closed the connection")
            buf += chunk
        return buf.decode(errors="replace").strip()

    def get_int(self, command):
        reply = self.query(command)
        try:
            return int(float(reply))
        except ValueError:
            return None

    def set_ok(self, command):
        return self.query(command) == "RPRT 0"

    def close(self):
        self.sock.close()


class IqStream:
    """A subscription to iq_stream.c. Single-threaded: the caller pulls
    packets, and the keepalive is sent from inside that loop."""

    def __init__(self, host, port):
        self.dest = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # A second of the stream is 384 kB; leave room for a slow step.
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        except OSError:
            pass
        self.sock.settimeout(0.2)
        self.last_keepalive = 0.0
        self.last_seq = None
        self.broken = True
        self.dropped = 0

    def _keepalive(self):
        now = time.monotonic()
        if now - self.last_keepalive >= KEEPALIVE_S:
            self.sock.sendto(b"S", self.dest)
            self.last_keepalive = now

    def _header(self, data):
        """(seq, n) for a well-formed packet, else None. Counts packets
        lost since the previous one in self.dropped."""
        if len(data) < 12 or data[:4] != IQ_STREAM_MAGIC:
            return None
        seq, n = struct.unpack(">II", data[4:12])
        if len(data) != 12 + 4 * n:
            return None
        if self.last_seq is not None:
            self.dropped += (seq - self.last_seq - 1) & 0xFFFFFFFF
        return seq, n

    def flush(self):
        """Discards everything already queued in the socket: I/Q from
        before a change the caller has just made. Without this, a host
        that falls behind the stream would measure the previous step."""
        self.sock.setblocking(False)
        try:
            while True:
                head = self._header(self.sock.recv(2048))
                if head:
                    self.last_seq = head[0]
        except (BlockingIOError, InterruptedError):
            pass
        finally:
            self.sock.settimeout(0.2)
        self.broken = True   # what follows doesn't continue a block

    def packets(self, n_samples):
        """Yields (contiguous, samples) until n_samples have arrived, or
        stops early if the stream goes quiet for 2 s. samples are complex,
        full scale 1.0, with the stream's spectral inversion undone so +f
        is above the dial. contiguous is False when packets were lost, or
        flushed, just before this one."""
        got = 0
        quiet_since = time.monotonic()
        while got < n_samples:
            self._keepalive()
            try:
                data = self.sock.recv(2048)
            except socket.timeout:
                if time.monotonic() - quiet_since > 2.0:
                    return
                continue
            head = self._header(data)
            if not head:
                continue
            seq, n = head
            contiguous = (not self.broken and self.last_seq is not None
                          and seq == (self.last_seq + 1) & 0xFFFFFFFF)
            self.last_seq = seq
            self.broken = False
            quiet_since = time.monotonic()
            got += n
            iq = np.frombuffer(data, dtype=">i2", count=2 * n, offset=12)
            iq = iq.astype(np.float64).reshape(-1, 2)
            # The raw I/Q has a station at dial+d at -d (sound.c's mixer);
            # conjugating puts it at +d, the way rigctl_panel.py shows it.
            yield contiguous, (iq[:, 0] - 1j * iq[:, 1]) / 32767.0


FREQS = np.fft.fftfreq(NFFT, d=1.0 / IQ_RATE_HZ)


def step_spectrum(stream, seconds):
    """The power spectrum over `seconds` of I/Q (counted in samples, not
    wall time): Hann-windowed FFT blocks averaged, each built only from an
    unbroken run of packets. Each bin is its share of the mean-square
    level, so bins add to band power. Returns (spectrum, blocks), with
    spectrum None if no block was completed; bins are in FREQS order."""
    window = np.hanning(NFFT)
    norm = np.sum(window ** 2) * NFFT
    acc = None
    blocks = 0
    pending = []
    pending_len = 0
    for contiguous, samples in stream.packets(int(seconds * IQ_RATE_HZ)):
        if not contiguous:
            pending, pending_len = [], 0
        pending.append(samples)
        pending_len += len(samples)
        if pending_len >= NFFT:
            run = np.concatenate(pending)
            block, rest = run[:NFFT], run[NFFT:]
            pending, pending_len = [rest], len(rest)
            spec = np.abs(np.fft.fft(block * window)) ** 2 / norm
            acc = spec if acc is None else acc + spec
            blocks += 1
    if not blocks:
        return None, 0
    return acc / blocks, blocks


def window_power(spec, lo_hz, hi_hz):
    """Power in [lo_hz, hi_hz] relative to the dial."""
    return float(np.sum(spec[(FREQS >= lo_hz) & (FREQS <= hi_hz)]))


def offset_cells(spec, step, k_max):
    """The spectrum reduced to one level per offset d = k * step, k from
    -k_max to k_max: the median bin within +-step/2 of d, in dB. The
    median keeps a narrow signal in the cell from setting its level."""
    out = np.empty(2 * k_max + 1)
    for i, k in enumerate(range(-k_max, k_max + 1)):
        d = k * step
        cell = spec[np.abs(FREQS - d) < step / 2]
        out[i] = 10 * math.log10(max(float(np.median(cell)), 1e-30))
    return out


def fit(levels, k_max):
    """Separates levels[iS, iD] (dB, NaN where missing) into X + C, with
    X indexed by crystal frequency and C by offset, by alternating
    medians. With S = S0 + iS * step and d = (iD - k_max) * step, the
    crystal frequency S - d is S0 + (iX - k_max) * step, iX = iS - iD +
    2 * k_max. C is pinned to 0 at the dial, so X reads as the level a
    signal at the dial would have. Returns (X, n, C): n is how many
    offsets each crystal frequency was seen at."""
    n_s, n_d = levels.shape
    ix = np.arange(n_s)[:, None] - np.arange(n_d)[None, :] + 2 * k_max
    n_x = n_s + 2 * k_max
    valid = ~np.isnan(levels)
    flat_ix = ix[valid]
    flat_lv = levels[valid]
    order = np.argsort(flat_ix, kind="stable")
    flat_ix, flat_lv = flat_ix[order], flat_lv[order]
    bounds = np.searchsorted(flat_ix, np.arange(n_x + 1))
    n = np.diff(bounds)

    x = np.zeros(n_x)
    c = np.zeros(n_d)
    for _ in range(100):
        resid = np.where(valid, levels - x[ix], np.nan)
        with np.errstate(all="ignore"):
            c_new = np.array([np.nanmedian(col) if np.any(~np.isnan(col)) else 0.0
                              for col in resid.T])
        c_new -= c_new[k_max]
        flat_c = (levels - c_new[None, :])[valid][order]
        x_new = np.array([np.median(flat_c[bounds[i]:bounds[i + 1]]) if n[i] else np.nan
                          for i in range(n_x)])
        change = max(np.nanmax(np.abs(x_new - x)), np.max(np.abs(c_new - c)))
        x, c = x_new, c_new
        if change < 0.005:
            break
    return x, n, c


def find_edge(xs, rel, peak_i, level, direction, step):
    """The interpolated crystal frequency where the response, going from
    the peak in `direction` (+1 up, -1 down), falls below -level and stays
    there for EDGE_HOLD_HZ - so a ripple dip in the passband isn't taken
    for an edge. None if it doesn't within the data."""
    hold = max(1, int(math.ceil(EDGE_HOLD_HZ / step)))
    i = peak_i
    while 0 <= i + direction < len(xs):
        j = i + direction
        run = rel[j: j + direction * hold: direction] if direction > 0 else \
            rel[max(j - hold + 1, 0): j + 1][::-1]
        if rel[j] < -level and len(run) == hold and np.all(run < -level):
            t = (-level - rel[i]) / (rel[j] - rel[i])
            return xs[i] + t * (xs[j] - xs[i])
        i = j
    return None


def bar(rel, depth, width=40):
    if depth <= 0:
        return ""
    return "#" * int(round(width * min(1.0, max(0.0, 1.0 + rel / depth))))


def report(settings, step, k_max, levels, window_db, prefix):
    """Fits, prints the result and writes PREFIX.csv, PREFIX_if.csv and,
    if matplotlib is there, PREFIX.png."""
    x, n, c = fit(levels, k_max)
    s0 = settings[0]
    xs = np.array([s0 + (i - k_max) * step for i in range(len(x))])
    ok = (n >= max(5, MIN_SEEN * (2 * k_max + 1))) & ~np.isnan(x)
    if np.count_nonzero(ok) < 3:
        sys.exit("too little data to fit")
    xs_ok, x_ok, n_ok = xs[ok], x[ok], n[ok]
    peak_i = int(np.argmax(x_ok))
    rel = x_ok - x_ok[peak_i]
    floor = float(np.percentile(rel, 5))
    depth = -floor
    offsets = np.array([(k - k_max) for k in range(2 * k_max + 1)]) * step

    print()
    print("crystal filter, fitted from every offset (dB below the peak):")
    print(f"{'crystal Hz':>10}  {'rel dB':>7}  {'seen':>4}")
    for f, r, m in zip(xs_ok, rel, n_ok):
        print(f"{f:>10}  {r:7.1f}  {m:>4}  {bar(r, depth)}")
    print()
    print(f"peak     {xs_ok[peak_i]} Hz")
    print(f"depth    {depth:.1f} dB from the peak to the floor (5th percentile) - "
          "the deepest an edge can be measured")
    edges = {}
    for level in LEVELS_DB:
        lo = find_edge(xs_ok, rel, peak_i, level, -1, step)
        hi = find_edge(xs_ok, rel, peak_i, level, +1, step)
        edges[level] = (lo, hi)
        if level > depth - 1:
            print(f"-{level:<2} dB   below this measurement's floor")
        elif lo is None or hi is None:
            missing = " and ".join(nm for nm, e in (("low", lo), ("high", hi)) if e is None)
            print(f"-{level:<2} dB   {missing} edge beyond the data")
        else:
            print(f"-{level:<2} dB   {lo:.0f} .. {hi:.0f} Hz, width {hi - lo:.0f} Hz, "
                  f"center {(lo + hi) / 2:.0f} Hz")
    for level in (6, 3):
        lo, hi = edges[level]
        if lo is not None and hi is not None and level <= depth - 1:
            print()
            print(f"xtal_filter_center = {round((lo + hi) / 2 / 100) * 100}"
                  f"   (the -{level} dB center, to 100 Hz)")
            break

    print()
    print("after the filter (C: codec, digital filters and steady signals), "
          "dB relative to the dial:")
    every = max(1, int(round(2000 / step)))
    picks = list(range(0, len(offsets), every))
    for row in range(0, len(picks), 6):
        print("  " + "  ".join(f"{offsets[i] / 1000:+6.1f}k {c[i]:+6.1f}"
                               for i in picks[row:row + 6]))

    with open(prefix + ".csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["crystal_hz", "rel_db", "offsets_seen", "dial_window_rel_db"])
        win = dict(window_db)
        win_peak = max((v for v in win.values() if v is not None), default=None)
        for fx, r, m in zip(xs_ok, rel, n_ok):
            wv = win.get(int(fx))
            w.writerow([int(fx), f"{r:.2f}", m,
                        "" if wv is None or win_peak is None else f"{wv - win_peak:.2f}"])
    with open(prefix + "_if.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["offset_hz", "rel_db"])
        for d, v in zip(offsets, c):
            w.writerow([int(d), f"{v:.2f}"])
    written = [prefix + ".csv", prefix + "_if.csv"]
    if plot(prefix + ".png", xs_ok, rel, window_db, offsets, c, edges, depth):
        written.append(prefix + ".png")
    return written


def plot(path, xs, rel, window_db, offsets, c, edges, depth):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    ink, muted, grid, ground = "#0b0b0b", "#52514e", "#e6e5e0", "#fcfcfb"
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(10, 7.5), dpi=120,
                                 gridspec_kw={"height_ratios": [3, 2]})
    fig.patch.set_facecolor(ground)
    a1.plot(xs / 1e3, rel, color="#2a78d6", lw=2, label="fitted from every offset")
    pts = [(s, v) for s, v in window_db if v is not None]
    if pts:
        wp = max(v for _, v in pts)
        a1.plot([s / 1e3 for s, _ in pts], [v - wp for _, v in pts], color="#eb6834",
                lw=0, marker="o", ms=3, label="window at the dial")
    for level in LEVELS_DB:
        lo, hi = edges[level]
        if level <= depth - 1 and lo is not None and hi is not None:
            a1.hlines(-level, lo / 1e3, hi / 1e3, color=muted, lw=1, linestyles=":")
            a1.text(hi / 1e3, -level, f"  -{level} dB: {(hi - lo) / 1e3:.1f} kHz wide, "
                    f"center {(lo + hi) / 2 / 1e3:.2f}", va="center", fontsize=8, color=muted)
    a1.set_xlabel("crystal frequency (kHz)", color=muted)
    a1.set_ylabel("dB below the peak", color=muted)
    a1.set_title("Crystal filter", loc="left", color=ink)
    a1.legend(frameon=False, fontsize=9, loc="lower center")
    a2.plot(offsets / 1e3, c, color="#1baf7a", lw=2)
    a2.set_xlabel("offset from the dial (kHz)", color=muted)
    a2.set_ylabel("dB relative to the dial", color=muted)
    a2.set_title("After the filter: codec, digital filters and steady signals",
                 loc="left", color=ink)
    for a in (a1, a2):
        a.set_facecolor(ground)
        a.grid(color=grid, lw=0.8)
        a.tick_params(colors=muted)
        for side in ("top", "right"):
            a.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            a.spines[side].set_color("#b9b8b2")
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
    return True


def write_raw(path, settings, step, k_max, levels, window_db, blocks, lost):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["xtal_filter_center_hz", "dial_window_dbfs", "fft_blocks", "packets_lost"]
                   + [f"d{(k - k_max) * step:+d}" for k in range(2 * k_max + 1)])
        for i, s in enumerate(settings):
            wv = window_db[i][1]
            w.writerow([s, "" if wv is None else f"{wv:.2f}", blocks[i], lost[i]]
                       + ["" if np.isnan(v) else f"{v:.2f}" for v in levels[i]])


def read_raw(path):
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    head, body = rows[0], rows[1:]
    offsets = [int(h[1:]) for h in head[4:]]
    settings = [int(r[0]) for r in body]
    step = settings[1] - settings[0]
    if any(b - a != step for a, b in zip(settings, settings[1:])) or \
            any(o % step for o in offsets):
        sys.exit(f"{path}: settings and offsets must be on one even grid")
    k_max = -offsets[0] // step
    levels = np.array([[float(v) if v else np.nan for v in r[4:]] for r in body])
    window_db = [(s, float(r[1]) if r[1] else None) for s, r in zip(settings, body)]
    return settings, step, k_max, levels, window_db


def main():
    ap = argparse.ArgumentParser(
        description="Measure the crystal filter by sweeping xtal_filter_center "
                    "with the dial fixed (docs/dsp_design_notes/zbitx_port_study.md §11).")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=RIGCTL_PORT, help="rigctld port")
    ap.add_argument("--iq-port", type=int, default=IQ_STREAM_PORT)
    ap.add_argument("--start", type=int, default=39975000, help="first setting, Hz")
    ap.add_argument("--stop", type=int, default=40050000, help="last setting, Hz")
    ap.add_argument("--step", type=int, default=1000,
                    help="Hz between settings, and the result's resolution")
    ap.add_argument("--dwell", type=float, default=1.0, help="seconds of I/Q measured per step")
    ap.add_argument("--settle", type=float, default=0.5,
                    help="seconds of I/Q discarded after each change")
    ap.add_argument("--span", type=int, default=22000,
                    help="largest offset from the dial used in the fit, Hz")
    ap.add_argument("--bw", type=float, default=1000.0, help="dial window width, Hz")
    ap.add_argument("--offset", type=float, default=0.0,
                    help="dial window center relative to the dial, Hz (+ is above)")
    ap.add_argument("--dial", type=int, help="tune here for the sweep, and back afterwards")
    ap.add_argument("--out", help="output name prefix (default xtal_sweep_<time>)")
    ap.add_argument("--refit", metavar="RAW_CSV",
                    help="fit and report a sweep's _raw.csv again, without the radio")
    args = ap.parse_args()

    if args.refit:
        settings, step, k_max, levels, window_db = read_raw(args.refit)
        prefix = args.out or (args.refit[:-8] if args.refit.endswith("_raw.csv")
                              else args.refit) + "_refit"
        for path in report(settings, step, k_max, levels, window_db, prefix):
            print(f"wrote {path}")
        return

    if args.step <= 0 or args.stop < args.start:
        sys.exit("need --start <= --stop and a positive --step")
    if not 0 < args.span < 24000:
        sys.exit("--span must be under 24000: past that is the codec's DC and Nyquist")
    k_max = args.span // args.step
    if k_max < 1:
        sys.exit("--span must be at least one --step")
    settings = list(range(args.start, args.stop + 1, args.step))
    lo_hz = args.offset - args.bw / 2
    hi_hz = args.offset + args.bw / 2
    prefix = args.out or time.strftime("xtal_sweep_%Y%m%d_%H%M%S")

    rig = Rigctl(args.host, args.port)
    original = rig.get_int("l XTALCENTER")
    if original is None:
        sys.exit("this maxibitx has no XTALCENTER level - it needs a build "
                 "with rigctld's L XTALCENTER (docs/06_api.md)")
    if rig.get_int("t") == 1:
        sys.exit("the radio is transmitting - try again in receive")
    original_dial = rig.get_int("f")
    if args.dial is not None and not rig.set_ok(f"F {args.dial}"):
        sys.exit(f"could not tune to {args.dial} Hz")
    dial = rig.get_int("f")

    stream = IqStream(args.host, args.iq_port)
    probe, _ = step_spectrum(stream, 1.0)
    if probe is None:
        sys.exit(f"no I/Q arriving on UDP {args.iq_port} from {args.host}")

    print(f"dial {dial} Hz, xtal_filter_center now {original} Hz")
    print(f"sweeping {settings[0]}..{settings[-1]} Hz in {args.step} Hz steps, "
          f"fitting offsets to +-{k_max * args.step} Hz "
          f"(crystal {settings[0] - k_max * args.step}..{settings[-1] + k_max * args.step} Hz), "
          f"{args.dwell:g} s per step (about {len(settings) * (args.dwell + args.settle) + 1:.0f} s)")
    print()
    print(f"{'setting Hz':>10}  {'dial window':>12}  blocks  lost")

    levels = np.full((len(settings), 2 * k_max + 1), np.nan)
    window_db = [(s, None) for s in settings]
    blocks = [0] * len(settings)
    lost = [0] * len(settings)
    try:
        for i, s in enumerate(settings):
            if not rig.set_ok(f"L XTALCENTER {s}"):
                print(f"{s:>10}  refused - transmitting, or outside maxibitx's bounds")
                continue
            stream.flush()
            for _ in stream.packets(int(args.settle * IQ_RATE_HZ)):
                pass
            before = stream.dropped
            spec, blocks[i] = step_spectrum(stream, args.dwell)
            lost[i] = stream.dropped - before
            if spec is None:
                print(f"{s:>10}  no data")
                continue
            p = window_power(spec, lo_hz, hi_hz)
            window_db[i] = (s, 10 * math.log10(p) if p > 0 else None)
            levels[i] = offset_cells(spec, args.step, k_max)
            wtxt = f"{window_db[i][1]:7.1f} dBFS" if window_db[i][1] is not None else "     --"
            print(f"{s:>10}  {wtxt:>12}  {blocks[i]:>6}  {lost[i]:>4}", flush=True)
    except KeyboardInterrupt:
        print("\ninterrupted - restoring and reporting what was measured")
    finally:
        if not rig.set_ok(f"L XTALCENTER {original}"):
            print(f"WARNING: could not restore xtal_filter_center to {original} Hz "
                  f"- set it with 'L XTALCENTER {original}', or restart maxibitx")
        if args.dial is not None and original_dial is not None:
            rig.set_ok(f"F {original_dial}")
        rig.close()

    write_raw(prefix + "_raw.csv", settings, args.step, k_max, levels, window_db, blocks, lost)
    written = [prefix + "_raw.csv"]
    thin = sum(1 for b in blocks if 0 < b < 0.5 * args.dwell * IQ_RATE_HZ / NFFT)
    if thin:
        print(f"\n{thin} step(s) got under half the expected I/Q - "
              "a slow network or host; consider a longer --dwell")
    written += report(settings, args.step, k_max, levels, window_db, prefix)
    print()
    for path in written:
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
