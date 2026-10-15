#!/usr/bin/env python3
"""
cw_fist.py - a model of hand-sent CW for
docs/dsp_design_notes/rx_cw_decoder_study.md: QSO-like text, keyed with a
"fist" (how one operator's element and gap lengths vary), and the audio a
receiver would hear.

A fist is a set of timing statistics, in dot units at the sender's speed:

  dash      mean dash length, and its spread (standard deviation, relative)
  dot_sd    spread of dot lengths
  gaps      mean and spread of the gap inside a character, between
            characters, and between words
  weight    added to every mark and taken from every gap (a heavy fist
            holds the key down longer)
  run, brk  chance a character gap comes out short enough to run two
            characters together, and that a gap inside a character comes
            out long enough to break it in two
  drift     how far the speed wanders, relative, over about a minute
  bug       dots and the gaps after them are machine-made; dashes are
            hand-made
  farns     character speed for Farnsworth spacing (the overall speed is
            the sender's WPM, the gaps between characters and words are
            stretched to reach it, as the ARRL's code practice does)

The presets below are a model, not measurements of real operators; real
recordings are what keep them honest.

Used by cw_bench.py; on its own it writes one WAV:

  python3 tools/cw_study/cw_fist.py --fist average --wpm 20 --snr 15 out.wav
"""
import argparse
import wave

import numpy as np
import scipy.signal as ss

FS = 96000
PITCH = 700.0
RISE_S = 0.005          # raised-cosine edges, 50% points at the element ends
CHANNEL_HZ = 500.0      # the receiver's CW filter
AGC_PEAK = 1.0e8        # what sbitx's rx_linear() hands the decoder

MORSE = {
    "A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.",
    "G": "--.", "H": "....", "I": "..", "J": ".---", "K": "-.-", "L": ".-..",
    "M": "--", "N": "-.", "O": "---", "P": ".--.", "Q": "--.-", "R": ".-.",
    "S": "...", "T": "-", "U": "..-", "V": "...-", "W": ".--", "X": "-..-",
    "Y": "-.--", "Z": "--..",
    "0": "-----", "1": ".----", "2": "..---", "3": "...--", "4": "....-",
    "5": ".....", "6": "-....", "7": "--...", "8": "---..", "9": "----.",
    "/": "-..-.", "?": "..--..",
}
DECODE = {v: k for k, v in MORSE.items()}

FISTS = {
    #           dash, dash_sd, dot_sd, g1, g1_sd, g3, g3_sd, g7, g7_sd, weight, run, brk, drift
    "machine": dict(dash=3.0, dash_sd=0.0, dot_sd=0.0, g1=1.0, g1_sd=0.0, g3=3.0, g3_sd=0.0,
                    g7=7.0, g7_sd=0.0, weight=0.0, run=0.0, brk=0.0, drift=0.0),
    "good":    dict(dash=3.0, dash_sd=0.10, dot_sd=0.10, g1=1.0, g1_sd=0.15, g3=3.0, g3_sd=0.15,
                    g7=7.0, g7_sd=0.20, weight=0.0, run=0.0, brk=0.0, drift=0.03),
    "average": dict(dash=2.8, dash_sd=0.15, dot_sd=0.15, g1=1.1, g1_sd=0.20, g3=3.0, g3_sd=0.20,
                    g7=6.5, g7_sd=0.25, weight=0.1, run=0.02, brk=0.01, drift=0.05),
    "poor":    dict(dash=2.5, dash_sd=0.20, dot_sd=0.20, g1=1.2, g1_sd=0.25, g3=2.8, g3_sd=0.25,
                    g7=6.0, g7_sd=0.30, weight=0.2, run=0.05, brk=0.03, drift=0.08),
    "heavy":   dict(dash=3.3, dash_sd=0.12, dot_sd=0.12, g1=1.0, g1_sd=0.20, g3=3.0, g3_sd=0.20,
                    g7=7.0, g7_sd=0.25, weight=0.4, run=0.02, brk=0.0, drift=0.04),
    "bug":     dict(dash=3.8, dash_sd=0.20, dot_sd=0.03, g1=1.0, g1_sd=0.05, g3=3.2, g3_sd=0.25,
                    g7=7.0, g7_sd=0.30, weight=0.0, run=0.03, brk=0.0, drift=0.03, bug=True),
}

CALLS = ("KB2ML", "W2JON", "N1MM", "K3LR", "W1AW", "VE3NEA", "G4ABC", "DL1XYZ",
         "AC9TU", "W9JES", "KD2QRP", "N0POT", "WA6XYZ", "K4BUG")
NAMES = ("MIKE", "JOHN", "ED", "BOB", "SUE", "AL", "PETE", "ANN")
QTHS = ("NY", "ALBANY", "OHIO", "TX", "BOSTON", "MAINE", "VA", "ONT")
RSTS = ("599", "579", "559", "449", "5NN", "339")
PARKS = ("K1234", "K0567", "VE0123", "K4321")


def qso_text(rng, n_chars):
    """QSO-like text, about n_chars long, upper case, single spaces."""
    lines = []
    while sum(len(x) + 1 for x in lines) < n_chars:
        a, b = rng.choice(CALLS, 2, replace=False)
        kind = rng.integers(0, 7)
        if kind == 0:
            lines.append(f"CQ CQ POTA DE {a} {a} K")
        elif kind == 1:
            lines.append(f"{a} DE {b} GM TNX CALL UR {rng.choice(RSTS)} {rng.choice(RSTS)} "
                         f"IN {rng.choice(QTHS)} NAME {rng.choice(NAMES)} HW?")
        elif kind == 2:
            lines.append(f"R TU UR {rng.choice(RSTS)} {rng.choice(QTHS)} BK")
        elif kind == 3:
            lines.append(f"{b} DE {a} TU 5NN {rng.choice(PARKS)} K")
        elif kind == 4:
            lines.append(f"RIG SBITX ANT DIPOLE PWR 5W WX SUNNY 73 ES GL")
        elif kind == 5:
            lines.append(f"QRL? QRZ? {a}/P {a}/P")
        else:
            lines.append(f"NAME {rng.choice(NAMES)} QTH {rng.choice(QTHS)} FB OM TNX FER QSO")
    return " ".join(lines)


def _draw(rng, mean, sd, floor=0.3):
    """A duration in units: mean times a lognormal spread of sd."""
    if sd <= 0:
        return mean
    return max(floor, mean * float(np.exp(rng.normal(0.0, sd) - sd * sd / 2)))


def key_text(text, fist, wpm, rng, farns=None):
    """
    The text as a list of elements (is_mark, seconds, unit_seconds, kind),
    kind one of dot, dash, intra, char, word; unit_seconds is the sender's
    dot length at that moment (the truth a perfect speed tracker would
    know).
    """
    f = dict(FISTS[fist]) if isinstance(fist, str) else dict(fist)
    char_wpm = farns or wpm
    u0 = 1.2 / char_wpm
    # Farnsworth (ARRL): a standard word's 19 units of spacing between
    # characters and words take ta seconds instead of 19 units, so those
    # gaps are stretched by k
    k = 1.0
    if farns and farns > wpm:
        ta = (60.0 * farns - 37.2 * wpm) / (wpm * farns)
        k = (ta / 19.0) / u0
    drift_rate = f["drift"] / np.sqrt(60.0)                   # per sqrt(second)
    log_u = 0.0
    t = 0.0
    out = []
    w = f["weight"]
    bug = f.get("bug", False)

    def unit():
        return u0 * float(np.exp(log_u))

    def add(is_mark, units, kind):
        nonlocal t, log_u
        u = unit()
        s = units * u
        out.append((is_mark, s, u, kind))
        t += s
        if drift_rate > 0:
            log_u += rng.normal(0.0, drift_rate * np.sqrt(s))
            log_u = float(np.clip(log_u, -0.25, 0.25))

    words = text.split(" ")
    for wi, word in enumerate(words):
        for ci, ch in enumerate(word):
            code = MORSE[ch]
            for ei, el in enumerate(code):
                if el == ".":
                    add(True, _draw(rng, 1.0, f["dot_sd"]) + w, "dot")
                else:
                    add(True, _draw(rng, f["dash"], f["dash_sd"]) + w, "dash")
                if ei < len(code) - 1:
                    sd = f["g1_sd"]
                    if bug and el == "-":
                        sd = 0.20                             # hand-made after a dash
                    g = _draw(rng, f["g1"], sd) - w
                    if f["brk"] and rng.random() < f["brk"]:
                        g = _draw(rng, 2.6, 0.15) - w
                    add(False, max(0.3, g), "intra")
            if ci < len(word) - 1:
                g = _draw(rng, f["g3"] * k, f["g3_sd"]) - w
                if f["run"] and rng.random() < f["run"]:
                    g = _draw(rng, 1.6, 0.15) - w
                add(False, max(0.5, g), "char")
        if wi < len(words) - 1:
            add(False, max(1.0, _draw(rng, f["g7"] * k, f["g7_sd"]) - w), "word")
    return out


def audio(elements, snr_db, rng, lead_s=1.0, tail_s=1.5):
    """
    The keyed tone at PITCH in white noise at snr_db (dB in 2500 Hz),
    through a CHANNEL_HZ bandpass, scaled so the tone peaks near AGC_PEAK;
    int32, FS.
    """
    total = lead_s + sum(e[1] for e in elements) + tail_s
    n = int(total * FS)
    env = np.zeros(n, np.float32)
    rise = int(RISE_S * FS)
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(rise) / rise)
    t = lead_s
    for is_mark, s, _, _ in elements:
        if is_mark:
            a = int((t - RISE_S / 2) * FS)
            b = int((t + s - RISE_S / 2) * FS)
            env[a:a + rise] = ramp
            env[a + rise:b] = 1.0
            env[b:b + rise] = ramp[::-1]
        t += s
    tone = env * np.sin(2 * np.pi * PITCH * np.arange(n) / FS).astype(np.float32)
    sigma = np.sqrt(0.5 / (10 ** (snr_db / 10)) * (FS / 2) / 2500.0)
    x = tone + np.float32(sigma) * rng.standard_normal(n, dtype=np.float32)
    taps = ss.firwin(4001, [PITCH - CHANNEL_HZ / 2, PITCH + CHANNEL_HZ / 2],
                     pass_zero=False, fs=FS)
    y = ss.oaconvolve(x, taps.astype(np.float32), mode="same")
    return np.clip(y * AGC_PEAK, -2**31, 2**31 - 1).astype(np.int32)


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes((samples / AGC_PEAK * 0.5 * 32767).astype(np.int16).tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--fist", default="average", choices=sorted(FISTS))
    ap.add_argument("--wpm", type=float, default=20)
    ap.add_argument("--farns", type=float, default=None, help="character speed, WPM")
    ap.add_argument("--snr", type=float, default=15)
    ap.add_argument("--chars", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("out")
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    text = qso_text(rng, a.chars)
    el = key_text(text, a.fist, a.wpm, rng, a.farns)
    write_wav(a.out, audio(el, a.snr, rng))
    print(text)


if __name__ == "__main__":
    main()
