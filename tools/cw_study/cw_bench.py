#!/usr/bin/env python3
"""
cw_bench.py - the bench behind docs/dsp_design_notes/rx_cw_decoder_study.md.

Keys QSO-like text with the fists in cw_fist.py, makes the audio a
receiver would hand its decoder, and measures:

  detector  the KB2ML decoder (sbitx's modem_cw.c, receive half) on
            machine-perfect code at 20 WPM, against SNR: how many marks
            it finds, and how long it measures dots, dashes and the gaps
            inside characters, against the truth (60, 180 and 60 ms)
  grid      its character error rate (CER) for each fist and speed, at
            one SNR, with the decoder's WPM setting (the operator's own
            keyer speed, which it uses as a prior) at 20 and at the
            sender's speed
  ceiling   the CER of a timing-only decoder that knows the sender's
            speed at every moment and uses the best fixed thresholds for
            that run: what the fist itself costs, before any detector or
            speed tracking (no language model)
  ambiguity how often splitting a character at one of its gaps gives two
            valid characters, and merging two adjacent characters gives
            one, over the text

CER is the edit distance between the decoded and the sent text divided by
the sent text's length, with spaces removed from both (so it counts
characters), and "+sp" the same with single spaces kept (so word breaks
count too).

  python3 tools/cw_study/cw_bench.py --sbitx ~/sbitx/src          # all, ~15 min
  python3 tools/cw_study/cw_bench.py --sbitx ~/sbitx/src --only ceiling,ambiguity
  python3 tools/cw_study/cw_bench.py --sbitx ~/sbitx/src --wav DIR    # also WAVs
  python3 tools/cw_study/cw_bench.py --sbitx ~/sbitx/src --decode rec.wav --wpm 20

--sbitx is the src directory of a checkout of MikeJohnshoy/sbitx, branch
mj_decoder (measured at 6875d43). The bench builds two harnesses from its
modem_cw.c with kb2ml/harness.c: the decoder as it is, and the same with
one fprintf added to cw_rx_add_symbol() that logs each mark and gap (the
detector table). --decode runs the decoder on a recording instead (any
rate, left channel, scaled so its peaks sit where sbitx's AGC puts them),
to check the harness against what the radio shows for the same audio.
Needs gcc, libfftw3-dev, numpy and scipy.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cw_fist as cf  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TICK_S = 1024 / cf.FS          # the decoder's time step: 10.67 ms, or CW_HOP if defined
SPEEDS = (10, 15, 20, 25, 30, 35)
FARNS = ((5, 18), (10, 18))    # (overall, character) WPM
GRID_FISTS = ("machine", "good", "average", "poor", "heavy", "bug")
CHARS = 250
SEEDS = (1, 2, 3, 4)
TRACE_LINE = ('  if (p == &decoder) fprintf(stderr, "%c%d ", '
              'symbol == \' \' ? \'s\' : \'M\', p->ticker);\n')


# ---------------------------------------------------------------- harness

def build(sbitx_src, outdir):
    """
    The decoder harness and its tracing twin; returns their paths. Sets
    TICK_S from modem_cw.c: one cw_rx() block, or CW_HOP samples at 12 kHz
    when the file defines it (the detector fix on branch decoder_fix).
    """
    global TICK_S
    plain = os.path.join(outdir, "kb2ml")
    trace = os.path.join(outdir, "kb2ml_trace")
    inc = os.path.join(HERE, "kb2ml")
    src = os.path.join(sbitx_src, "modem_cw.c")
    cmd = ["gcc", "-O2", "-w", "-I" + inc, "-I" + sbitx_src, os.path.join(inc, "harness.c")]
    subprocess.run(cmd + [src, "-lfftw3", "-lm", "-o", plain], check=True)
    text = open(src, encoding="utf-8", errors="replace").read()
    hop = re.search(r"^#define CW_HOP (\d+)", text, re.M)
    TICK_S = int(hop.group(1)) / 12000.0 if hop else 1024 / cf.FS
    anchor = "  // move to the next position in the symbol buffer\n  p->next_symbol++;\n"
    if anchor not in text:
        sys.exit("cw_bench: cw_rx_add_symbol() isn't as expected; is this mj_decoder?")
    patched = os.path.join(outdir, "modem_cw_trace.c")
    open(patched, "w").write(text.replace(anchor, anchor + TRACE_LINE))
    subprocess.run(cmd + ["-I" + os.path.dirname(src), patched, "-lfftw3", "-lm", "-o", trace],
                   check=True)
    return plain, trace


def run_decoder(exe, samples, wpm):
    r = subprocess.run([exe, str(int(round(wpm))), str(int(cf.PITCH))],
                       input=samples.tobytes(), capture_output=True, check=True)
    return r.stdout.decode(errors="replace"), r.stderr.decode(errors="replace")


def read_wav(path):
    """A mono or stereo (left) WAV at any rate, as the decoder's 96 kHz input."""
    import wave
    import scipy.signal as ss
    with wave.open(path, "rb") as w:
        rate, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(w.getnframes())
    dtype = {2: np.int16, 4: np.int32}[width]
    x = np.frombuffer(raw, dtype).reshape(-1, ch)[:, 0].astype(np.float64)
    if rate != cf.FS:
        x = ss.resample_poly(x, cf.FS, rate)
    x *= cf.AGC_PEAK / max(1.0, np.percentile(np.abs(x), 99.9))
    return np.clip(x, -2**31, 2**31 - 1).astype(np.int32)


# ---------------------------------------------------------------- scoring

def edit_distance(a, b):
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i] + [0] * len(b)
        for j, cb in enumerate(b, 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb))
        prev = cur
    return prev[-1]


def norm(text):
    return re.sub(r"\s+", " ", text.upper()).strip()


def cer(ref, hyp):
    """(characters only, with word breaks), as fractions of the reference."""
    r, h = norm(ref), norm(hyp)
    a = edit_distance(r.replace(" ", ""), h.replace(" ", "")) / max(1, len(r.replace(" ", "")))
    b = edit_distance(r, h) / max(1, len(r))
    return a, b


# ---------------------------------------------------------------- ceiling

def best_threshold(lo, hi):
    """The threshold between two samples of values with the fewest errors."""
    v = np.sort(np.concatenate([lo, hi]))
    cand = (v[:-1] + v[1:]) / 2 if len(v) > 1 else v
    errs = [(np.sum(lo >= c) + np.sum(hi < c)) for c in cand]
    return float(cand[int(np.argmin(errs))])


def ceiling_thresholds(elements):
    """The best fixed dot/dash, character and word thresholds, in units."""
    x = np.array([e[1] / e[2] for e in elements])
    k = np.array([e[3] for e in elements])
    t_mark = best_threshold(x[k == "dot"], x[k == "dash"])
    t_char = best_threshold(x[k == "intra"], x[(k == "char") | (k == "word")])
    t_word = best_threshold(x[(k == "intra") | (k == "char")], x[k == "word"])
    return t_mark, t_char, t_word


def decode_units(seq, thresholds):
    """Text from (is_mark, duration in units) pairs and the thresholds."""
    t_mark, t_char, t_word = thresholds
    out, code = [], ""
    for is_mark, v in seq:
        if is_mark:
            code += "." if v < t_mark else "-"
        elif v >= t_word:
            out.append(cf.DECODE.get(code, "*") + " ")
            code = ""
        elif v >= t_char:
            out.append(cf.DECODE.get(code, "*"))
            code = ""
    out.append(cf.DECODE.get(code, "*"))
    return "".join(out)


def ceiling_decode(elements):
    """Timing-only decode with the true local unit and the best thresholds."""
    seq = [(e[0], e[1] / e[2]) for e in elements]
    return decode_units(seq, ceiling_thresholds(elements))


# ---------------------------------------------------------------- simple detector

def simple_detector(samples, lead_s=1.0):
    """
    A plain detector, to test whether detection is the easy part at good
    SNR: mix the audio down from PITCH, lowpass at 150 Hz, magnitude at a
    1 ms step, and decide mark or gap at half way between a tracked peak
    (learned in marks) and a tracked noise floor (learned in gaps), with
    10% hysteresis. Returns the marks and
    gaps as (is_mark, start_s, seconds), starting at the first mark.
    """
    import scipy.signal as ss
    lp = ss.firwin(1921, 150.0, fs=cf.FS).astype(np.float32)
    step = cf.FS // 1000
    chunk, pad = 10 * cf.FS, 960          # 10 s at a time, in float32
    env = []
    for start in range(0, len(samples), chunk):
        a, b = max(0, start - pad), min(len(samples), start + chunk + pad)
        x = samples[a:b].astype(np.float32) / np.float32(cf.AGC_PEAK)
        ph = 2 * np.pi * cf.PITCH * (np.arange(a, b) % cf.FS) / cf.FS
        zi = ss.oaconvolve(x * np.cos(ph).astype(np.float32), lp, mode="same")
        zq = ss.oaconvolve(x * np.sin(ph).astype(np.float32), lp, mode="same")
        keep = slice(start - a, start - a + min(chunk, len(samples) - start), step)
        env.append(np.hypot(zi[keep], zq[keep]))
    env = np.concatenate(env)
    peak, floor = env[: int(lead_s * 1000)].max() * 2, env[: int(lead_s * 1000)].mean()
    a_peak = np.exp(-1.0 / 500.0)         # in marks, the peak falls with 0.5 s
    a_up, a_down = np.exp(-1.0 / 1000.0), np.exp(-1.0 / 50.0)
    mark = False
    states = np.zeros(len(env), bool)
    for i, e in enumerate(env):
        if e > peak:
            peak = e
        elif mark:                          # the peak only falls in marks
            peak = a_peak * peak + (1 - a_peak) * e
        if not mark:                        # and the floor only learns in gaps
            a = a_down if e < floor else a_up
            floor = a * floor + (1 - a) * e
        mid = floor + 0.5 * (peak - floor)
        hyst = 0.1 * (peak - floor)
        if mark and e < mid - hyst:
            mark = False
        elif not mark and e > mid + hyst:
            mark = True
        states[i] = mark
    edges = np.flatnonzero(np.diff(states.astype(np.int8))) + 1
    out = []
    for a, b in zip(edges[:-1], edges[1:]):
        out.append((bool(states[a]), a / 1000.0, (b - a) / 1000.0))
    return out


def unit_at(elements, lead_s=1.0):
    """A function from time to the sender's dot length then."""
    starts = lead_s + np.concatenate([[0.0], np.cumsum([e[1] for e in elements])[:-1]])
    units = np.array([e[2] for e in elements])
    return lambda t: units[min(len(units) - 1, max(0, np.searchsorted(starts, t) - 1))]


def simple_decode(samples, elements):
    """The simple detector's marks and gaps through the ceiling's classifier."""
    det = simple_detector(samples)
    u = unit_at(elements)
    seq = [(m, d / u(t0)) for m, t0, d in det]
    return decode_units(seq, ceiling_thresholds(elements)), det


# ---------------------------------------------------------------- one run

def one_run(job):
    fist, wpm, farns, snr, seed, exe, wav_dir = job
    rng = np.random.default_rng(seed * 1000 + int(wpm) * 7 + int(snr))
    text = cf.qso_text(rng, CHARS)
    el = cf.key_text(text, fist, wpm, rng, farns)
    x = cf.audio(el, snr, rng)
    if wav_dir:
        tag = f"{fist}_{wpm}" + (f"f{farns}" if farns else "") + f"_{snr}dB_s{seed}"
        cf.write_wav(os.path.join(wav_dir, tag + ".wav"), x)
    res = dict(fist=fist, wpm=wpm, farns=farns, snr=snr, seed=seed)
    sender = farns or wpm
    for label, setting in (("w20", 20), ("wmatch", sender)):
        hyp, _ = run_decoder(exe, x, setting)
        res[label] = cer(text, hyp)
        res[label + "_text"] = hyp.strip()
    res["ceiling"] = cer(text, ceiling_decode(el))
    res["simple"] = cer(text, simple_decode(x, el)[0])
    res["text"] = text
    return res


def detector_run(job):
    snr, seed, exe, tick_s = job
    rng = np.random.default_rng(seed)
    text = cf.qso_text(rng, CHARS)
    el = cf.key_text(text, "machine", 20, rng)
    x = cf.audio(el, snr, rng)
    out, log = run_decoder(exe, x, 20)
    marks, gaps, pending = [], [], None
    for tok in log.split():
        kind, n = tok[0], int(tok[1:])
        if kind == "M":
            if pending is not None:
                gaps.append(pending)
            pending = None
            marks.append(n)
        else:
            pending = n          # the last 's' before a mark is the whole gap
    true_marks = sum(1 for e in el if e[0])
    m = np.array(marks[5:], float) * tick_s * 1000
    g = np.array(gaps[5:], float) * tick_s * 1000
    u = 60.0
    dots = m[m < u * np.sqrt(3)]
    dashes = m[(m >= u * np.sqrt(3)) & (m < u * 5)]
    intra = g[g < u * np.sqrt(3)]
    hyp, det = simple_decode(x, el)
    sm = np.array([d for k, _, d in det if k]) * 1000
    sg = np.array([d for k, _, d in det if not k]) * 1000
    med = lambda v: float(np.median(v)) if len(v) else np.nan
    return dict(snr=snr, seed=seed, found=len(marks) / true_marks,
                dot=med(dots), dash=med(dashes), intra=med(intra),
                long_marks=float(np.mean(m >= u * 5)) if len(m) else np.nan,
                cer=cer(text, out),
                s_found=len(sm) / true_marks, s_dot=med(sm[sm < u * np.sqrt(3)]),
                s_dash=med(sm[(sm >= u * np.sqrt(3)) & (sm < u * 5)]),
                s_intra=med(sg[sg < u * np.sqrt(3)]), s_cer=cer(text, hyp))


# ---------------------------------------------------------------- reports

def report_detector(exe, workers):
    print("\n== detector: KB2ML on machine code, 20 WPM, WPM setting 20 "
          "(truth: dot 60, dash 180, gap 60 ms)\n")
    jobs = [(snr, seed, exe, TICK_S) for snr in (6, 10, 15, 20, 30, 40) for seed in range(1, 13)]
    with ProcessPoolExecutor(workers) as ex:
        rows = list(ex.map(detector_run, jobs))
    print(" KB2ML")
    print(" SNR  marks found  dot ms  dash ms  gap ms  marks >5 units   CER    CER+sp  runs lost")
    for snr in sorted({r["snr"] for r in rows}):
        rs = [r for r in rows if r["snr"] == snr]
        f = lambda k: np.nanmean([r[k] for r in rs])
        c = np.mean([r["cer"][0] for r in rs]) * 100
        cs = np.mean([r["cer"][1] for r in rs]) * 100
        print(f"{snr:4d}  {f('found')*100:9.0f}%  {f('dot'):6.0f}  {f('dash'):7.0f}  "
              f"{f('intra'):6.0f}  {f('long_marks')*100:13.1f}%  {c:5.1f}%  {cs:5.1f}%  "
              f"{sum(r['cer'][0] > 0.10 for r in rs)} of {len(rs)}")
    print("\n simple detector (1 ms step, half-way threshold), its marks and gaps through the"
          "\n ceiling's classifier")
    print(" SNR  marks found  dot ms  dash ms  gap ms   CER")
    for snr in sorted({r["snr"] for r in rows}):
        rs = [r for r in rows if r["snr"] == snr]
        f = lambda k: np.nanmean([r[k] for r in rs])
        c = np.mean([r["s_cer"][0] for r in rs]) * 100
        print(f"{snr:4d}  {f('s_found')*100:9.0f}%  {f('s_dot'):6.0f}  {f('s_dash'):7.0f}  "
              f"{f('s_intra'):6.0f}  {c:5.1f}%")
    print("\n (medians per run, then the mean of the runs; a run is lost when its CER "
          "is over 10%)")


def report_grid(exe, snr, workers, wav_dir):
    print(f"\n== grid: CER at {snr} dB SNR (in 2500 Hz), mean of {len(SEEDS)} runs of "
          f"~{CHARS} characters\n")
    jobs = [(f, w, None, snr, s, exe, wav_dir) for f in GRID_FISTS for w in SPEEDS for s in SEEDS]
    jobs += [(f, w, c, snr, s, exe, wav_dir) for f in ("good", "average")
             for (w, c) in FARNS for s in SEEDS]
    with ProcessPoolExecutor(workers) as ex:
        rows = list(ex.map(one_run, jobs))
    print(" fist       WPM   KB2ML, WPM set 20   KB2ML, WPM set to sender   ceiling  simple")
    print("                  median CER   lost   median CER   lost          mean CER")
    keys = []
    for r in rows:
        k = (r["fist"], r["wpm"], r["farns"])
        if k not in keys:
            keys.append(k)
    for k in keys:
        rs = [r for r in rows if (r["fist"], r["wpm"], r["farns"]) == k]
        def med(lbl):
            return np.median([r[lbl][0] for r in rs]) * 100
        def lost(lbl):
            return sum(r[lbl][0] > 0.10 for r in rs)
        ceil = np.mean([r["ceiling"][0] for r in rs]) * 100
        simp = np.mean([r["simple"][0] for r in rs]) * 100
        sp = f"{k[1]}/{k[2]}" if k[2] else f"{k[1]}"
        print(f" {k[0]:8s} {sp:>6s}  {med('w20'):9.1f}%  {lost('w20'):3d}/{len(rs)}"
              f"  {med('wmatch'):9.1f}%  {lost('wmatch'):3d}/{len(rs)}  {ceil:14.1f}%"
              f"  {simp:5.1f}%")
    for lbl, name in (("w20", "WPM set 20"), ("wmatch", "WPM set to sender")):
        a = np.mean([r[lbl][0] for r in rows]) * 100
        b = np.mean([r[lbl][1] for r in rows]) * 100
        print(f"\n {name}: mean CER over the grid {a:.1f}%, with word breaks {b:.1f}%", end="")
    print("\n (lost: runs with CER over 10%)")
    ex_row = next(r for r in rows if r["fist"] == "average" and r["wpm"] == 20 and not r["farns"])
    print("\n example, average fist, 20 WPM, seed", ex_row["seed"])
    print("  sent    :", ex_row["text"][:150])
    print("  KB2ML@20:", ex_row["w20_text"][:150])


def report_ceiling_only(workers):
    print("\n== ceiling only: timing-only decode with perfect speed tracking, "
          f"best thresholds per run, {len(SEEDS)} runs\n")
    print(" fist      " + "".join(f"{w:>7d}" for w in SPEEDS) + "   (CER %, spaces removed)")
    for fist in GRID_FISTS:
        cells = []
        for w in SPEEDS:
            vals = []
            for s in SEEDS:
                rng = np.random.default_rng(s * 1000 + w * 7 + 15)   # as the grid at 15 dB
                text = cf.qso_text(rng, CHARS)
                el = cf.key_text(text, fist, w, rng)
                vals.append(cer(text, ceiling_decode(el))[0])
            cells.append(np.mean(vals) * 100)
        print(f" {fist:8s} " + "".join(f"{c:7.1f}" for c in cells))


def report_ambiguity():
    rng = np.random.default_rng(7)
    text = cf.qso_text(rng, 20000)
    valid = set(cf.DECODE)
    splits = split_ok = 0
    merges = merge_ok = 0
    split_by_len = {}
    for word in text.split():
        for i, ch in enumerate(word):
            code = cf.MORSE[ch]
            for cut in range(1, len(code)):
                ok = code[:cut] in valid and code[cut:] in valid
                splits += 1
                split_ok += ok
                n, k = split_by_len.get(len(code), (0, 0))
                split_by_len[len(code)] = (n + 1, k + ok)
            if i + 1 < len(word):
                merges += 1
                merge_ok += (code + cf.MORSE[word[i + 1]]) in valid
    print("\n== ambiguity over ~20000 characters of the QSO text\n")
    print(f" split at one gap inside a character -> two valid characters: "
          f"{100*split_ok/splits:.0f}% of {splits} gaps")
    for n in sorted(split_by_len):
        a, b = split_by_len[n]
        print(f"   characters of {n} elements: {100*b/a:5.1f}%")
    print(f" merge two adjacent characters -> one valid character: "
          f"{100*merge_ok/merges:.0f}% of {merges} pairs")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sbitx", help="sbitx src directory (mj_decoder)")
    ap.add_argument("--only", default="detector,grid,ceiling,ambiguity")
    ap.add_argument("--snr", type=float, default=15)
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--wav", default=None, help="directory for the grid's WAVs")
    ap.add_argument("--decode", default=None, metavar="WAV",
                    help="just decode a recording with the KB2ML harness and print it")
    ap.add_argument("--wpm", type=int, default=20, help="WPM setting for --decode")
    a = ap.parse_args()
    parts = set(a.only.split(","))
    if a.wav:
        os.makedirs(a.wav, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        if a.decode:
            plain, _ = build(os.path.expanduser(a.sbitx), tmp)
            print(run_decoder(plain, read_wav(a.decode), a.wpm)[0])
            return
        if parts & {"detector", "grid"}:
            if not a.sbitx:
                sys.exit("cw_bench: --sbitx is needed for detector and grid")
            plain, trace = build(os.path.expanduser(a.sbitx), tmp)
        if "detector" in parts:
            report_detector(trace, a.workers)
        if "grid" in parts:
            report_grid(plain, a.snr, a.workers, a.wav)
        if "ceiling" in parts:
            report_ceiling_only(a.workers)
        if "ambiguity" in parts:
            report_ambiguity()


if __name__ == "__main__":
    main()
