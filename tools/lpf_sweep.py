#!/usr/bin/env python3
"""
lpf_sweep.py - measures the sBitx's four low-pass filters through its own
receiver, from a laptop, by stepping the dial and, at each frequency,
holding each filter in circuit in turn (rigctld's L LPF) while recording
the received noise from the I/Q stream.

    lpf_sweep.py --host sbitx.local [options]
    lpf_sweep.py --replot lpf_sweep_<time>_raw.csv

What a frequency measures. On the sBitx the LPFs sit between the antenna
and the first mixer, so at a dial frequency f each of the five states -
LPF_A, B, C, D held, or every relay off - sees the same source (the
antenna's noise, or a noise generator), the same mixer, crystal filter
and codec, and the same receiver noise; only the filter differs. In
power:

    P_X(f) = |H_X(f)|^2 * S(f) + N(f)      X = A, B, C, D
    P_none(f) = L(f) + N(f)

S is the source as the receiver sees it, N the noise added after the
filters, L what leaks past open relays. P_none is the floor; what each
filter passes above it is P_X - P_none, and the ratio of two filters'
excess is the ratio of their responses, with S and the receiver gone:

    B vs A (dB) = 10 log10((P_B - P_none) / (P_A - P_none))

LPF_A (10-15 m) passes everything up to about 30 MHz flat, so below that
"X vs A" is X's own response. Above it the ratio is still exact, but it
is relative to A, which is rolling off too. A itself is reported as its
level above the floor, which includes the source's and the receiver's
own slope - with a flat noise generator that is close to A's response.

The five states are measured within a few seconds at each frequency, so
the band rising and falling between frequencies cancels in the ratios.
--passes 2 measures each state twice, the second time in reverse order,
so a steady drift within a frequency cancels too. The level is the
median over the bins 1 to 10 kHz either side of the dial (inside the
crystal filter's flat top, clear of the dial's own carrier), so a
station in the window doesn't set it.

    --start HZ / --stop HZ / --step HZ
                    dial frequencies (default 1,000,000 to 40,000,000 in
                    250,000 Hz steps: 157 frequencies, about 6 minutes)
    --dwell S       seconds of I/Q averaged per state (default 0.25)
    --settle S      seconds of I/Q discarded after each change (default
                    0.15), longer than the capture and stream queues
    --passes N      1 or 2 (default 1); 2 doubles the time
    --out PREFIX    output name (default lpf_sweep_<time>)
    --replot FILE   no radio: report a sweep's _raw.csv again

Outputs: PREFIX.csv (each filter against LPF_A, A above the floor),
PREFIX_raw.csv (every state's level, for --replot), and PREFIX.png if
matplotlib is installed. Printed: each filter's passband loss against A,
its -3 and -20 dB points, and its attenuation at the 2nd and 3rd
harmonics of the bands it carries.

What to measure with. The depth of the result is how far the source is
above the receiver's own noise. Antenna noise gives 10 to 20 dB on the
low bands and less higher up: the passbands, the losses and the
cutoffs, but little of the stopbands. A broadband noise generator at the
antenna jack (with an attenuator if it overloads the receiver) gives
40 dB or more. Where a filter's excess falls into the floor, the report
says "below -N dB": the floor, not the filter. Antenna noise is strong
only in places (the low bands, the broadcast bands), and frequencies
where LPF_A isn't 6 dB above the floor are stepped over; an edge that
falls in such a gap is given as "between X and Y MHz".

The sweep switches the relays about 800 times. maxibitx refuses to
transmit through a held filter: a transmission releases the hold and
selects the band's own filter first, and the sweep then reports those
states as unmeasured. The hold is released and the dial restored at the
end, also on Ctrl-C.

Needs a maxibitx with rigctld's LPF level (docs/06_api.md) on an sBitx,
the I/Q stream on UDP 4536, numpy, and xtal_sweep.py beside this file;
matplotlib only for the plot.
"""

import argparse
import csv
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xtal_sweep import (FREQS, IQ_RATE_HZ, IQ_STREAM_PORT, NFFT,  # noqa: E402
                        RIGCTL_PORT, IqStream, Rigctl, np, step_spectrum)

STATES = ("A", "B", "C", "D", "none")   # L LPF 1..5
FILTERS = STATES[:4]
WINDOW_HZ = (1000.0, 10000.0)           # |offset| from the dial measured
MIN_EXCESS_DB = 3.0                     # above the floor, or "below the floor"
MIN_REF_DB = 6.0                        # LPF_A above the floor, to compare against
A_FLAT_HZ = 28000000                    # LPF_A taken as flat up to here
EDGE_HOLD_POINTS = 2                    # an edge: this many points in a row below

# The sBitx's switch points (radio_hw.c) and the bands each filter carries.
SWITCH_HZ = {"D": 5500000, "C": 10500000, "B": 18500000, "A": 30000000}
BANDS = (("80 m", 3500000, 4000000), ("60 m", 5330500, 5406500),
         ("40 m", 7000000, 7300000), ("30 m", 10100000, 10150000),
         ("20 m", 14000000, 14350000), ("17 m", 18068000, 18168000),
         ("15 m", 21000000, 21450000), ("12 m", 24890000, 24990000),
         ("10 m", 28000000, 29700000))


def filter_for(hz):
    for x in ("D", "C", "B", "A"):
        if hz < SWITCH_HZ[x]:
            return x
    return None


def level_db(spec):
    """The median bin in the window either side of the dial, dBFS per Hz."""
    sel = (np.abs(FREQS) >= WINDOW_HZ[0]) & (np.abs(FREQS) <= WINDOW_HZ[1])
    per_hz = float(np.median(spec[sel])) / (IQ_RATE_HZ / NFFT)
    return 10 * math.log10(max(per_hz, 1e-30))


# ---- analysis ----------------------------------------------------------------

def analyse(raw):
    """raw: (n, 5) levels in dB, NaN where missing, columns STATES.
    Returns (vs_a, a_above, bound): vs_a[x] is filter x against A in dB
    (NaN where unmeasured, where x is within MIN_EXCESS_DB of the floor,
    or A within MIN_REF_DB); a_above is A's level above the floor; bound[x] is what the
    floor allows x vs A to be told from, where x falls into it."""
    lin = 10 ** (raw / 10)
    floor = lin[:, 4]
    with np.errstate(invalid="ignore", divide="ignore"):
        excess = lin[:, :4] - floor[:, None]
        usable = lin[:, :4] > floor[:, None] * 10 ** (MIN_EXCESS_DB / 10)
        ref_ok = lin[:, 0] > floor * 10 ** (MIN_REF_DB / 10)
        a = np.where(ref_ok, excess[:, 0], np.nan)
        vs_a, bound = {}, {}
        for i, x in enumerate(FILTERS):
            vs_a[x] = np.where(usable[:, i], 10 * np.log10(excess[:, i] / a), np.nan)
            smallest = floor * (10 ** (MIN_EXCESS_DB / 10) - 1)
            bound[x] = np.where(~usable[:, i], 10 * np.log10(smallest / a), np.nan)
        a_above = 10 * np.log10(lin[:, 0] / floor)
    return vs_a, a_above, bound


def at(freqs, values, hz):
    """values at hz, by linear interpolation, or NaN outside / unmeasured."""
    if hz < freqs[0] or hz > freqs[-1]:
        return float("nan")
    i = int(np.searchsorted(freqs, hz))
    if i < len(freqs) and freqs[i] == hz:
        return float(values[i])
    lo, hi = i - 1, i
    t = (hz - freqs[lo]) / (freqs[hi] - freqs[lo])
    return float(values[lo] * (1 - t) + values[hi] * t)


def edge(freqs, rel, bnd, drop, start_hz):
    """Where rel first falls `drop` dB below its passband (0 dB) above
    start_hz, to stay there for EDGE_HOLD_POINTS known points. A point
    is known when it was measured, or fell into the floor with a bound
    (bnd) already that far down; the rest - where the source was too weak
    - are stepped over, so an edge is found in the next stretch the
    source reaches. Returns (lo_hz, hi_hz, why): equal when interpolated
    between neighbouring points, a range when the crossing lies in a gap,
    or (None, None, why) if it never falls that far where measured."""
    below = np.where(np.isnan(rel), np.nan_to_num(bnd, nan=0.0) <= -drop, rel <= -drop)
    known = ~np.isnan(rel) | below
    idx = [i for i in np.nonzero(known)[0] if freqs[i] > start_hz]
    if not idx:
        return None, None, "not measurable"
    prev = None   # the last known point above the line
    for n, i in enumerate(idx):
        if not below[i]:
            prev = i
            continue
        if prev is None or not all(below[j] for j in idx[n:n + EDGE_HOLD_POINTS]):
            continue
        if i == prev + 1 and not math.isnan(rel[i]):
            a, b = rel[prev], rel[i]
            hz = float(freqs[prev] + (a + drop) / (a - b) * (freqs[i] - freqs[prev]))
            return hz, hz, ""
        return float(freqs[prev]), float(freqs[i]), ""
    return None, None, f"not reached where measured (to {freqs[idx[-1]] / 1e6:.2f} MHz)"


def fmt_edge(drop, lo, hi, why):
    if lo is None:
        return f"-{drop} dB {why}"
    star = "*" if hi > A_FLAT_HZ else ""
    if lo == hi:
        return f"-{drop} dB at {lo / 1e6:.2f} MHz{star}"
    return f"-{drop} dB between {lo / 1e6:.2f} and {hi / 1e6:.2f} MHz{star}"


def fmt_db(v, b):
    if not math.isnan(v):
        return f"{v:6.1f} dB"
    if not math.isnan(b):
        return f"below {b:.0f} dB"
    return "      --"


def report(freqs, raw, prefix):
    """Prints the summary and writes PREFIX.csv and the plot. Returns the
    paths written."""
    vs_a, a_above, bound = analyse(raw)
    written = []
    path = prefix + ".csv"
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["freq_hz", "B_vs_A_db", "C_vs_A_db", "D_vs_A_db", "A_above_floor_db",
                    "B_floor_bound_db", "C_floor_bound_db", "D_floor_bound_db"])
        for i, hz in enumerate(freqs):
            w.writerow([int(hz)] + [round(float(v), 2) if not math.isnan(v) else ""
                                    for v in (vs_a["B"][i], vs_a["C"][i], vs_a["D"][i],
                                              a_above[i], bound["B"][i], bound["C"][i],
                                              bound["D"][i])])
    written.append(path)

    print()
    print("Each filter against LPF_A (flat to about 30 MHz). Figures marked * are")
    print("above 28 MHz, where A is rolling off too, so they read high by A's own loss.")
    for x in ("D", "C", "B"):
        lo_hz = freqs[0]
        top = SWITCH_HZ[x]
        pas = vs_a[x][(freqs >= lo_hz) & (freqs <= top)]
        pas = pas[~np.isnan(pas)]
        if not len(pas):
            print(f"\nLPF_{x}: no passband measured")
            continue
        p0 = float(np.median(pas))
        rel, bnd = vs_a[x] - p0, bound[x] - p0
        points = []
        for drop in (3, 20):
            points.append(fmt_edge(drop, *edge(freqs, rel, bnd, drop, lo_hz)))
        print(f"\nLPF_{x} (switched below {top / 1e6:g} MHz): passband {p0:+.1f} dB vs A "
              f"(scatter {np.std(pas):.1f} dB), " + ", ".join(points))
        for name, b_lo, b_hi in BANDS:
            if filter_for(b_lo) != x:
                continue
            c = (b_lo + b_hi) / 2
            cells = []
            for k in (1, 2, 3):
                hz = k * c
                v, b = at(freqs, vs_a[x], hz), at(freqs, bound[x], hz)
                star = "*" if hz > A_FLAT_HZ else " "
                if hz > freqs[-1]:
                    cells.append(f"{k}f {hz / 1e6:5.2f} MHz: beyond the sweep")
                else:
                    cells.append(f"{k}f {hz / 1e6:5.2f} MHz{star} {fmt_db(v, b)}")
            print(f"  {name:5} " + "   ".join(cells))
    a_ok = a_above[~np.isnan(a_above)]
    if len(a_ok):
        ref = float(np.median(a_above[(freqs <= A_FLAT_HZ) & ~np.isnan(a_above)])) \
            if np.any((freqs <= A_FLAT_HZ) & ~np.isnan(a_above)) else float("nan")
        print(f"\nLPF_A above the floor: {np.min(a_ok):.0f} to {np.max(a_ok):.0f} dB "
              f"(median {ref:.0f} dB below 28 MHz) - the depth this source allows")
        e3, _, _ = edge(freqs, a_above - ref, np.full(len(freqs), np.nan), 3, A_FLAT_HZ / 2)
        weak = freqs[~(a_above >= MIN_REF_DB)]
        if len(weak):
            print(f"LPF_A is less than {MIN_REF_DB:g} dB above the floor at {len(weak)} "
                  f"frequencies, "
                  f"from {weak[0] / 1e6:.2f} MHz: the source is too weak there to measure "
                  "anything (a noise generator would reach further)")
        if e3:
            print(f"LPF_A's level falls 3 dB below that at {e3 / 1e6:.2f} MHz "
                  "(the source's and receiver's own slope included)")
    png = plot(prefix + ".png", freqs, raw, vs_a, a_above, bound)
    if png:
        written.append(png)
    return written


def plot(path, freqs, raw, vs_a, a_above, bound):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return None
    mhz = freqs / 1e6
    fig, (top, bot) = plt.subplots(2, 1, figsize=(11, 8), sharex=True,
                                   gridspec_kw={"height_ratios": [1, 1.4]})
    colors = {"A": "#1f77b4", "B": "#2ca02c", "C": "#ff7f0e", "D": "#d62728", "none": "#555555"}
    for i, s in enumerate(STATES):
        top.plot(mhz, raw[:, i], color=colors[s], lw=1.2,
                 ls="--" if s == "none" else "-", label=f"LPF_{s}" if s != "none" else "all off")
    top.set_ylabel("level, dBFS/Hz")
    top.set_title("Received noise with each filter held")
    top.legend(ncol=5, fontsize=8)
    top.grid(alpha=0.3)
    for x in ("D", "C", "B"):
        bot.plot(mhz, vs_a[x], color=colors[x], lw=1.5, label=f"LPF_{x} vs A")
        bot.plot(mhz, bound[x], color=colors[x], lw=0.8, ls=":")
    bot.plot(mhz, a_above - np.nanmedian(a_above[freqs <= A_FLAT_HZ]), color=colors["A"],
             lw=1.2, ls="--", label="LPF_A above floor (re its median)")
    for x in ("D", "C", "B", "A"):
        bot.axvline(SWITCH_HZ[x] / 1e6, color=colors[x], lw=0.6, alpha=0.5)
    bot.set_ylabel("dB")
    bot.set_xlabel("MHz")
    bot.set_title("Each filter against LPF_A (dotted: where it fell into the floor)")
    shown = np.concatenate([v[~np.isnan(v)] for v in list(vs_a.values()) + list(bound.values())])
    bot.set_ylim(bottom=max(-80.0, float(np.min(shown)) - 5 if len(shown) else -80.0), top=5)
    bot.legend(fontsize=8)
    bot.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=100)
    return path


# ---- raw file ----------------------------------------------------------------

def write_raw(path, freqs, raw, blocks):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["freq_hz"] + [f"{s}_dbfs_hz" for s in STATES] + ["blocks"])
        for i, hz in enumerate(freqs):
            w.writerow([int(hz)] + [round(float(v), 3) if not math.isnan(v) else ""
                                    for v in raw[i]] + [blocks[i]])


def read_raw(path):
    freqs, rows = [], []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            freqs.append(int(r["freq_hz"]))
            rows.append([float(r[f"{s}_dbfs_hz"]) if r[f"{s}_dbfs_hz"] else np.nan
                         for s in STATES])
    return np.array(freqs, dtype=float), np.array(rows)


# ---- the sweep ---------------------------------------------------------------

def measure_state(rig, stream, code, args):
    """One state's level in dB, or NaN if the hold was refused or released
    while measuring. Also the FFT blocks it used."""
    if not rig.set_ok(f"L LPF {code}"):
        return float("nan"), 0
    stream.flush()
    for _ in stream.packets(int(args.settle * IQ_RATE_HZ)):
        pass
    spec, blocks = step_spectrum(stream, args.dwell)
    if spec is None or rig.get_int("l LPF") != code:
        return float("nan"), 0
    return level_db(spec), blocks


def main():
    ap = argparse.ArgumentParser(description="Measure the sBitx's LPFs through its receiver")
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=RIGCTL_PORT)
    ap.add_argument("--iq-port", type=int, default=IQ_STREAM_PORT)
    ap.add_argument("--start", type=int, default=1000000)
    ap.add_argument("--stop", type=int, default=40000000)
    ap.add_argument("--step", type=int, default=250000)
    ap.add_argument("--dwell", type=float, default=0.25)
    ap.add_argument("--settle", type=float, default=0.15)
    ap.add_argument("--passes", type=int, default=1, choices=(1, 2))
    ap.add_argument("--out", help="output name prefix (default lpf_sweep_<time>)")
    ap.add_argument("--replot", metavar="RAW_CSV", help="report a sweep's _raw.csv again")
    args = ap.parse_args()

    if args.replot:
        freqs, raw = read_raw(args.replot)
        prefix = args.out or (args.replot[:-8] if args.replot.endswith("_raw.csv")
                              else args.replot) + "_replot"
        for path in report(freqs, raw, prefix):
            print(f"wrote {path}")
        return

    if args.step <= 0 or args.stop < args.start or args.start <= 0:
        sys.exit("need 0 < --start <= --stop and a positive --step")
    freqs = np.arange(args.start, args.stop + 1, args.step, dtype=float)
    prefix = args.out or time.strftime("lpf_sweep_%Y%m%d_%H%M%S")

    rig = Rigctl(args.host, args.port)
    hold = rig.get_int("l LPF")
    if hold is None:
        sys.exit("this maxibitx has no LPF level - it needs a build with rigctld's "
                 "L LPF (docs/06_api.md)")
    if rig.get_int("t") == 1:
        sys.exit("the radio is transmitting - try again in receive")
    if not rig.set_ok("L LPF 0"):
        sys.exit("maxibitx refused L LPF: this board's LPFs aren't in the receive path "
                 "(the zBitx's carry only the transmitter)")
    original_dial = rig.get_int("f")

    stream = IqStream(args.host, args.iq_port)
    probe, _ = step_spectrum(stream, 0.5)
    if probe is None:
        sys.exit(f"no I/Q arriving on UDP {args.iq_port} from {args.host}")

    per_freq = len(STATES) * args.passes * (args.dwell + args.settle) + args.settle
    print(f"sweeping {args.start}..{args.stop} Hz in {args.step} Hz steps, "
          f"{len(freqs)} frequencies x {len(STATES)} states x {args.passes} pass(es), "
          f"about {len(freqs) * per_freq / 60:.0f} minutes")
    print()
    print(f"{'dial Hz':>10}  " + "  ".join(f"{s:>6}" for s in STATES) + "   dBFS/Hz")

    raw = np.full((len(freqs), len(STATES)), np.nan)
    blocks = [0] * len(freqs)
    try:
        for i, hz in enumerate(freqs):
            if not rig.set_ok(f"F {int(hz)}"):
                print(f"{int(hz):>10}  tuning refused")
                continue
            # A different first state at each frequency, so no state is
            # always the one measured straight after the retune.
            order = [STATES[(k + i) % len(STATES)] for k in range(len(STATES))]
            if args.passes == 2:
                order += order[::-1]
            sums = {s: [] for s in STATES}
            for s in order:
                v, b = measure_state(rig, stream, STATES.index(s) + 1, args)
                blocks[i] += b
                if not math.isnan(v):
                    sums[s].append(10 ** (v / 10))
            for k, s in enumerate(STATES):
                if sums[s]:
                    raw[i, k] = 10 * math.log10(sum(sums[s]) / len(sums[s]))
            print(f"{int(hz):>10}  " + "  ".join(
                f"{v:6.1f}" if not math.isnan(v) else "    --" for v in raw[i]), flush=True)
    except KeyboardInterrupt:
        print("\ninterrupted - releasing the hold and reporting what was measured")
    finally:
        if not rig.set_ok("L LPF 0"):
            print("WARNING: could not release the LPF hold - send 'L LPF 0', "
                  "or transmit, or restart maxibitx")
        if original_dial is not None:
            rig.set_ok(f"F {original_dial}")
        rig.close()

    write_raw(prefix + "_raw.csv", freqs, raw, blocks)
    written = [prefix + "_raw.csv"] + report(freqs, raw, prefix)
    print()
    for path in written:
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
