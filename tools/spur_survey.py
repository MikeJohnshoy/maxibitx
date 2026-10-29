#!/usr/bin/env python3
"""
spur_survey.py - finds the receiver's own spurs (birdies) with a 50 ohm
dummy load on the antenna jack, and says from how each one moves with
the dial where it is made.

    spur_survey.py --host sbitx.local [options]
    spur_survey.py --reanalyse spur_survey_<time>_steps.npz [--threshold DB]
    spur_survey.py --compare BEFORE_spurs.csv AFTER_spurs.csv
    spur_survey.py --audible DUMMY_spurs.csv ANTENNA_bands.csv

What it does. The dial steps through the ham bands (each widened by
25 kHz) in 5 kHz steps. At each step the I/Q spectrum is averaged
(8192-point FFTs, 11.7 Hz bins, half overlapped) and searched within
+-10 kHz of the dial - the crystal filter's flat top - so every RF
frequency is seen from four dial settings. A bin is a candidate when it
is a local maximum at least --threshold dB (7) above the median of the
bins within +-1 kHz; its frequency is refined between bins.

How a spur moves says what it is. Its offset from the dial, against the
dial, is a straight line, offset = k + s * dial, and the slope s is the
class:

    s = -1  fixed RF     one RF frequency (k), seen at four offsets, as a
                         station would be: radiated or conducted into the
                         RF input - clock harmonics, regulators
    s =  0  fixed offset the same offset (k) at every dial: made after the
                         first mixer (the IF, the codec, the DSP)
    s = +1  backward     moves with the dial: an image. A spur at RF F
                         48 kHz below the dial images through the second
                         mixer at offset dial - 48 kHz - F
    other   mixing       a product of the oscillators, reported where it
                         crosses the dial. Up to |s| = 4 it is a low
                         harmonic of the first LO (clk2); steeper ones
                         are matched against products of the si5351's two
                         PLLs (A, clk2's, at clk2 x its divider, moving
                         10-16 times as fast as the dial; B, clk1's, fixed)
                         and its 25 MHz reference
    -       single       seen once or twice only - appears and disappears

Lines are found from pairs of candidates up to three steps apart, with
slopes in quarter steps up to 4 and whole numbers beyond, up to the
steepest that can be seen twice at the step (20 kHz / step: 4 at 5 kHz
steps, 200 at 100 Hz). Steep lines are grouped by where they cross the
dial, which an error in the slope barely moves, and fitted by least
squares; a sighting may sit 35 Hz plus 5 Hz per unit of slope from its
line, since a PLL product carries the si5351's fractional-setting error
multiplied. A line needs three sightings (two where the step or the
slope lets it cross the window in fewer, except a steep one: two
sightings at neighbouring dials fit some whole-number slope by chance
too often). For fixed-RF spurs the report
lists clock harmonics within their tolerance (the codec's 12 MHz and
96 kHz frame, the si5351's 25 MHz TCXO, the Pi's crystals, clk1 and its
sub-multiples); for steep ones the PLL products whose crossing and
slope fit. Suspects, not proof. A line steeper than about 4 crosses the
window between two 5 kHz steps: --range with small steps finds it.

Runs that locate a source (docs/dsp_design_notes/
rx_artifacts_and_spur_survey_study.md §4.6) - compare each with the
baseline using --compare:

    --lpf-off       every LPF relay open for the run (L LPF 5): spurs that
                    vanish come in through the antenna path
    --drive MA      the si5351's drive for the run (L SIDRIVE 2/4/6/8 mA,
                    restored after): its harmonics, and the products
                    they make, against the 8 mA start-up drive
    --clk1-div N    clk1's si5351 divider for the run (L CLK1DIV 16/18/20/22,
                    restored after): PLL B (clk1's) moved out of PLL A's
                    range, so the two PLLs' beat leaves the bands; 18 is the
                    automatic choice
    (by hand)       the Pi quieter (HDMI, Wi-Fi, USB off), another supply,
                    a different xtal_filter_center

    --bands LIST    comma-separated: 160,80,60,40,30,20,17,15,12,10
                    (default 80 m to 10 m, 60 m included: about 8 minutes)
    --general       1-30 MHz in 10 kHz steps instead (about 30 minutes)
    --range LO HI   dials from LO to HI Hz instead (step 1000 Hz unless
                    --step), e.g. --range 5360000 5368000 --step 100
    --step HZ / --dwell S / --settle S / --threshold DB
                    5000, 0.4, 0.15, 7
    --out PREFIX    output name (default spur_survey_<time>)

A step that gets no I/Q, or under half the expected, is measured again
once; the steps that still got nothing are listed at the end.

Which spurs are heard (--audible). A spur matters by ear when, at some
dial setting, it is inside the passband and louder there than the
antenna's noise. --audible takes a dummy-load run's _spurs.csv and the
_bands.csv of a run made with the antenna connected (its spur list is
full of stations; only each band's floor is used) and lists the spurs
that would be heard:

    CW     the narrow filter, --cw-width wide (300 Hz) around the pitch:
           offsets within +-150 Hz of the dial (CW and CWR alike)
    SSB    300-3000 Hz: LSB below 10 MHz, USB above (and on 60 m);
           none on 30 m

with the dials where each is heard, and its margin: the spur's level
over the antenna noise in --ear-hz (50 Hz, about what the ear picks a
tone out of noise with). A tone can be heard well under the noise in
the whole 2.7 kHz. Worst first, 60 m last; also written to
PREFIX_audible.csv. The antenna's noise changes with the time of day and
the band's openings; a quiet band is the worst case.

Outputs: PREFIX_spurs.csv (the classified list), PREFIX_peaks.csv
(every candidate), PREFIX_bands.csv (each band's noise floor and spur
count), PREFIX_steps.npz (every step's spectrum, for --reanalyse) and
PREFIX.png if matplotlib is installed.

Limits. A dummy load shows only what the radio makes itself: images,
intermodulation and aliasing of real signals need a signal. Keep the
dummy load's lead short and the same between runs: a spur the Pi
radiates into it is real, but its level depends on the lead. Levels are
dBFS (a full-scale tone is 0) and dB above the floor, not microvolts.
The floor with a dummy load is the receiver's own noise; a lower LO
drive's effect on gain shows on a real signal, not here.

Needs a maxibitx with the I/Q stream on UDP 4536, numpy, and
xtal_sweep.py beside this file (its rigctld and I/Q code); --lpf-off
needs L LPF (sBitx), --drive needs L SIDRIVE, --clk1-div needs L CLK1DIV.
"""

import argparse
import csv
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xtal_sweep import (IQ_RATE_HZ, IQ_STREAM_PORT, RIGCTL_PORT,  # noqa: E402
                        IqStream, Rigctl, np)

NFFT = 8192
HOP = NFFT // 2
BIN_HZ = IQ_RATE_HZ / NFFT              # 11.72 Hz
WINDOW_HZ = 10000                       # searched either side of the dial
FLOOR_HZ = 1000                         # the floor: median within this
LOCAL_MAX_BINS = 4                      # a peak is the highest within this
NARROW_BINS = 4                         # bins within 6 dB of the peak: narrow
LINE_TOL_HZ = 35                        # a sighting's distance from its line
SI_QUANT_HZ = 5.0                       # si5351 output error per unit of slope
GUARD_HZ = 25000

BANDS = {"160": (1800000, 2000000), "80": (3500000, 4000000), "60": (5330500, 5406500),
         "40": (7000000, 7300000), "30": (10100000, 10150000), "20": (14000000, 14350000),
         "17": (18068000, 18168000), "15": (21000000, 21450000), "12": (24890000, 24990000),
         "10": (28000000, 29700000)}
DEFAULT_BANDS = "80,60,40,30,20,17,15,12,10"

# Clocks whose harmonics are suspects for a fixed-RF spur:
# (name, Hz, ppm, family). Within a family a match on the clock itself
# hides the same match on its sub-multiples.
CLOCKS = [("si5351 TCXO 25 MHz", 25e6, 3, "tcxo"), ("TCXO/2 12.5 MHz", 12.5e6, 3, "tcxo"),
          ("codec 12 MHz", 12e6, 50, "codec"), ("codec/2 6 MHz", 6e6, 50, "codec"),
          ("codec frame 96 kHz", 96e3, 50, "codec"),
          ("the 48.0135 kHz comb", 48013.5, 5, "comb"),
          ("Pi 4 54/2 = 27 MHz", 27e6, 30, "pi4"), ("Pi 4 54/3 = 18 MHz", 18e6, 30, "pi4"),
          ("Pi 4 54/4 = 13.5 MHz", 13.5e6, 30, "pi4"),
          ("Pi Zero 2W 19.2 MHz", 19.2e6, 30, "pi02"),
          ("Pi Zero 2W 19.2/2 = 9.6 MHz", 9.6e6, 30, "pi02")]

CLASSES = {-1.0: "fixed RF", 0.0: "fixed offset", 1.0: "backward"}


def band_of(hz):
    for name, (lo, hi) in BANDS.items():
        if lo <= hz <= hi:
            return f"{name} m"
    return ""


# ---- measuring --------------------------------------------------------------

WINDOW = np.hanning(NFFT)
SINE_NORM = np.sum(WINDOW) ** 2          # a full-scale tone reads 0 dBFS
ENBW_BINS = NFFT * np.sum(WINDOW ** 2) / np.sum(WINDOW) ** 2
OFFSETS = (np.arange(NFFT) - NFFT // 2) * BIN_HZ
KEEP = np.abs(OFFSETS) <= WINDOW_HZ + FLOOR_HZ   # what is kept of each step


def step_spectrum(stream, seconds):
    """Power per bin, fftshifted (OFFSETS order), averaged over `seconds` of
    I/Q in half-overlapped Hann blocks, each from unbroken packets; a tone
    of amplitude A reads A^2. Returns (spectrum or None, blocks)."""
    acc, blocks = None, 0
    pending = np.zeros(0, dtype=np.complex128)
    for contiguous, samples in stream.packets(int(seconds * IQ_RATE_HZ)):
        if not contiguous:
            pending = np.zeros(0, dtype=np.complex128)
        pending = np.concatenate([pending, samples])
        while len(pending) >= NFFT:
            spec = np.abs(np.fft.fftshift(np.fft.fft(pending[:NFFT] * WINDOW))) ** 2 / SINE_NORM
            acc = spec if acc is None else acc + spec
            blocks += 1
            pending = pending[HOP:]
    return (acc / blocks if blocks else None), blocks


def find_peaks(level_db, threshold_db):
    """Candidates in one step's kept spectrum (dB, OFFSETS[KEEP] order).
    Returns (peaks, floor_dbfs_hz): peaks as dicts of offset_hz, above_db
    (over the local floor), dbfs and width_hz (at -6 dB)."""
    offs = OFFSETS[KEEP]
    p = 10 ** (level_db / 10)
    half = int(FLOOR_HZ / BIN_HZ)
    padded = np.pad(p, half, mode="edge")
    floor = np.median(np.lib.stride_tricks.sliding_window_view(padded, 2 * half + 1), axis=1)
    above = 10 * np.log10(p / floor)
    lm = LOCAL_MAX_BINS
    padded = np.pad(p, lm, mode="constant")
    local_max = p >= np.max(np.lib.stride_tricks.sliding_window_view(padded, 2 * lm + 1), axis=1)
    search = np.abs(offs) <= WINDOW_HZ
    peaks = []
    for k in np.nonzero((above >= threshold_db) & local_max & search)[0]:
        lo, hi = k, k
        while lo > 0 and p[lo - 1] >= p[k] / 4:
            lo -= 1
        while hi < len(p) - 1 and p[hi + 1] >= p[k] / 4:
            hi += 1
        d = 0.0
        if 0 < k < len(p) - 1:
            a, b, c = level_db[k - 1], level_db[k], level_db[k + 1]
            den = a - 2 * b + c
            d = 0.5 * (a - c) / den if den else 0.0
        peaks.append({"offset_hz": float(offs[k] + d * BIN_HZ), "above_db": float(above[k]),
                      "dbfs": float(level_db[k]), "width_hz": float((hi - lo + 1) * BIN_HZ)})
    floor_hz = float(np.median(floor[search])) / (ENBW_BINS * BIN_HZ)
    return peaks, 10 * math.log10(max(floor_hz, 1e-30))


# ---- classifying ------------------------------------------------------------

def line_tol(s):
    """How far a sighting may sit from its line. A product of the si5351's
    PLLs carries their fractional-setting error (up to 25 MHz / 2^19 =
    48 Hz at the PLL, about SI_QUANT_HZ at the output) multiplied by the
    slope, so steep lines get a wider tolerance."""
    return LINE_TOL_HZ + SI_QUANT_HZ * abs(s)


def max_slope(step_hz):
    """The steepest line that can be seen twice: one crossing the +-10 kHz
    window in two steps."""
    return max(4.0, 2 * WINDOW_HZ / step_hz)


def quantise_slope(s):
    """Quarter steps up to 4 (clk2's low harmonics), whole numbers beyond
    (the PLLs' products: a multiple of the PLL divider, less one)."""
    return round(s * 4) / 4 if abs(s) <= 4.5 else float(round(s))


def anchor(s, dial, off):
    """A line's position that a small error in its slope barely moves:
    the offset itself for s = 0, otherwise the dial where the line
    crosses offset 0 (the RF for s = -1)."""
    return off if s == 0 else dial - off / s


def min_sightings(s, step_hz):
    """Sightings a line needs: three, unless the step or the slope lets a
    spur cross the window in fewer. A steep line (slope over 4) always
    needs three: two sightings at neighbouring dials fit some whole-number
    slope about one time in five by chance, so two unrelated lines would
    pass for one; a smaller step sees a real one three times or more."""
    if abs(s) > 4.5:
        return 3
    crossings = 2 * WINDOW_HZ / (step_hz * max(abs(s), 1.0)) if s != 0 else 99
    return 3 if crossings >= 3 else 2


def fit_line(dial, off, idx, s_fixed=None):
    """Fits offset = k + s * dial through idx, dropping the worst sighting
    while any is outside line_tol(s). s_fixed holds the slope (the classes
    at -1, 0 and +1). Returns (members, s, k)."""
    idx = list(idx)
    while idx:
        d, o = dial[idx], off[idx]
        if s_fixed is None and (len(idx) < 2 or (len(idx) == 2 and d[1] == d[0])):
            return [], 0.0, 0.0  # no slope from one dial
        if s_fixed is not None or len(idx) < 3:
            s = s_fixed if s_fixed is not None else (o[-1] - o[0]) / (d[-1] - d[0])
            k = float(np.median(o - s * d))
        else:
            s, k = np.polyfit(d - d.mean(), o, 1)
            k = k - s * d.mean()
        resid = np.abs(o - (k + s * d))
        worst = int(np.argmax(resid))
        if resid[worst] <= line_tol(s):
            return idx, float(s), float(k)
        idx.pop(worst)
    return [], 0.0, 0.0


def classify(cands, step_hz):
    """cands: list of dicts with dial, offset_hz, above_db, dbfs, width_hz.
    Returns (spurs, singles). A spur is a line offset = k + s * dial
    through its sightings, at least min_sightings() of them from distinct
    dials, each within line_tol(s) of it."""
    n = len(cands)
    dial = np.array([c["dial"] for c in cands], dtype=float)
    off = np.array([c["offset_hz"] for c in cands])
    order = np.argsort(dial, kind="stable")
    smax = max_slope(step_hz)
    votes = {}
    for ii, i in enumerate(order):
        for j in order[ii + 1:]:
            dd = dial[j] - dial[i]
            if dd == 0:
                continue
            if dd > 3 * step_hz + 1:
                break
            s = (off[j] - off[i]) / dd
            sq = quantise_slope(s)
            if abs(sq) > smax:
                continue
            if abs(sq) <= 4.5 and abs(s - sq) * dd > line_tol(sq):
                continue
            width = LINE_TOL_HZ if abs(sq) <= 1.5 else 100.0
            key = (sq, int(round(anchor(sq, dial[i], off[i]) / width)))
            votes.setdefault(key, set()).update((i, j))
    claimed = np.zeros(n, bool)
    spurs = []
    steep_pairs = 0
    for (sq, aq), _members in sorted(votes.items(), key=lambda kv: -len(kv[1])):
        slopes = [sq] if abs(sq) <= 4.5 else [sq - 1, sq, sq + 1]
        pool = set()
        for s2 in slopes:
            for da in (-1, 0, 1):
                pool |= votes.get((s2, aq + da), set())
        pool = [i for i in pool if not claimed[i]]
        if not pool:
            continue
        # one sighting per dial: the strongest
        best = {}
        for i in pool:
            if dial[i] not in best or cands[i]["above_db"] > cands[best[dial[i]]]["above_db"]:
                best[dial[i]] = i
        pool = sorted(best.values(), key=lambda i: dial[i])
        fixed = sq if sq in CLASSES else None
        members, s_fit, k = fit_line(dial, off, pool, fixed)
        s_cls = fixed if fixed is not None else quantise_slope(s_fit)
        if len(members) < min_sightings(s_cls, step_hz):
            steep_pairs += abs(s_cls) > 4.5 and len(members) == 2
            continue
        claimed[members] = True
        top = max(members, key=lambda i: cands[i]["above_db"])
        spurs.append({"cls": CLASSES.get(s_cls, "mixing"), "slope": s_cls, "slope_fit": s_fit,
                      "k_hz": k, "cross_hz": k if s_cls == 0 else -k / s_fit,
                      "sightings": len(members), "above_db": cands[top]["above_db"],
                      "dbfs": cands[top]["dbfs"], "width_hz": cands[top]["width_hz"],
                      "dial_lo": float(min(dial[members])), "dial_hi": float(max(dial[members])),
                      "rf_seen": [float(dial[i] + off[i]) for i in members]})
    singles = [cands[i] for i in range(n) if not claimed[i]]
    return spurs, singles, steep_pairs


# ---- the si5351's PLLs ---------------------------------------------------------

SI_XTAL = 25000000
SI_DENOM = 0x80000


def si_pll(freq, div=0):
    """The PLL frequency and output divider si5351v2.c uses for an output
    at freq: div if given, else the smallest even divider above 650 MHz /
    freq; the PLL a 25 MHz multiple plus a fraction in 1/2^19 steps."""
    if not div:
        div = 650000000 // int(freq) + 1
        if div & 1:
            div += 1
    pll = int(freq) * div
    num = ((pll % SI_XTAL) * SI_DENOM) // SI_XTAL
    return SI_XTAL * (pll // SI_XTAL + num / SI_DENOM), div


def pll_products(dial0, s_fit, clk1_hz, clk1_div=0):
    """Products of PLL A (clk2's), PLL B (clk1's) and the 25 MHz reference
    that cross the dial within 3 kHz of dial0 at the fitted slope:
    sgn * (m * A - n * B + k * 25 MHz), whose offset moves at
    sgn * m * (A's divider) - 1 times the dial."""
    xc = clk1_hz - 24000
    a0, div_a = si_pll(dial0 + xc)
    b, _ = si_pll(clk1_hz, clk1_div)
    out = []
    for m in (1, 2, 3):
        for nb in (0, 1, 2, 3):
            for k in range(-12, 13):
                for sgn in (1, -1):
                    s_pred = sgn * m * div_a - 1
                    if abs(s_pred - s_fit) > max(1.5, 0.05 * abs(s_fit)):
                        continue
                    p0 = sgn * (m * a0 - nb * b + k * SI_XTAL)
                    cross = dial0 - (p0 - dial0) / s_pred
                    if abs(cross - dial0) > 3000:
                        continue
                    term = (f"{m}x" if m > 1 else "") + "PLL A"
                    if nb:
                        term += f" - {nb}x" if nb > 1 else " - "
                        term += "PLL B"
                    if k:
                        term += f" {'+' if k > 0 else '-'} {abs(k)}x25 MHz"
                    if sgn < 0:
                        term = f"-({term})"
                    out.append((abs(cross - dial0), f"{term} (crosses at {cross / 1e6:.6f} MHz)"))
    return [t for _, t in sorted(out)[:2]]


CLK1_LATTICE_A = 30      # clk1 multiples tried for a fixed-RF spur
CLK1_LATTICE_B = 60      # 25 MHz multiples
CLK1_LATTICE_TOL_HZ = 15  # a measured frequency's error (the TCXO itself reads ~7 Hz low)


def clk1_lattice(k, clk1_hz, clk1_div=0):
    """The simplest a x clk1 + b x 25 MHz (a != 0) within
    CLK1_LATTICE_TOL_HZ of k, as text, or "". clk1 is the frequency the
    si5351 really makes: PLL B's fractional setting divided down."""
    pll, div = si_pll(clk1_hz, clk1_div)
    c1 = pll / div
    best = None
    for a in range(-CLK1_LATTICE_A, CLK1_LATTICE_A + 1):
        if a == 0:
            continue
        b = round((k - a * c1) / SI_XTAL)
        if abs(b) <= CLK1_LATTICE_B and abs(k - a * c1 - b * SI_XTAL) <= CLK1_LATTICE_TOL_HZ:
            if best is None or abs(a) + abs(b) < abs(best[0]) + abs(best[1]):
                best = (a, b)
    if best is None:
        return ""
    a, b = best
    return f"{a} x clk1 {'+' if b >= 0 else '-'} {abs(b)} x 25 MHz"


def suspects(spur, spurs, clk1_hz, clk1_div=0):
    """Likely sources, as text."""
    s, k = spur["slope"], spur["k_hz"]
    if s == -1:
        bases = CLOCKS + [("clk1 (RX)", clk1_hz, 3, "clk1"), ("clk1/2", clk1_hz / 2, 3, "clk1"),
                          ("clk1/3", clk1_hz / 3, 3, "clk1")]
        hits = []
        for name, f, ppm, fam in bases:
            h = round(k / f)
            if h >= 1 and abs(k - h * f) <= h * f * ppm * 1e-6 + LINE_TOL_HZ:
                hits.append((name, f, h, fam))
        # the codec's 12 MHz, not also its 6 MHz and 96 kHz frame
        hits = [x for x in hits if not any(y[3] == x[3] and y[1] > x[1] for y in hits)]
        out = [f"{h} x {name}" if h > 1 else name for name, _, h, _ in hits]
        if not any(x[3] in ("tcxo", "clk1") for x in hits):
            lattice = clk1_lattice(k, clk1_hz, clk1_div)
            if lattice:
                out.append(lattice)
        return "; ".join(out)
    if s == 0:
        return "at the dial (fs/4 in the codec, or LO leakage)" if abs(k) < 100 else \
            "made after the first mixer"
    if s == 1:
        f = -k - 48000.0
        found = [x for x in spurs if x["slope"] == -1 and abs(x["k_hz"] - f) <= 2 * LINE_TOL_HZ]
        return (f"second-mixer image of {f / 1e6:.6f} MHz" +
                (" (found there: a fixed-RF spur)" if found else ""))
    if abs(s) >= 5:
        found = pll_products(spur["cross_hz"], spur["slope_fit"], clk1_hz, clk1_div)
        return "; ".join(found) if found else "steep: no PLL A/B product found there"
    return f"{abs(s):g}x the first LO (clk2) in a mixing product"


# ---- reporting ---------------------------------------------------------------

def spur_where(sp):
    if sp["slope"] == -1:
        return f"{sp['k_hz'] / 1e6:12.6f} MHz"
    if sp["slope"] == 0:
        return f"{sp['k_hz']:+9.0f} Hz from dial"
    if sp["slope"] == 1:
        return f"seen {np.mean(sp['rf_seen']) / 1e6:9.4f} MHz"
    return f"crosses {sp['cross_hz'] / 1e6:10.6f} MHz"


def report(cands, dials, floors, step_hz, meta, prefix, spectra=None):
    spurs, singles, steep_pairs = classify(cands, step_hz)
    clk1 = meta.get("clk1_hz", 40036400.0)
    for sp in spurs:
        sp["suspects"] = suspects(sp, spurs, clk1, meta.get("clk1_div", 0))
        # the bands it was seen in: by RF, or for a fixed offset by the dial
        where = sp["rf_seen"] if sp["slope"] != 0 else [r - sp["k_hz"] for r in sp["rf_seen"]]
        sp["bands"] = ",".join(sorted({band_of(r) for r in where} - {""},
                                      key=lambda b: BANDS[b.split()[0]][0]))
    spurs.sort(key=lambda x: (x["slope"] != -1, x["slope"] != 0, x["slope"] != 1,
                              x["k_hz"] if x["slope"] in (-1, 0) else x["cross_hz"]))
    written = []

    path = prefix + "_spurs.csv"
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["class", "slope", "k_hz", "rf_seen_mean_hz", "sightings", "above_floor_db",
                    "dbfs", "width_hz", "dial_lo_hz", "dial_hi_hz", "bands", "suspects",
                    "slope_fit", "crosses_dial_hz"])
        for sp in spurs:
            w.writerow([sp["cls"], sp["slope"], round(sp["k_hz"], 1),
                        round(float(np.mean(sp["rf_seen"])), 1), sp["sightings"],
                        round(sp["above_db"], 1), round(sp["dbfs"], 1), round(sp["width_hz"], 1),
                        int(sp["dial_lo"]), int(sp["dial_hi"]), sp["bands"], sp["suspects"],
                        round(sp["slope_fit"], 3), round(sp["cross_hz"], 1)])
    written.append(path)
    path = prefix + "_peaks.csv"
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["dial_hz", "offset_hz", "rf_hz", "above_floor_db", "dbfs", "width_hz"])
        for c in cands:
            w.writerow([int(c["dial"]), round(c["offset_hz"], 1),
                        round(c["dial"] + c["offset_hz"], 1), round(c["above_db"], 1),
                        round(c["dbfs"], 1), round(c["width_hz"], 1)])
    written.append(path)
    band_rows = []
    for name, (lo, hi) in BANDS.items():
        sel = [fl for d, fl in zip(dials, floors) if lo - GUARD_HZ <= d <= hi + GUARD_HZ]
        if not sel:
            continue
        n_sp = sum(1 for sp in spurs if f"{name} m" in sp["bands"].split(","))
        band_rows.append((f"{name} m", float(np.median(sel)), n_sp))
    path = prefix + "_bands.csv"
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["band", "floor_dbfs_hz", "spurs"])
        for b, fl, n_sp in band_rows:
            w.writerow([b, round(fl, 1), n_sp])
    written.append(path)

    print()
    print(f"{len(cands)} candidates, {len(spurs)} spurs, {len(singles)} seen once or twice "
          f"(conditions: {meta.get('conditions', '')})")
    for cls in ("fixed RF", "fixed offset", "backward", "mixing"):
        group = [sp for sp in spurs if sp["cls"] == cls]
        if not group:
            continue
        print(f"\n{cls}:")
        for sp in group:
            slope = f" s={sp['slope_fit']:+.2f}" if cls == "mixing" else ""
            print(f"  {spur_where(sp)}{slope}  {sp['above_db']:5.1f} dB over floor "
                  f"({sp['dbfs']:6.1f} dBFS)  x{sp['sightings']}  {sp['bands'] or '-':12} "
                  f"{sp['suspects']}")
    if steep_pairs:
        print(f"\n{steep_pairs} pair(s) of sightings at neighbouring dials could be steep "
              f"lines (slope over 4) but none was seen three times: --range with a smaller "
              f"--step would tell (a slope s needs a step under {2 * WINDOW_HZ // 3} / s Hz)")
    if singles:
        strong = sorted(singles, key=lambda c: -c["above_db"])[:10]
        print(f"\nseen once or twice ({len(singles)}; strongest {len(strong)}):")
        for c in strong:
            print(f"  {(c['dial'] + c['offset_hz']) / 1e6:12.6f} MHz  (dial {c['dial'] / 1e6:.3f}"
                  f", {c['offset_hz']:+7.0f} Hz)  {c['above_db']:5.1f} dB over floor")
    print("\nnoise floor by band (dBFS/Hz): " +
          ", ".join(f"{b} {fl:.1f}" for b, fl, _ in band_rows))
    if spectra is not None:
        png = plot(prefix + ".png", dials, spectra, spurs, singles, meta.get("range"))
        if png:
            written.append(png)
    return written


def plot(path, dials, spectra, spurs, singles, dial_range=None):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return None
    offs = OFFSETS[KEEP]
    search = np.abs(offs) <= WINDOW_HZ
    # a fixed-offset spur is at every dial: drawn in RF it would be a fence
    fixed = [sp["k_hz"] for sp in spurs if sp["cls"] == "fixed offset"]
    for k in fixed:
        search &= np.abs(offs - k) > 3 * BIN_HZ
    # panels: (title, first dial, last dial, band shading or None)
    if dial_range:
        lo, hi = dial_range
        shown = [(f"{lo / 1e6:.4f}-{hi / 1e6:.4f} MHz", lo, hi, None)]
    else:
        shown = [(f"{n} m", lo - GUARD_HZ, hi + GUARD_HZ, (lo, hi)) for n, (lo, hi) in BANDS.items()
                 if any(lo - GUARD_HZ <= d <= hi + GUARD_HZ for d in dials)]
    if not shown:
        return None
    fig, axes = plt.subplots(len(shown), 1, figsize=(11, 2.2 * len(shown)), squeeze=False)
    colors = {"fixed RF": "#d62728", "fixed offset": "#1f77b4", "backward": "#9467bd",
              "mixing": "#ff7f0e"}
    for ax, (name, a, b, band) in zip(axes[:, 0], shown):
        grid = np.arange(a - WINDOW_HZ, b + WINDOW_HZ, 50.0)
        hold = np.full(len(grid), np.nan)
        for d, spec in zip(dials, spectra):
            if not a <= d <= b:
                continue
            rf = d + offs[search]
            idx = np.clip(((rf - grid[0]) / 50.0).astype(int), 0, len(grid) - 1)
            vals = spec[search] - np.median(spec[search])
            np.fmax.at(hold, idx, vals)
        ax.plot(grid / 1e6, hold, color="#444", lw=0.6)
        for sp in spurs:
            if sp["cls"] == "fixed offset":
                continue
            for r in sp["rf_seen"]:
                if a - WINDOW_HZ <= r <= b + WINDOW_HZ:
                    ax.plot(r / 1e6, sp["above_db"], "v", ms=6, color=colors[sp["cls"]])
        for c in singles:
            r = c["dial"] + c["offset_hz"]
            if a - WINDOW_HZ <= r <= b + WINDOW_HZ:
                ax.plot(r / 1e6, c["above_db"], "x", ms=4, color="#999")
        if band:
            ax.axvspan(band[0] / 1e6, band[1] / 1e6, color="#2ca02c", alpha=0.06)
        ax.set_title(name, fontsize=9, loc="left")
        ax.set_ylabel("dB over floor", fontsize=8)
        ax.grid(alpha=0.3)
    handles = [plt.Line2D([], [], marker="v", ls="", color=c, label=k) for k, c in colors.items()
               if k != "fixed offset"]
    handles.append(plt.Line2D([], [], marker="x", ls="", color="#999", label="seen once or twice"))
    axes[0, 0].legend(handles=handles, fontsize=7, ncol=5, loc="upper right")
    note = ("; fixed-offset spurs left out: " + ", ".join(f"{k:+.0f} Hz" for k in fixed)) \
        if fixed else ""
    axes[-1, 0].set_xlabel("MHz (max-hold over the steps; markers: classified spurs" + note + ")")
    fig.tight_layout()
    fig.savefig(path, dpi=90)
    return path


def compare(path_a, path_b):
    def load(p):
        with open(p, newline="") as f:
            return list(csv.DictReader(f))

    def bands(p):
        q = p.replace("_spurs.csv", "_bands.csv")
        try:
            with open(q, newline="") as f:
                return {r["band"]: float(r["floor_dbfs_hz"]) for r in csv.DictReader(f)}
        except OSError:
            return {}

    a, b = load(path_a), load(path_b)

    def key(r):
        s = float(r["slope"])
        if s in (-1.0, 0.0):
            return s, float(r["k_hz"])
        if s != 1.0 and r.get("crosses_dial_hz"):
            return s, float(r["crosses_dial_hz"])
        return s, float(r["rf_seen_mean_hz"])

    used = set()
    print(f"{'':12} {'where':>24}  {'A dB':>6} {'B dB':>6}  change")
    for r in a:
        sa, ka = key(r)
        match = None
        for j, q in enumerate(b):
            sb, kb = key(q)
            tol = 2 * LINE_TOL_HZ if sa in (-1.0, 0.0) else 3000
            if j not in used and sa == sb and abs(ka - kb) <= tol:
                match = j
                break
        where = (f"{ka / 1e6:.6f} MHz" if sa == -1 else f"{ka:+.0f} Hz" if sa == 0
                 else f"~{ka / 1e6:.4f} MHz s={sa:+g}")
        if match is None:
            print(f"{r['class']:12} {where:>24}  {float(r['above_floor_db']):6.1f} {'--':>6}  "
                  "only in A")
        else:
            used.add(match)
            da, db_ = float(r["above_floor_db"]), float(b[match]["above_floor_db"])
            print(f"{r['class']:12} {where:>24}  {da:6.1f} {db_:6.1f}  {db_ - da:+.1f} dB")
    for j, q in enumerate(b):
        if j not in used:
            sb, kb = key(q)
            where = (f"{kb / 1e6:.6f} MHz" if sb == -1 else f"{kb:+.0f} Hz" if sb == 0
                     else f"~{kb / 1e6:.4f} MHz s={sb:+g}")
            print(f"{q['class']:12} {where:>24}  {'--':>6} {float(q['above_floor_db']):6.1f}  "
                  "only in B")
    fa, fb = bands(path_a), bands(path_b)
    common = [x for x in fa if x in fb]
    if common:
        print("\nnoise floor, B - A: " + ", ".join(f"{x} {fb[x] - fa[x]:+.1f} dB" for x in common))


# ---- the survey --------------------------------------------------------------

def plan_dials(args):
    if args.range:
        lo, hi = args.range
        step = args.step or 1000
        if not 0 < lo < hi or (hi - lo) // step > 20000:
            sys.exit("--range needs 0 < LO < HI, and no more than 20000 steps")
        return list(range(lo, hi + 1, step)), step
    if args.general:
        step = args.step or 10000
        return list(range(1000000, 30000001, step)), step
    step = args.step or 5000
    dials = []
    for name in args.bands.split(","):
        name = name.strip().rstrip("m").strip()
        if name not in BANDS:
            sys.exit(f"unknown band {name!r}: choose from {', '.join(BANDS)}")
        lo, hi = BANDS[name]
        dials += list(range(lo - GUARD_HZ, hi + GUARD_HZ + 1, step))
    return sorted(set(dials)), step


# ---- which are heard ---------------------------------------------------------

SSB_LO_HZ, SSB_HI_HZ = 300, 3000
EDGE_HZ = 100                           # within this of a passband edge: "at the edge"
LSB_BANDS = ("160", "80", "40")
NO_SSB_BANDS = ("30",)


def passbands(band, cw_width):
    """(mode, lowest offset, highest offset) heard on band."""
    out = [("CW", -cw_width / 2, cw_width / 2)]
    if band in LSB_BANDS:
        out.append(("LSB", -SSB_HI_HZ, -SSB_LO_HZ))
    elif band not in NO_SSB_BANDS:
        out.append(("USB", SSB_LO_HZ, SSB_HI_HZ))
    return out


def heard(r, cw_width):
    """Where a spur (a _spurs.csv row) is heard: a list of (band, mode,
    dial_lo, dial_hi, offsets), dial_lo == dial_hi = None meaning at every
    dial on that band; and the line's slope and where it crosses the dial."""
    s = float(r["slope"])
    out = []
    if s == 0:
        k = float(r["k_hz"])
        for b in [x.split()[0] for x in r["bands"].split(",") if x]:
            for mode, lo, hi in passbands(b, cw_width):
                if lo <= k <= hi:
                    out.append((b, mode, None, None, (k, k)))
        return out, 0.0, None
    sf = float(r.get("slope_fit") or s) or s
    # offset = k + s * dial crosses 0 at -k / s (runs before the
    # crosses_dial_hz column was written have only k).
    cross = float(r.get("crosses_dial_hz") or -float(r["k_hz"]) / s)
    # Where it was seen, widened by the window it was seen in: the line
    # isn't assumed to run beyond that.
    reach = WINDOW_HZ / max(abs(sf), 1.0)
    seen_lo, seen_hi = float(r["dial_lo_hz"]) - reach, float(r["dial_hi_hz"]) + reach
    for b, (blo, bhi) in BANDS.items():
        if bhi < seen_lo or blo > seen_hi:
            continue
        for mode, lo, hi in passbands(b, cw_width):
            # offset = sf * (dial - cross)
            d1, d2 = sorted((cross + lo / sf, cross + hi / sf))
            d1, d2 = max(d1, blo, seen_lo), min(d2, bhi, seen_hi)
            if d1 <= d2:
                o1, o2 = sorted((sf * (d1 - cross), sf * (d2 - cross)))
                out.append((b, mode, d1, d2, (o1, o2)))
    return out, sf, cross


def audible(spurs_path, bands_path, ear_hz, cw_width, out_path):
    with open(spurs_path, newline="") as f:
        spurs = list(csv.DictReader(f))
    with open(bands_path, newline="") as f:
        floors = {r["band"].split()[0]: float(r["floor_dbfs_hz"]) for r in csv.DictReader(f)}
    ear_db = 10 * math.log10(ear_hz)
    rows, unknown = [], set()
    for r in spurs:
        level = float(r["dbfs"])
        s = float(r["slope"])
        where_heard, sf, cross = heard(r, cw_width)
        sus = r.get("suspects", "")
        if s == -1 and not sus:  # runs made before a clock was added to CLOCKS
            sus = suspects({"slope": -1.0, "k_hz": float(r["k_hz"])}, [], 40036400.0)
        if s == 0:
            spur = f"{float(r['k_hz']):+.0f} Hz from the dial"
        elif s == -1:
            spur = f"{float(r['k_hz']) / 1e6:.6f} MHz"
        else:
            spur = f"{r['class']}, slope {sf:+.2f}, crossing {cross / 1e6:.6f} MHz"
        by_band = {}
        for b, mode, d1, d2, (o1, o2) in where_heard:
            if b not in floors:
                unknown.add(b)
                continue
            lo, hi = next((lo, hi) for m, lo, hi in passbands(b, cw_width) if m == mode)
            edge = min(o1 - lo, hi - o2) < EDGE_HZ and not (o1 <= lo + EDGE_HZ and o2 >= hi - EDGE_HZ)
            where = "every dial" if d1 is None else f"{d1 / 1e6:.5f}-{d2 / 1e6:.5f}"
            by_band.setdefault(b, []).append(f"{mode} {where}" + (" (filter edge)" if edge else ""))
        for b, heard_at in by_band.items():
            rows.append({"band": f"{b} m", "margin_db": round(level - (floors[b] + ear_db), 1),
                         "spur": spur, "heard_at": ", ".join(heard_at), "class": r["class"],
                         "dbfs": round(level, 1), "antenna_floor_dbfs_hz": floors[b],
                         "suspects": sus})
    rows.sort(key=lambda x: (x["band"] == "60 m", -x["margin_db"]))
    loud = [x for x in rows if x["margin_db"] >= 0]
    near = [x for x in rows if -6 <= x["margin_db"] < 0]
    print(f"Spurs in {os.path.basename(spurs_path)} against the antenna noise in "
          f"{os.path.basename(bands_path)}, taken in {ear_hz:g} Hz. Heard at: the dials "
          f"(MHz) where each is in the CW filter ({cw_width:g} Hz) or the SSB passband "
          f"({SSB_LO_HZ}-{SSB_HI_HZ} Hz).\n")
    print(f"{len(loud)} heard (0 dB or more over the antenna noise), {len(near)} within 6 dB "
          "under it. Worst first, 60 m last.\n")

    def show(x):
        print(f"{x['band']:6} {x['margin_db']:+6.1f}  {x['spur']:44} {x['heard_at']}"
              + (f"  [{x['suspects']}]" if x["suspects"] else ""))
    print(f"{'band':6} {'dB':>6}  {'spur':44} heard at")
    for x in loud:
        show(x)
    if near:
        print("-- within 6 dB under the antenna noise --")
        for x in near:
            show(x)
    if unknown:
        print(f"\nNo antenna floor for {', '.join(sorted(unknown, key=int))} m: not judged.")
    with open(out_path, "w", newline="") as f:
        cols = ["band", "margin_db", "spur", "heard_at", "class", "dbfs",
                "antenna_floor_dbfs_hz", "suspects"]
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerows(loud + near)
    print(f"\nwrote {out_path}")


def main():
    ap = argparse.ArgumentParser(description="Find the receiver's own spurs with a dummy load")
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=RIGCTL_PORT)
    ap.add_argument("--iq-port", type=int, default=IQ_STREAM_PORT)
    ap.add_argument("--bands", default=DEFAULT_BANDS)
    ap.add_argument("--general", action="store_true", help="1-30 MHz in 10 kHz steps")
    ap.add_argument("--range", nargs=2, type=int, metavar=("LO_HZ", "HI_HZ"),
                    help="dials from LO to HI (default step 1000 Hz)")
    ap.add_argument("--step", type=int)
    ap.add_argument("--dwell", type=float, default=0.4)
    ap.add_argument("--settle", type=float, default=0.15)
    ap.add_argument("--threshold", type=float, default=7.0)
    ap.add_argument("--lpf-off", action="store_true", help="every LPF relay open (L LPF 5)")
    ap.add_argument("--drive", type=int, choices=(2, 4, 6, 8), help="si5351 drive, mA")
    ap.add_argument("--clk1-div", type=int, choices=(16, 18, 20, 22),
                    help="clk1's si5351 divider (18 is the automatic one)")
    ap.add_argument("--out", help="output name prefix (default spur_survey_<time>)")
    ap.add_argument("--reanalyse", metavar="STEPS_NPZ")
    ap.add_argument("--compare", nargs=2, metavar=("A_SPURS_CSV", "B_SPURS_CSV"))
    ap.add_argument("--audible", nargs=2, metavar=("DUMMY_SPURS_CSV", "ANTENNA_BANDS_CSV"))
    ap.add_argument("--ear-hz", type=float, default=50.0,
                    help="bandwidth the antenna noise is taken in for --audible (50 Hz)")
    ap.add_argument("--cw-width", type=float, default=300.0,
                    help="CW filter width for --audible (300 Hz)")
    args = ap.parse_args()

    if args.compare:
        compare(*args.compare)
        return

    if args.audible:
        sp, bd = args.audible
        out = (args.out + "_audible.csv") if args.out else sp.replace("_spurs.csv", "") + "_audible.csv"
        audible(sp, bd, args.ear_hz, args.cw_width, out)
        return

    if args.reanalyse:
        z = np.load(args.reanalyse, allow_pickle=False)
        dials, spectra = z["dials"], z["spectra"]
        meta = {"clk1_hz": float(z["clk1_hz"]), "conditions": str(z["conditions"])}
        if "clk1_div" in z.files:
            meta["clk1_div"] = int(z["clk1_div"])
        if "range_lo" in z.files and int(z["range_lo"]):
            meta["range"] = (int(z["range_lo"]), int(z["range_hi"]))
        step = int(z["step"])
        cands, floors = [], []
        for d, spec in zip(dials, spectra):
            peaks, fl = find_peaks(spec.astype(float), args.threshold)
            floors.append(fl)
            cands += [dict(p, dial=float(d)) for p in peaks]
        prefix = args.out or args.reanalyse.replace("_steps.npz", "") + "_reanalysed"
        for path in report(cands, list(dials), floors, step, meta, prefix, spectra):
            print(f"wrote {path}")
        return

    dials, step = plan_dials(args)
    prefix = args.out or time.strftime("spur_survey_%Y%m%d_%H%M%S")
    rig = Rigctl(args.host, args.port)
    if rig.get_int("t") == 1:
        sys.exit("the radio is transmitting - try again in receive")
    original_dial = rig.get_int("f")
    xtal = rig.get_int("l XTALCENTER")
    clk1 = (xtal + 24000) if xtal else 40036400
    original_drive = rig.get_int("l SIDRIVE")
    conditions = []
    if args.drive is not None:
        if original_drive is None or not rig.set_ok(f"L SIDRIVE {args.drive}"):
            sys.exit("could not set the si5351 drive (this maxibitx needs L SIDRIVE)")
    drive_now = rig.get_int("l SIDRIVE")
    conditions.append(f"si5351 {drive_now} mA" if drive_now else "si5351 drive unknown")
    original_div = rig.get_int("l CLK1DIV")
    if args.clk1_div is not None:
        if original_div is None or not rig.set_ok(f"L CLK1DIV {args.clk1_div}"):
            sys.exit("could not set clk1's divider (this maxibitx needs L CLK1DIV)")
        time.sleep(0.2)  # the si5351's PLLs relock
    div_now = rig.get_int("l CLK1DIV")
    if div_now:
        conditions.append(f"clk1 divider {div_now}")
    if args.lpf_off:
        if not rig.set_ok("L LPF 5"):
            sys.exit("could not open the LPF relays (L LPF 5 needs an sBitx and a maxibitx "
                     "with L LPF)")
        conditions.append("LPF relays open")
    else:
        conditions.append("LPF by the dial")
    conditions.append(f"xtal_filter_center {xtal} Hz" if xtal else "xtal_filter_center ?")

    stream = IqStream(args.host, args.iq_port)
    probe, _ = step_spectrum(stream, 0.2)
    if probe is None:
        sys.exit(f"no I/Q arriving on UDP {args.iq_port} from {args.host}")

    per = args.dwell + args.settle + 0.03
    print(f"{len(dials)} steps of {step} Hz, {', '.join(conditions)}, "
          f"about {len(dials) * per / 60:.0f} minutes")
    cands, floors, kept, done, missed = [], [], [], [], []
    retried = 0
    expected = max(1, (int(args.dwell * IQ_RATE_HZ) - NFFT) // HOP + 1)  # FFT blocks a step
    try:
        for n, d in enumerate(dials):
            if not rig.set_ok(f"F {d}"):
                print(f"{d:>10}  tuning refused")
                continue
            for attempt in (1, 2):
                stream.flush()
                for _ in stream.packets(int(args.settle * IQ_RATE_HZ)):
                    pass
                spec, blocks = step_spectrum(stream, args.dwell)
                if spec is not None and blocks >= expected / 2:
                    break
                if attempt == 1:
                    print(f"{d:>10}  {'no' if spec is None else 'too little'} I/Q - again",
                          flush=True)
                    retried += 1
            if spec is None:
                print(f"{d:>10}  no data")
                missed.append(d)
                continue
            level = 10 * np.log10(spec[KEEP] + 1e-30)
            peaks, fl = find_peaks(level, args.threshold)
            cands += [dict(p, dial=float(d)) for p in peaks]
            floors.append(fl)
            kept.append(level.astype(np.float32))
            done.append(d)
            if n % 20 == 0 or peaks:
                top = max((p["above_db"] for p in peaks), default=None)
                print(f"{d:>10}  floor {fl:6.1f} dBFS/Hz  {len(peaks)} peak(s)"
                      + (f", strongest {top:.1f} dB" if top is not None else ""), flush=True)
    except KeyboardInterrupt:
        print("\ninterrupted - restoring and reporting what was measured")
    finally:
        if args.lpf_off and not rig.set_ok("L LPF 0"):
            print("WARNING: could not release the LPF hold - send 'L LPF 0'")
        if args.drive is not None and original_drive and \
                not rig.set_ok(f"L SIDRIVE {original_drive}"):
            print(f"WARNING: could not restore the si5351 drive - send 'L SIDRIVE "
                  f"{original_drive}'")
        if args.clk1_div is not None and original_div:
            back = 0 if original_div == 18 else original_div  # 18: the automatic one
            if not rig.set_ok(f"L CLK1DIV {back}"):
                print(f"WARNING: could not restore clk1's divider - send 'L CLK1DIV {back}'")
        if original_dial is not None:
            rig.set_ok(f"F {original_dial}")
        rig.close()

    if retried or missed:
        print(f"\n{retried} step(s) measured again; {len(missed)} with no data"
              + (f": {', '.join(str(m) for m in missed[:10])}" if missed else ""))
    meta = {"clk1_hz": float(clk1), "conditions": ", ".join(conditions),
            "clk1_div": div_now or 0}
    if args.range:
        meta["range"] = tuple(args.range)
    np.savez_compressed(prefix + "_steps.npz", dials=np.array(done),
                        spectra=np.array(kept) if kept else np.zeros((0, int(np.sum(KEEP)))),
                        clk1_hz=clk1, conditions=meta["conditions"], step=step,
                        clk1_div=div_now or 0,
                        range_lo=args.range[0] if args.range else 0,
                        range_hi=args.range[1] if args.range else 0)
    written = [prefix + "_steps.npz"] + report(cands, done, floors, step, meta, prefix,
                                               np.array(kept) if kept else None)
    print()
    for path in written:
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
