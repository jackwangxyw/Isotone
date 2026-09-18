#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# Stage 1c's acceptance measurement, the Linux counterpart of the Windows
# spikes: a peaking band at 1 kHz, -12 dB, Q 1, measured through a real PipeWire
# graph and compared against the analytic response.
#
# Topology under test (the EasyEffects model named in the plan):
#
#   pw-play -> isotone_virt (null sink)
#             isotone_virt.monitor -> isotone-spike:in_*      (the core)
#             isotone-spike:out_*  -> isotone_hw:playback_*
#                                     isotone_hw.monitor -> pw-record
#
# Each frequency is measured twice, once with the band and once with --bypass,
# and the reported figure is the difference. That cancels any fixed gain in the
# chain, so what is left is the filter. The bypass column earns its place: the
# run that had the core out of the path produced identical numbers in both
# columns, which is what a broken rig looks like.

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import measure_lib as m   # noqa: E402

# The build directory, so a run works both from an interactive ~/build-linux
# and from CI's build/ at the repository root.
BUILD = os.environ.get("ISOTONE_BUILD_DIR") or os.path.expanduser("~/build-linux")
SPIKE = os.path.join(BUILD, "linux", "spike", "isotone-pw-spike")
CONF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "10-isotone-spike.conf")

VIRT = "isotone_virt"
HW = "isotone_hw"
BAND = (1000.0, -12.0, 1.0)


def run_case(freq, bypass):
    spike = subprocess.Popen([SPIKE] + (["--bypass"] if bypass else []),
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        spike_in = m.wait_for_ports("isotone-spike", "-i", 2)
        spike_out = m.wait_for_ports("isotone-spike", "-o", 2)
        virt_monitor = m.wait_for_ports(VIRT, "-o", 2)
        hw_in = m.wait_for_ports(HW, "-i", 2)

        for src, dst in zip(virt_monitor, spike_in):
            m.sh(f"pw-link '{src}' '{dst}'")
        for src, dst in zip(spike_out, hw_in):
            m.sh(f"pw-link '{src}' '{dst}'")

        return m.play_and_capture(VIRT, HW, freq)
    finally:
        spike.terminate()
        spike.wait(timeout=10)


def main():
    if not os.path.exists(SPIKE):
        sys.exit(f"{SPIKE} not built")
    m.install_conf(CONF)
    m.wait_for_ports(VIRT, "-o", 2)
    m.wait_for_ports(HW, "-i", 2)

    print(f"band: peaking {BAND[0]:.0f} Hz {BAND[1]:+.0f} dB Q {BAND[2]:.0f}\n")
    print(f"{'freq':>8}  {'bypass':>9}  {'filtered':>9}  {'measured':>9}  "
          f"{'analytic':>9}  {'error':>8}")
    print("-" * 62)

    failures = 0
    for freq in (1000.0, 100.0):
        base = run_case(freq, bypass=True)
        filtered = run_case(freq, bypass=False)
        measured = filtered - base
        analytic = m.analytic_peaking_db(freq, *BAND)
        error = measured - analytic
        if abs(error) > 0.05:
            failures += 1
        print(f"{freq:8.0f}  {base:9.3f}  {filtered:9.3f}  {measured:9.3f}  "
              f"{analytic:9.3f}  {error:+8.3f}")

    print()
    print("PASS" if failures == 0 else f"FAIL ({failures} outside 0.05 dB)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
