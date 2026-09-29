#!/usr/bin/env python3
"""envelope_study.py - keying-envelope measurements behind
docs/dsp_design_notes/cw_keyer_design_study.md §3, §4 and §17.

  python3 tools/keyer_study/envelope_study.py [--ref PATH] [--limiter]

Measures src/cw.c's Blackman-Harris table: where its 50% point falls, the
weighting it produces when driven the way cw_get_sample() drives it, the
compensation that restores 1:1, and the occupied bandwidth of the
alternatives. --ref adds the reference keyer's table (sbitx
dev-54bugfixes src/modem_cw.c). --limiter builds limiter_harness.c against
src/ (needs gcc and fftw3f) and checks what tx_pipeline.c's limiter does to
the keyed envelope. Needs numpy and scipy.
"""
import argparse
import math
import os
import re
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FS = 96000.0
BH = (0.35875, 0.48829, 0.14128, 0.01168)  # 4-term Blackman-Harris


def bh_window(L):
    x = 2 * math.pi * np.arange(L) / (L - 1)
    return BH[0] - BH[1] * np.cos(x) + BH[2] * np.cos(2 * x) - BH[3] * np.cos(3 * x)


def integrated_bh(L):
    """An antisymmetric edge: the running integral of a BH window. Crosses
    50% at its midpoint by construction."""
    e = np.cumsum(bh_window(L))
    return e / e[-1]


def load_table(path, pattern, declared=None):
    text = open(path).read()
    m = re.search(pattern + r"\s*=\s*\{(.*?)\};", text, re.S)
    vals = [float(v) for v in re.findall(r"-?\d+\.\d+(?:[eE]-?\d+)?", m.group(1))]
    if declared is not None and len(vals) < declared:
        vals += [0.0] * (declared - len(vals))  # C zero-fills the remainder
    return np.array(vals)


def cross(v, level):
    for i in range(len(v) - 1):
        if v[i] < level <= v[i + 1]:
            return i + (level - v[i]) / (v[i + 1] - v[i])
    return float("nan")


def env_maxibitx(table, key):
    """cw_get_sample(): one step per sample toward the key state, clamped,
    read after the step."""
    top, pos = len(table) - 1, 0
    out = np.empty(len(key))
    for i, k in enumerate(key):
        pos = min(pos + 1, top) if k else max(pos - 1, 0)
        out[i] = table[pos]
    return out


def env_reference(table, key):
    """modem_cw.c's cw_tx_get_sample(): rise reads data[pos++] while
    pos < 480 then holds 1.0; fall pre-decrements and reads data[pos]."""
    pos, out = 0, np.empty(len(key))
    for i, k in enumerate(key):
        if k:
            if pos < 480:
                e = table[pos]
                pos += 1
            else:
                e = 1.0
        elif pos > 0:
            pos -= 1
            e = table[pos]
        else:
            e = 0.0
        out[i] = e
    return out


def dit_train(wpm, n_dits, lead_dits=2, extend=0):
    """Continuous dits; extend lengthens every mark (and shortens the
    following space) by that many samples."""
    dit = int(round(FS * 1.2 / wpm))
    key = np.zeros(dit * (2 * n_dits + 2 * lead_dits), dtype=bool)
    for d in range(n_dits):
        s = dit * (lead_dits + 2 * d)
        key[s:s + dit + extend] = True
    return key, dit


def marks_spaces(env, level=0.5):
    above = env >= level
    idx = np.flatnonzero(np.diff(above.astype(int)))
    t = np.array([i + (level - env[i]) / (env[i + 1] - env[i]) for i in idx])
    ups, downs = t[0::2], t[1::2]
    n = min(len(ups), len(downs))
    return np.median(downs[:n] - ups[:n]), np.median(ups[1:n] - downs[:n - 1])


def occupied_bw(sig, level_db):
    """Span of every spectral component above level_db re the peak."""
    spec = np.abs(np.fft.rfft(sig * bh_window(len(sig))))
    db = 20 * np.log10(spec / spec.max() + 1e-30)
    f = np.fft.rfftfreq(len(sig), 1 / FS)
    k = np.flatnonzero(db > level_db)
    return f[k].max() - f[k].min()


def keyed_bw(env):
    tone = np.cos(2 * math.pi * 700.0 * np.arange(len(env)) / FS)
    return [occupied_bw(env * tone, lv) for lv in (-40, -60, -80)]


def ms(samples):
    return samples / FS * 1e3


def study(cur, ref):
    anti = integrated_bh(len(cur))
    print("== Table shapes ==")
    shapes = [("maxibitx src/cw.c", cur), ("integrated BH, same length", anti)]
    if ref is not None:
        shapes.insert(1, ("reference modem_cw.c", ref[:int(np.count_nonzero(ref)) + 1]))
    for name, v in shapes:
        n = len(v)
        print(f"  {name:<28} len {n}  50% at {cross(v, .5):6.1f} of {n - 1} "
              f"(midpoint {(n - 1) / 2:.1f})  10-90% {ms(cross(v, .9) - cross(v, .1)):.3f} ms")
    if ref is not None:
        print(f"  reference array: {len(ref)} declared, {int(np.count_nonzero(ref))} nonzero, "
              f"last element {ref[-1]}")

    ext = int(round(cross(cur, .5) - (len(cur) - 1 - cross(cur, .5))))
    print(f"\n== Weighting at the 50% points, continuous dits ==")
    print(f"  compensation derived from the table: {ext} samples ({ms(ext):.3f} ms)")
    print(f"  {'WPM':>4} {'dit':>7} | {'as today':^22} | {'+ compensation':^22} | {'integrated BH':^22}")
    for wpm in (1, 5, 10, 20, 30, 40, 50, 60):
        row = f"  {wpm:>4} {ms(round(FS * 1.2 / wpm)):6.1f}ms"
        for table, extend in ((cur, 0), (cur, ext), (anti, 0)):
            key, _ = dit_train(wpm, 4, extend=extend)
            m, s = marks_spaces(env_maxibitx(table, key))
            row += f" | mark {ms(m):8.2f} ms {100 * m / (m + s):5.1f}%"
        print(row)

    print("\n== Occupied bandwidth, 30 WPM continuous dits at 700 Hz (-40/-60/-80 dBc) ==")
    key, _ = dit_train(30, 40, lead_dits=4)
    key_c, _ = dit_train(30, 40, lead_dits=4, extend=ext)
    rows = [("hard keyed", key.astype(float)),
            ("as today", env_maxibitx(cur, key)),
            ("as today + compensation", env_maxibitx(cur, key_c))]
    rows.append(("integrated BH, same 5 ms", env_maxibitx(anti, key)))
    # An integrated BH stretched until its 10-90% rise matches the table's.
    target = cross(cur, .9) - cross(cur, .1)
    L = min(range(len(cur), 1000),
            key=lambda L: abs((lambda e: cross(e, .9) - cross(e, .1))(integrated_bh(L)) - target))
    rows.append((f"integrated BH, {ms(L):.2f} ms", env_maxibitx(integrated_bh(L), key)))
    if ref is not None:
        rows.append(("reference, as written", env_reference(ref, key)))
        fixed = ref.copy()
        fixed[-1] = 1.0
        rows.append(("reference, last value 1.0", env_reference(fixed, key)))
    for name, e in rows:
        print(f"  {name:<28} " + "  ".join(f"{b:6.0f} Hz" for b in keyed_bw(e)))
    if ref is not None:
        k1, dit = dit_train(30, 1, lead_dits=1)
        e = env_reference(ref, k1)
        print(f"\n  reference envelope at the top of the rise (samples 476-482): "
              f"{np.round(e[dit + 476:dit + 483], 4).tolist()}")
        print(f"  and the first four samples after key-up: {np.round(e[2 * dit:2 * dit + 4], 4).tolist()}")


def limiter(cur):
    from scipy.signal import hilbert
    src = os.path.join(ROOT, "src")
    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, "cw_table.h"), "w") as f:
            f.write("#define ENV_LEN %d\nstatic const double env_table[ENV_LEN] = {%s};\n"
                    % (len(cur), ",".join(repr(float(v)) for v in cur)))
        exe = os.path.join(tmp, "limiter_harness")
        subprocess.run(["gcc", "-O2", "-std=gnu11", "-I" + src, "-I" + tmp,
                        os.path.join(src, "fft_filter.c"), os.path.join(src, "tx_pipeline.c"),
                        os.path.join(HERE, "limiter_harness.c"), "-o", exe, "-lfftw3f", "-lm"],
                       check=True)
        print("\n== Through the real tx_pipeline.c, 30 WPM dits (40.00 ms) ==")
        for c in (1.0, 0.707, 0.5, 0.316):
            out = os.path.join(tmp, "out.f32")
            subprocess.run([exe, "30", str(c), out], check=True, capture_output=True)
            x = np.fromfile(out, dtype=np.float32).astype(float)
            env = np.abs(hilbert(x))
            peak = np.percentile(env, 99.5)
            m, s = marks_spaces(env, 0.5 * peak)
            above = env >= 0.5 * peak
            idx = np.flatnonzero(np.diff(above.astype(int)))
            first = ms(idx[1] - idx[0])
            bws = [occupied_bw(x, lv) for lv in (-40, -60, -80)]
            print(f"  ceiling {c:5.3f}: mark {ms(m):6.2f} ms (first element {first:6.2f})  "
                  f"space {ms(s):6.2f} ms  BW " + "/".join(f"{b:.0f}" for b in bws) + " Hz")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cw", default=os.path.join(ROOT, "src", "cw.c"))
    ap.add_argument("--ref", help="sbitx dev-54bugfixes src/modem_cw.c")
    ap.add_argument("--limiter", action="store_true")
    a = ap.parse_args()
    cur = load_table(a.cw, r"cw_envelope\[CW_ENVELOPE_LEN\]")
    ref = load_table(a.ref, r"cw_envelope_data\[480\]", declared=480) if a.ref else None
    study(cur, ref)
    if a.limiter:
        limiter(cur)


if __name__ == "__main__":
    sys.exit(main())
