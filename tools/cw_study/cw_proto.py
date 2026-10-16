#!/usr/bin/env python3
"""
cw_proto.py - the reference model for src/cw_decoder.c's timing
classifier: marks and gaps in, text out, learning everything from the
signal (no WPM setting, no oracle).

  marks  the last MARK_HIST mark lengths, in log seconds, split in two
         (dot, dash) by the threshold that best separates them (Otsu: the
         largest between-class variance). If they don't separate (a run
         of E, T, S, O...), the gaps inside characters, which are one unit
         like a dot, say which class they are.
  gaps   the last GAP_HIST gaps, split the same way into short (inside a
         character) and long; the long ones split again into between
         characters and between words, if they separate. If they don't,
         the word threshold sits WORD_OVER_CHAR above the character gaps.
  start  nothing is decoded until MIN_MARKS marks and MIN_GAPS gaps have
         been seen; what came before is decoded then, with what was
         learned.

  pause  a gap of PAUSE_S, and PAUSE_OVER_WORD word thresholds, ends the
         transmission: what was learned is dropped.

The dot/dash and character/word thresholds sit half way between the two
classes' centres (in log); the character threshold sits CHAR_FRAC of the
way up from the gaps inside characters. A mark or gap shorter than BLIP
dots (MIN_MARK_S at least) is noise: it joins the event before it.

src/cw_decoder.c is this classifier in C, behind its own detector;
docs/dsp_design_notes/rx_cw_decoder_study.md §8 compares them.

  python3 tools/cw_study/cw_proto.py          # the grid, through the bench's simple detector, ~5 min
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cw_fist as cf  # noqa: E402

MARK_HIST = 32
GAP_HIST = 64
MIN_MARKS = 4
MIN_GAPS = 4
SEPARATED = math.log(1.8)      # two classes are distinct if their centres differ this much
WORD_SEPARATED = math.log(1.6)
WORD_OVER_CHAR = math.log(1.7)
MIN_MARK_S = 0.006
PAUSE_S = 2.0             # a gap this long, and PAUSE_OVER_WORD word thresholds,
PAUSE_OVER_WORD = 2.5     # ends a transmission: learn afresh
BLIP = 0.35               # marks and gaps shorter than this many dots are noise
CHAR_FRAC = 0.42          # intra/char threshold, this far from the intra centre (log)
WORD_FRAC = 0.5           # char/word threshold, this far from the char centre (log)


def otsu(values):
    """
    (threshold, low centre, high centre) of the best two-way split, in log;
    the threshold is the midpoint of the centres.
    """
    v = np.sort(np.asarray(values))
    n = len(v)
    if n < 2:
        return None
    c = np.cumsum(v)
    best, best_k = -1.0, 1
    for k in range(1, n):
        m0, m1 = c[k - 1] / k, (c[-1] - c[k - 1]) / (n - k)
        var = k * (n - k) * (m1 - m0) ** 2
        if var > best:
            best, best_k = var, k
    lo, hi = v[:best_k].mean(), v[best_k:].mean()
    return (lo + hi) / 2, lo, hi


class Timing:
    def __init__(self):
        self.marks = []
        self.gaps = []

    def add_mark(self, s):
        self.marks = (self.marks + [math.log(s)])[-MARK_HIST:]

    def add_gap(self, s):
        self.gaps = (self.gaps + [math.log(s)])[-GAP_HIST:]

    def ready(self):
        return len(self.marks) >= MIN_MARKS and len(self.gaps) >= MIN_GAPS

    def gap_thresholds(self):
        """(intra/char, char/word) thresholds and the intra-gap centre, log s."""
        sp = otsu(self.gaps)
        if sp is None:
            return math.log(0.1), math.log(0.25), math.log(0.06)
        _, lo, hi = sp
        if hi - lo < SEPARATED:          # no character gaps in the window yet
            hi = lo + math.log(3.0)
        t1 = lo + CHAR_FRAC * (hi - lo)
        long = [g for g in self.gaps if g > t1]
        sp2 = otsu(long) if len(long) >= 2 else None
        if sp2 and sp2[2] - sp2[1] >= WORD_SEPARATED:
            t2 = sp2[1] + WORD_FRAC * (sp2[2] - sp2[1])
        else:
            centre = float(np.mean(long)) if long else hi
            t2 = centre + WORD_OVER_CHAR
        return t1, t2, lo

    def mark_threshold(self, intra):
        sp = otsu(self.marks)
        if sp and sp[2] - sp[1] >= SEPARATED:
            return sp[0]
        centre = float(np.mean(self.marks)) if self.marks else intra
        # one class: dots if about as long as the gaps inside characters
        if centre - intra < math.log(2.0):
            return centre + math.log(1.7)     # all dots
        return centre - math.log(1.7)          # all dashes

    def dot_s(self):
        sp = otsu(self.marks)
        return math.exp(sp[1]) if sp and sp[2] - sp[1] >= SEPARATED else 0.06


def decode(events):
    """Text from (is_mark, start_s, seconds) events, in order."""
    tm = Timing()
    out = []
    code = ""
    pending = []                 # events before the classifier is ready
    # join noise blips to their neighbours: a very short mark to the gaps
    # around it, a very short gap to the marks around it
    ev = []
    for e in events:
        m, t0, d = e
        if ev and ev[-1][0] == m:
            ev[-1] = (m, ev[-1][1], ev[-1][2] + d)
            continue
        if ev and d < max(MIN_MARK_S, BLIP * tm.dot_s()):
            # a blip: it becomes part of the event before it, and the next
            # event (of that same kind) joins them by the rule above
            ev[-1] = (ev[-1][0], ev[-1][1], ev[-1][2] + d)
            continue
        ev.append(e)

    def classify(m, d):
        nonlocal code
        t1, t2, intra = tm.gap_thresholds()
        x = math.log(d)
        if m:
            code += "." if x < tm.mark_threshold(intra) else "-"
            return
        if x >= t1:
            if code:
                out.append(cf.DECODE.get(code, "*"))
            code = ""
            if x >= t2:
                out.append(" ")

    for m, _, d in ev:
        if not m and tm.ready() and d >= max(PAUSE_S, PAUSE_OVER_WORD * math.exp(tm.gap_thresholds()[1])):
            # a pause: finish what was being sent, then start learning again,
            # since the next transmission may be another station
            for pm, pd in pending:
                classify(pm, pd)
            pending = []
            if code:
                out.append(cf.DECODE.get(code, "*"))
                code = ""
            if out and out[-1] != " ":
                out.append(" ")
            tm = Timing()
            continue
        if m:
            tm.add_mark(d)
        else:
            tm.add_gap(d)
        if not tm.ready():
            pending.append((m, d))
            continue
        for pm, pd in pending:
            classify(pm, pd)
        pending = []
        classify(m, d)
    if code:
        out.append(cf.DECODE.get(code, "*"))
    return "".join(out)


def main():
    import cw_bench as cb
    from concurrent.futures import ProcessPoolExecutor

    cells = [(f, w, None) for f in cb.GRID_FISTS for w in cb.SPEEDS]
    cells += [(f, w, c) for f in ("good", "average") for (w, c) in cb.FARNS]
    with ProcessPoolExecutor(2) as ex:
        res = list(ex.map(_run, [(c, s) for c in cells for s in cb.SEEDS]))
    print(" fist       WPM   proto CER  +sp    ceiling   (median CER %, 15 dB)")
    tot = []
    for c in cells:
        rs = [r for k, r in res if k == c]
        a = np.median([r[0][0] for r in rs]) * 100
        b = np.median([r[0][1] for r in rs]) * 100
        ce = np.mean([r[1][0] for r in rs]) * 100
        tot += [r[0] for r in rs]
        sp = f"{c[1]}/{c[2]}" if c[2] else f"{c[1]}"
        print(f" {c[0]:8s} {sp:>6s}  {a:8.1f}  {b:5.1f}   {ce:7.1f}")
    print(f" mean CER {np.mean([t[0] for t in tot])*100:.1f}%, "
          f"with word breaks {np.mean([t[1] for t in tot])*100:.1f}%")


def _run(arg):
    import cw_bench as cb
    (f, w, fa), seed = arg
    rng = np.random.default_rng(seed * 1000 + int(w) * 7 + 15)
    text = cf.qso_text(rng, cb.CHARS)
    el = cf.key_text(text, f, w, rng, fa)
    x = cf.audio(el, 15, rng)
    det = cb.simple_detector(x)
    return (f, w, fa), (cb.cer(text, decode(det)), cb.cer(text, cb.ceiling_decode(el)))


if __name__ == "__main__":
    main()
