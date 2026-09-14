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

Usage:
    python tools/gen_reference.py            regenerate the checked-in files
    python tools/gen_reference.py --check    verify them without writing

--check compares numerically with a tolerance rather than byte for byte. Two
scipy or libm versions disagree in the last bit or two of a 17-digit double, so
a textual diff fails on a different machine for reasons that say nothing about
correctness. A 1e-9 dB tolerance is still five orders of magnitude tighter than
the 0.01 dB the C++ tests assert, so a real change in the maths cannot slip past.
"""

from __future__ import annotations

import math
import pathlib
import sys

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
    # A dB slope with the corner shift: LS/HS with a slope before Fc. A slope of
    # 0.9 dB is S = 0.075, a very gentle shelf, not upstream's no-width default.
    ("ls_1100_m3_corner",  "low_shelf",  1100.0,  -3.0, 0.9,   "slope", True),
    ("hs_3000_p4_corner",  "high_shelf", 3000.0,   4.0, 0.9,   "slope", True),
    ("ls_100_p6_slope6",   "low_shelf",   100.0,   6.0, 6.0,   "slope", True),
    ("hsc_8k_p5_slope12",  "high_shelf", 8000.0,   5.0, 12.0,  "slope", False),
    # Peace emits LS/HS with no width. Upstream then uses S = 0.9 directly, with
    # no division by 12 and no corner shift; the parser stores that as a 10.8 dB
    # slope without the corner flag.
    ("ls_1100_m3_nowidth", "low_shelf",  1100.0,  -3.0, 10.8,  "slope", False),
    ("hs_3000_p4_nowidth", "high_shelf", 3000.0,   4.0, 10.8,  "slope", False),
    # LS/HS with a Q: the corner shift through the Q to S conversion.
    ("ls_300_p6_q0p7_corner", "low_shelf",  300.0,  6.0, 0.7,  "q",     True),
    ("hs_4k_m4_q1_corner",    "high_shelf", 4000.0, -4.0, 1.0, "q",     True),
]


# Agreement required between a regenerated value and the checked-in one. Not
# zero: see the note in the module docstring.
CHECK_TOLERANCE = 1e-9


def parse_reference(text: str) -> dict:
    """Reads a reference file's text back into its header, grid and cases."""
    tokens = []
    for line in text.splitlines():
        if line.startswith("#"):
            continue
        tokens.extend(line.split())

    header, freqs, cases, i = {}, [], {}, 0
    while i < len(tokens):
        tok = tokens[i]
        if tok in ("version", "sample_rate", "points", "cases"):
            header[tok] = float(tokens[i + 1]); i += 2
        elif tok == "freqs":
            points = int(header["points"])
            freqs = [float(v) for v in tokens[i + 1:i + 1 + points]]
            i += 1 + points
        elif tok == "case":
            points = int(header["points"])
            # case <name> <type> <fc> <gain> <width> <mode> <corner>
            name, ftype, fc, gain, width, mode, corner = tokens[i + 1:i + 8]
            i += 8
            assert tokens[i] == "coeffs", tokens[i]
            coeffs = [float(v) for v in tokens[i + 1:i + 6]]
            i += 6
            assert tokens[i] == "magnitude_db", tokens[i]
            mag = [float(v) for v in tokens[i + 1:i + 1 + points]]
            i += 1 + points
            assert tokens[i] == "phase_deg", tokens[i]
            phase = [float(v) for v in tokens[i + 1:i + 1 + points]]
            i += 1 + points
            if name in cases:
                raise ValueError(f"case {name} appears twice")
            cases[name] = {"labels": [ftype, mode, corner],
                           "params": [float(fc), float(gain), float(width)],
                           "coeffs": coeffs, "magnitude_db": mag, "phase_deg": phase}
        else:
            raise ValueError(f"unexpected token {tok!r}")
    return {"header": header, "freqs": freqs, "cases": cases}


def check(payloads: dict) -> int:
    """Compares freshly generated files against the checked-in ones.

    Both go through parse_reference, so what is checked is exactly what the
    generator writes, and a case either side lacks fails.
    """
    worst, worst_where, failures = 0.0, "", 0

    def compare(where: str, fresh: list, old: list) -> None:
        nonlocal worst, worst_where, failures
        if len(fresh) != len(old):
            print(f"LENGTH {where}")
            failures += 1
            return
        for a, b in zip(fresh, old):
            d = abs(a - b)
            if d > worst:
                worst, worst_where = d, where
            if d > CHECK_TOLERANCE:
                failures += 1

    for fs, text in payloads.items():
        path = OUT_DIR / f"response_{int(fs)}.txt"
        if not path.exists():
            print(f"MISSING {path.name}")
            failures += 1
            continue
        fresh = parse_reference(text)
        stored = parse_reference(path.read_text(encoding="utf-8"))

        for key in sorted(fresh["header"].keys() | stored["header"].keys()):
            if fresh["header"].get(key) != stored["header"].get(key):
                print(f"HEADER {path.name} {key}: {stored['header'].get(key)}, "
                      f"expected {fresh['header'].get(key)}")
                failures += 1
        compare(f"{path.name} freqs", fresh["freqs"], stored["freqs"])

        for name in sorted(stored["cases"].keys() - fresh["cases"].keys()):
            print(f"EXTRA case {name} in {path.name} is not in CASES")
            failures += 1
        for name in sorted(fresh["cases"].keys() - stored["cases"].keys()):
            print(f"MISSING case {name} in {path.name}")
            failures += 1

        for name in sorted(fresh["cases"].keys() & stored["cases"].keys()):
            new, have = fresh["cases"][name], stored["cases"][name]
            if new["labels"] != have["labels"]:
                print(f"HEADER {path.name} {name}: {have['labels']}, expected {new['labels']}")
                failures += 1
            for field in ("params", "coeffs", "magnitude_db", "phase_deg"):
                compare(f"{path.name} {name} {field}", new[field], have[field])

    print(f"largest difference {worst:.3e} at {worst_where}")
    print(f"tolerance {CHECK_TOLERANCE:.0e}")
    if failures:
        print(f"FAIL: {failures} value(s) outside tolerance, or cases missing or extra")
        return 1
    print("OK: checked-in reference data matches a fresh computation")
    return 0


def compute() -> dict:
    """Returns {sample_rate: file text} without touching the filesystem.

    The generator writes this text and --check parses it, so the two cannot drift.
    """
    payloads = {}
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
        lines = [
            "# isotone reference data, generated by tools/gen_reference.py\n",
            f"# scipy {__import__('scipy').__version__}\n",
            "version 1\n",
            f"sample_rate {fs:.17g}\n",
            f"points {GRID_POINTS}\n",
            f"cases {len(cases)}\n",
            "freqs\n",
            " ".join(f"{v:.17g}" for v in freqs) + "\n",
        ]
        for c in cases:
            lines.append(f"case {c['name']} {c['type']} {c['fc']:.17g} {c['gain_db']:.17g} "
                         f"{c['width']:.17g} {c['width_mode']} {int(c['shelf_corner'])}\n")
            k = c["coeffs"]
            lines.append(f"coeffs {k['b0']:.17g} {k['b1']:.17g} {k['b2']:.17g} "
                         f"{k['a1']:.17g} {k['a2']:.17g}\n")
            lines.append("magnitude_db\n")
            lines.append(" ".join(f"{v:.17g}" for v in c["magnitude_db"]) + "\n")
            lines.append("phase_deg\n")
            lines.append(" ".join(f"{v:.17g}" for v in c["phase_deg"]) + "\n")
        payloads[fs] = "".join(lines)
    return payloads


def main() -> None:
    payloads = compute()
    if "--check" in sys.argv:
        raise SystemExit(check(payloads))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for fs, text in payloads.items():
        path = OUT_DIR / f"response_{int(fs)}.txt"
        with path.open("w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        print(f"wrote {path.name}: {len(CASES)} cases x {GRID_POINTS} points")

    print(f"{len(payloads)} reference files in {OUT_DIR}")


if __name__ == "__main__":
    main()
