#!/usr/bin/env python3
"""
xtal_sweep.py - measures a radio's crystal filter from a laptop, by
stepping maxibitx's xtal_filter_center with the dial held still and
reading the level at the dial from the I/Q stream at each step.

    xtal_sweep.py --host zbitx.local [options]

Why this traces the filter and nothing else: xtal_filter_center is the
crystal frequency the dial is mixed onto (clk2 = dial + center), and the
second mixer always moves that frequency to the same 24 kHz IF (clk1 =
center + 24 kHz). So as the setting steps, the signal at the dial moves
across the crystal filter while everything after it - the codec, its
digital filter, the DSP - sees the same IF every time. The level at the
dial, plotted against the setting, is the filter's response, with the
setting on the x axis as the crystal frequency itself.

The setting that puts the dial in the middle of the filter is the value
for hw_settings.ini's xtal_filter_center. The sweep changes it only until
the end of the run (it is restored, also on Ctrl-C) and never writes the
ini.

    --start HZ / --stop HZ / --step HZ
                    settings to sweep (default 40,000,000 to 40,045,000 in
                    500 Hz steps, about two and a half minutes)
    --dwell S       seconds of I/Q averaged per step (default 1.0)
    --settle S      seconds of I/Q discarded after each change (default
                    0.5), longer than the capture and stream queues
    --bw HZ         width of the measurement window (default 300). On
                    noise, the measured widths come out wider than the
                    filter's by about this much, so keep it well under
                    the filter's width; a carrier is unaffected.
    --offset HZ     window center relative to the dial (default 0);
                    positive is above the dial, as on a spectrum display
    --dial HZ       tune here for the sweep, and back afterwards
    --csv FILE      results file (default xtal_sweep_<time>.csv)

A sweep needs something at the dial to measure. Band noise on an antenna
works if it is well above the receiver's own noise; a steady carrier at
the dial (a signal generator, or WWV/a broadcast carrier inside --bw) gives
the cleanest curve, and a dummy load gives only the receiver's noise,
which mostly enters after the filter and so draws it flattened.

Needs a maxibitx with rigctld's XTALCENTER level (docs/06_api.md), the
I/Q stream on UDP 4536, and numpy. Transmit is refused by maxibitx while
a setting leaves no usable TX IF, and the sweep refuses to start while
transmitting.

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


def band_power(stream, seconds, lo_hz, hi_hz):
    """Mean power in [lo_hz, hi_hz] relative to the dial, over `seconds`
    of I/Q (counted in samples, not wall time): Hann-windowed FFT blocks
    averaged, each built only from unbroken runs of packets. Returns
    (power, blocks)."""
    window = np.hanning(NFFT)
    norm = np.sum(window ** 2) * NFFT
    freqs = np.fft.fftfreq(NFFT, d=1.0 / IQ_RATE_HZ)
    in_band = (freqs >= lo_hz) & (freqs <= hi_hz)
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
    # Each bin of spec is that bin's share of the mean-square level, so
    # their sum over the window is the power in it.
    return float(np.sum(acc[in_band]) / blocks), blocks


def edges(settings, rel_db, peak_i, level):
    """Interpolated settings where the response first falls `level` dB
    below the peak on each side; None for a side that stays above it
    within the sweep."""
    def cross(i_in, i_out):
        a, b = rel_db[i_in], rel_db[i_out]
        t = (-level - a) / (b - a)
        return settings[i_in] + t * (settings[i_out] - settings[i_in])

    left = right = None
    for i in range(peak_i, 0, -1):
        if rel_db[i - 1] < -level:
            left = cross(i, i - 1)
            break
    for i in range(peak_i, len(settings) - 1):
        if rel_db[i + 1] < -level:
            right = cross(i, i + 1)
            break
    return left, right


def bar(rel, width=40, floor_db=40.0):
    filled = int(round(width * max(0.0, 1.0 + rel / floor_db)))
    return "#" * filled


def main():
    ap = argparse.ArgumentParser(
        description="Measure the crystal filter by sweeping xtal_filter_center "
                    "with the dial fixed (docs/dsp_design_notes/zbitx_port_study.md §11).")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=RIGCTL_PORT, help="rigctld port")
    ap.add_argument("--iq-port", type=int, default=IQ_STREAM_PORT)
    ap.add_argument("--start", type=int, default=40000000, help="first setting, Hz")
    ap.add_argument("--stop", type=int, default=40045000, help="last setting, Hz")
    ap.add_argument("--step", type=int, default=500, help="Hz between settings")
    ap.add_argument("--dwell", type=float, default=1.0, help="seconds of I/Q measured per step")
    ap.add_argument("--settle", type=float, default=0.5,
                    help="seconds of I/Q discarded after each change")
    ap.add_argument("--bw", type=float, default=300.0, help="measurement window width, Hz")
    ap.add_argument("--offset", type=float, default=0.0,
                    help="window center relative to the dial, Hz (+ is above)")
    ap.add_argument("--dial", type=int, help="tune here for the sweep, and back afterwards")
    ap.add_argument("--csv", help="results file (default xtal_sweep_<time>.csv)")
    args = ap.parse_args()

    if args.step <= 0 or args.stop < args.start:
        sys.exit("need --start <= --stop and a positive --step")
    settings = list(range(args.start, args.stop + 1, args.step))
    lo_hz = args.offset - args.bw / 2
    hi_hz = args.offset + args.bw / 2
    csv_path = args.csv or time.strftime("xtal_sweep_%Y%m%d_%H%M%S.csv")

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
    probe, _ = band_power(stream, 1.0, lo_hz, hi_hz)
    if probe is None:
        sys.exit(f"no I/Q arriving on UDP {args.iq_port} from {args.host}")

    print(f"dial {dial} Hz, xtal_filter_center now {original} Hz")
    print(f"sweeping {settings[0]}..{settings[-1]} Hz in {args.step} Hz steps, "
          f"window {lo_hz:+.0f}..{hi_hz:+.0f} Hz around the dial, "
          f"{args.dwell:g} s per step (about {len(settings) * (args.dwell + args.settle) + 1:.0f} s)")
    print()

    results = []   # (setting, power or None, blocks)
    try:
        for s in settings:
            if not rig.set_ok(f"L XTALCENTER {s}"):
                print(f"{s:>10}  refused - transmitting, or outside maxibitx's bounds")
                results.append((s, None, 0))
                continue
            stream.flush()
            for _ in stream.packets(int(args.settle * IQ_RATE_HZ)):
                pass
            p, blocks = band_power(stream, args.dwell, lo_hz, hi_hz)
            results.append((s, p, blocks))
            level = f"{10 * math.log10(p):7.1f} dBFS" if p else "  no data"
            print(f"{s:>10}  {level}", flush=True)
    except KeyboardInterrupt:
        print("\ninterrupted - restoring and reporting what was measured")
    finally:
        if not rig.set_ok(f"L XTALCENTER {original}"):
            print(f"WARNING: could not restore xtal_filter_center to {original} Hz "
                  f"- set it with 'L XTALCENTER {original}', or restart maxibitx")
        if args.dial is not None and original_dial is not None:
            rig.set_ok(f"F {original_dial}")
        rig.close()

    good = [(s, p, b) for s, p, b in results if p]
    if not good:
        sys.exit("nothing measured")
    xs = [s for s, _, _ in good]
    db = [10 * math.log10(p) for _, p, _ in good]
    peak_i = max(range(len(db)), key=lambda i: db[i])
    rel = [d - db[peak_i] for d in db]

    print()
    print(f"{'setting Hz':>10}  {'dBFS':>7}  {'rel dB':>7}")
    for s, d, r in zip(xs, db, rel):
        print(f"{s:>10}  {d:7.1f}  {r:7.1f}  {bar(r)}")
    print()
    print(f"peak     {xs[peak_i]} Hz ({db[peak_i]:.1f} dBFS)")
    report = {}
    for level in LEVELS_DB:
        left, right = edges(xs, rel, peak_i, level)
        report[level] = (left, right)
        if left is None or right is None:
            missing = " and ".join(n for n, e in (("low", left), ("high", right)) if e is None)
            print(f"-{level:<2} dB   {missing} edge beyond the sweep")
        else:
            print(f"-{level:<2} dB   {left:.0f} .. {right:.0f} Hz, width {right - left:.0f} Hz, "
                  f"center {(left + right) / 2:.0f} Hz")
    left6, right6 = report[6]
    if left6 is not None and right6 is not None:
        print()
        print(f"xtal_filter_center = {round((left6 + right6) / 2 / 100) * 100}"
              f"   (the -6 dB center, to 100 Hz)")
    dropped = stream.dropped
    if dropped:
        print(f"\n{dropped} I/Q packets lost in transit (those blocks were skipped, "
              "so levels are unaffected)")
    thin = [s for s, _, b in good if b < 0.5 * args.dwell * IQ_RATE_HZ / NFFT]
    if thin:
        print(f"{len(thin)} step(s) got under half the expected I/Q - "
              "a slow network or host; consider a longer --dwell")

    with open(csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["xtal_filter_center_hz", "level_dbfs", "rel_db", "fft_blocks"])
        for s, p, b in results:
            if p:
                d = 10 * math.log10(p)
                w.writerow([s, f"{d:.2f}", f"{d - db[peak_i]:.2f}", b])
            else:
                w.writerow([s, "", "", b])
    print(f"\nwrote {csv_path}")


if __name__ == "__main__":
    main()
