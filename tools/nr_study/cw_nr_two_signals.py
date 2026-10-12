#!/usr/bin/env python3
"""
cw_nr_two_signals.py - the two-signal check in
docs/dsp_design_notes/rx_cw_noise_reduction_study.md (§7).

Two keyed stations inside stage 3's 300 Hz: A at the 700 Hz pitch, -3 dB
SNR in 2500 Hz, and B 100 Hz above it at 0, -6 and -12 dB relative to A,
sending different text. Each output is listened to "at B" through a 60 Hz
analysis bandpass (zero phase, analysis only) and B's dit slots scored as
cw_nr_bench.py scores them. Prints B's slot errors and B's level relative
to stage 3 alone, for stage 3 alone, the line enhancer (lms) and the
128-point spectral NR (stft128), averaged over two noise draws.

  python3 tools/nr_study/cw_nr_two_signals.py     # about 2 min

Needs numpy, scipy and cw_nr_bench.py beside it.
"""
import os
import sys

import numpy as np
import scipy.signal as ss

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cw_nr_bench as b  # noqa: E402

N_SAMPLES = int(b.SECONDS * b.FS)
B_OFFSET_HZ = 100.0


def keyed(text, pitch):
    slots = b.keying(text, b.WPM)
    saved, b.PITCH = b.PITCH, pitch
    sig, unit = b.cw_signal(slots, b.WPM, N_SAMPLES)
    b.PITCH = saved
    return sig, slots[: N_SAMPLES // unit], unit


def main():
    a_sig, _, unit = keyed("CQ POTA DE KB2ML KB2ML K " * 4, b.PITCH)
    b_sig, b_slots, _ = keyed("TEST W1AW W1AW TEST " * 5, b.PITCH + B_OFFSET_HZ)
    H, h_delay = b.stage3_response()
    bins = b.passband_bins(128, b.FS_LO)
    bias = b.calibrate_bias(lambda: b.SpectralNR(bins, 32 / b.FS_LO, 0.80, -15),
                            lambda x, f: b.stft_nr(x, f, 128, 32))
    f_b = b.PITCH + B_OFFSET_HZ
    sos = ss.butter(4, [f_b - 30, f_b + 30], "bandpass", fs=b.FS, output="sos")

    def score(y, ref):
        yb, rb = ss.sosfiltfilt(sos, y), ss.sosfiltfilt(sos, ref)
        lag = b.delay_of(yb, rb) + h_delay
        e = b.slot_energies(yb, b_slots, unit, lag)
        return 100 * b.slot_errors(e, b_slots), 10 * np.log10(np.nanmean(e[b_slots == 1]))

    print(f"A at {b.PITCH:.0f} Hz, -3 dB SNR; B at {f_b:.0f} Hz; {b.WPM} WPM; "
          f"B's errors and B's level against stage 3 alone\n")
    for rel_db in (0, -6, -12):
        res = {}
        for _ in range(2):
            gain_b = 10 ** (rel_db / 20)
            x = a_sig + gain_b * b_sig + b.noise_for_snr(N_SAMPLES, -3)
            ref, _ = b.overlap_save(gain_b * b_sig, H)
            y0, _ = b.overlap_save(x, H)
            nr = b.SpectralNR(bins, 32 / b.FS_LO, 0.80, -15)
            nr.bias = bias
            for name, y in (("none", y0), ("lms", b.lms(y0)), ("stft128", b.stft_nr(y0, nr, 128, 32))):
                res.setdefault(name, []).append(score(y, ref))
        base = np.mean([r[1] for r in res["none"]])
        cells = [f"{k:8s} {np.mean([r[0] for r in v]):5.1f}% {np.mean([r[1] for r in v]) - base:+5.1f} dB"
                 for k, v in res.items()]
        print(f"B {-rel_db:2d} dB below A:  " + "   ".join(cells))


if __name__ == "__main__":
    main()
