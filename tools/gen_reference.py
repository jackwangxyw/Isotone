# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
"""Generate frequency-response reference data for the core test suite.

The point of this script is independence. It derives biquad coefficients from
the RBJ Audio EQ Cookbook formulas written out here separately from the C++, and
evaluates the response with scipy.signal.freqz rather than with our own complex
arithmetic. If core/src/biquad.cpp and this file agree to 0.01 dB, two
independent implementations of the same maths agree, which is meaningful
evidence. If they were the same code twice, it would not be.

Output: core/tests/reference/*.txt, checked in, so the C++ tests need no Python
at run time and CI stays hermetic.

Usage:  python tools/gen_reference.py
"""

from __future__ import annotations

import math
import pathlib

import numpy as np
from scipy import signal

OUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "core" / "tests" / "reference"

# Sample rates the core must handle (plan 4.3 / stage 2 acceptance).
SAMPLE_RATES = [44100.0, 48000.0, 96000.0, 192000.0]

# Grid: 256 log-spaced points. The upper limit is tied to the rate so we never
# evaluate above nyquist.
GRID_POINTS = 256
GRID_LO = 10.0


def alpha_from_q(sin_w0: float, q: float) -> float:
    return sin_w0 / (2.0 * q)


def alpha_from_bw(sin_w0: float, w0: float, bw_oct: float) -> float:
    return sin_w0 * math.sinh(math.log(2.0) / 2.0 * bw_oct * w0 / sin_w0)


def alpha_from_slope(sin_w0: float, slope_db: float, amp: float) -> float:
    s = slope_db / 12.0
    return sin_w0 / 2.0 * math.sqrt((amp + 1.0 / amp) * (1.0 / s - 1.0) + 2.0)


def shelf_corner_freq(ftype: str, fc: float, gain_db: float, width: float, mode: str,
                      amp: float) -> float:
    """Equalizer APO's LS/HS design-frequency shift (the DCX2496 correction)."""
    if mode == "bw":
        return fc
    if mode == "q":
        s = 1.0 / ((1.0 / (width * width) - 2.0) / (amp + 1.0 / amp) + 1.0)
    else:
        s = width / 12.0
    if s == 0.0 or not math.isfinite(s):
        return fc
    factor = 10.0 ** (abs(gain_db) / 80.0 / s)
    return fc * factor if ftype == "low_shelf" else fc / factor


def design(ftype: str, fc: float, gain_db: float, width: float, mode: str, fs: float,
           shelf_corner: bool) -> tuple[list[float], list[float]]:
    """Return ([b0, b1, b2], [1, a1, a2]) normalised by a0."""
    gain_type = ftype in ("peaking", "low_shelf", "high_shelf")
    amp = 10.0 ** (gain_db / 40.0) if gain_type else 10.0 ** (gain_db / 20.0)

    fc = min(max(fc, 10.0), fs * 0.5 * 0.95)
    if shelf_corner and ftype in ("low_shelf", "high_shelf"):
        fc = shelf_corner_freq(ftype, fc, gain_db, width, mode, amp)
        fc = min(max(fc, 10.0), fs * 0.5 * 0.95)

    w0 = 2.0 * math.pi * fc / fs
    sn, cs = math.sin(w0), math.cos(w0)

    if mode == "q":
        alpha = alpha_from_q(sn, width)
    elif mode == "bw":
        alpha = alpha_from_bw(sn, w0, width)
    elif mode == "slope":
        if ftype in ("low_shelf", "high_shelf"):
            alpha = alpha_from_slope(sn, width, amp)
        else:
            alpha = alpha_from_q(sn, width)
    else:
        raise ValueError(f"unknown width mode {mode}")

    beta = 2.0 * math.sqrt(amp) * alpha
    a_ = amp

    if ftype == "low_pass":
        b = [(1 - cs) / 2, 1 - cs, (1 - cs) / 2]
        a = [1 + alpha, -2 * cs, 1 - alpha]
    elif ftype == "high_pass":
        b = [(1 + cs) / 2, -(1 + cs), (1 + cs) / 2]
        a = [1 + alpha, -2 * cs, 1 - alpha]
    elif ftype == "band_pass":
        b = [alpha, 0.0, -alpha]
        a = [1 + alpha, -2 * cs, 1 - alpha]
    elif ftype == "notch":
        b = [1.0, -2 * cs, 1.0]
        a = [1 + alpha, -2 * cs, 1 - alpha]
    elif ftype == "all_pass":
        b = [1 - alpha, -2 * cs, 1 + alpha]
        a = [1 + alpha, -2 * cs, 1 - alpha]
    elif ftype == "peaking":
        b = [1 + alpha * a_, -2 * cs, 1 - alpha * a_]
        a = [1 + alpha / a_, -2 * cs, 1 - alpha / a_]
    elif ftype == "low_shelf":
        b = [a_ * ((a_ + 1) - (a_ - 1) * cs + beta),
             2 * a_ * ((a_ - 1) - (a_ + 1) * cs),
             a_ * ((a_ + 1) - (a_ - 1) * cs - beta)]
        a = [(a_ + 1) + (a_ - 1) * cs + beta,
             -2 * ((a_ - 1) + (a_ + 1) * cs),
             (a_ + 1) + (a_ - 1) * cs - beta]
    elif ftype == "high_shelf":
        b = [a_ * ((a_ + 1) + (a_ - 1) * cs + beta),
             -2 * a_ * ((a_ - 1) + (a_ + 1) * cs),
             a_ * ((a_ + 1) + (a_ - 1) * cs - beta)]
        a = [(a_ + 1) - (a_ - 1) * cs + beta,
             2 * ((a_ - 1) - (a_ + 1) * cs),
             (a_ + 1) - (a_ - 1) * cs - beta]
    else:
        raise ValueError(f"unknown filter type {ftype}")

    a0 = a[0]
    return [x / a0 for x in b], [1.0, a[1] / a0, a[2] / a0]


# (name, type, fc, gain_db, width, mode, shelf_corner)
CASES = [
    ("pk_1k_p6_q1",        "peaking",    1000.0,   6.0, 1.0,   "q",     False),
    ("pk_1k_m12_q1",       "peaking",    1000.0, -12.0, 1.0,   "q",     False),
    ("pk_100_p3_q4",       "peaking",     100.0,   3.0, 4.0,   "q",     False),
    ("pk_10k_m6_q0p5",     "peaking",   10000.0,  -6.0, 0.5,   "q",     False),
    ("pk_30_p10_q10",      "peaking",      30.0,  10.0, 10.0,  "q",     False),
    ("pk_1k_p6_bw1",       "peaking",    1000.0,   6.0, 1.0,   "bw",    False),
    ("pk_1k_p6_bw0p25",    "peaking",    1000.0,   6.0, 0.25,  "bw",    False),
    ("lp_1k_q0p7071",      "low_pass",   1000.0,   0.0, 0.7071067811865476, "q", False),
    ("hp_100_q0p7071",     "high_pass",   100.0,   0.0, 0.7071067811865476, "q", False),
    ("lp_5k_q2",           "low_pass",   5000.0,   0.0, 2.0,   "q",     False),
    ("hp_50_q0p5",         "high_pass",    50.0,   0.0, 0.5,   "q",     False),
    ("bp_1k_q1",           "band_pass",  1000.0,   0.0, 1.0,   "q",     False),
    ("bp_500_q3",          "band_pass",   500.0,   0.0, 3.0,   "q",     False),
    ("no_60_q30",          "notch",        60.0,   0.0, 30.0,  "q",     False),
    ("ap_1k_q1",           "all_pass",   1000.0,   0.0, 1.0,   "q",     False),
    # AutoEq emits LSC/HSC with Q 0.70, i.e. shelf_corner False.
    ("lsc_105_p6p4_q0p7",  "low_shelf",   105.0,   6.4, 0.70,  "q",     False),
    ("hsc_10k_m2p1_q0p7",  "high_shelf",10000.0,  -2.1, 0.70,  "q",     False),
    ("lsc_200_m6_q1",      "low_shelf",   200.0,  -6.0, 1.0,   "q",     False),
    # Peace emits LS/HS with no Q, i.e. default slope and shelf_corner True.
    ("ls_1100_m3_corner",  "low_shelf",  1100.0,  -3.0, 0.9,   "slope", True),
    ("hs_3000_p4_corner",  "high_shelf", 3000.0,   4.0, 0.9,   "slope", True),
    ("ls_100_p6_slope6",   "low_shelf",   100.0,   6.0, 6.0,   "slope", True),
    ("hsc_8k_p5_slope12",  "high_shelf", 8000.0,   5.0, 12.0,  "slope", False),
]


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    written = 0

    for fs in SAMPLE_RATES:
        f_hi = fs * 0.5 * 0.95
        freqs = np.logspace(math.log10(GRID_LO), math.log10(f_hi), GRID_POINTS)
        cases = []

        for name, ftype, fc, gain, width, mode, corner in CASES:
            b, a = design(ftype, fc, gain, width, mode, fs, corner)
            # freqz evaluates H(e^{jw}) on the unit circle. worN in Hz with fs
            # given avoids any hand-rolled radian conversion.
            _, h = signal.freqz(b, a, worN=freqs, fs=fs)
            mag_db = 20.0 * np.log10(np.abs(h))
            phase = np.degrees(np.angle(h))
            cases.append({
                "name": name,
                "type": ftype,
                "fc": fc,
                "gain_db": gain,
                "width": width,
                "width_mode": mode,
                "shelf_corner": corner,
                "coeffs": {"b0": b[0], "b1": b[1], "b2": b[2], "a1": a[1], "a2": a[2]},
                "magnitude_db": [float(v) for v in mag_db],
                "phase_deg": [float(v) for v in phase],
            })

        # Flat whitespace-separated text, so the C++ tests can read it with
        # ifstream >> and the repo needs no JSON dependency. %.17g round-trips a
        # double exactly.
        path = OUT_DIR / f"response_{int(fs)}.txt"
        with path.open("w", encoding="utf-8", newline="\n") as fh:
            fh.write("# isotone reference data, generated by tools/gen_reference.py\n")
            fh.write(f"# scipy {__import__('scipy').__version__}\n")
            fh.write("version 1\n")
            fh.write(f"sample_rate {fs:.17g}\n")
            fh.write(f"points {GRID_POINTS}\n")
            fh.write(f"cases {len(cases)}\n")
            fh.write("freqs\n")
            fh.write(" ".join(f"{v:.17g}" for v in freqs) + "\n")
            for c in cases:
                fh.write(f"case {c['name']} {c['type']} {c['fc']:.17g} {c['gain_db']:.17g} "
                         f"{c['width']:.17g} {c['width_mode']} {int(c['shelf_corner'])}\n")
                k = c["coeffs"]
                fh.write(f"coeffs {k['b0']:.17g} {k['b1']:.17g} {k['b2']:.17g} "
                         f"{k['a1']:.17g} {k['a2']:.17g}\n")
                fh.write("magnitude_db\n")
                fh.write(" ".join(f"{v:.17g}" for v in c["magnitude_db"]) + "\n")
                fh.write("phase_deg\n")
                fh.write(" ".join(f"{v:.17g}" for v in c["phase_deg"]) + "\n")
        written += 1
        print(f"wrote {path.name}: {len(cases)} cases x {GRID_POINTS} points")

    print(f"{written} reference files in {OUT_DIR}")


if __name__ == "__main__":
    main()
