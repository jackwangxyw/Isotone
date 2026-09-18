#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# Stage 3's acceptance measurement on Linux: the daemon, not the spike.
#
# Three paths reach the processor, and each one is measured through real audio
# rather than inspected:
#
#   flat   no saved state and nothing written: the daemon passes audio through
#   live   isotone-state set writes the shared region while audio is playing,
#          which is the path every UI edit takes
#   cold   isotone-state save writes the file, then the daemon is started fresh
#          and seeds its region from it, which is the path a sink takes when it
#          comes up before any UI runs
#
# The daemon creates its own virtual sink, so the only thing the rig declares is
# a stand-in for the hardware sink.

import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import measure_lib as m   # noqa: E402

# The build directory, so a run works both from an interactive ~/build-linux
# and from CI's build/ at the repository root.
BUILD = os.path.join(
    os.environ.get("ISOTONE_BUILD_DIR") or os.path.expanduser("~/build-linux"),
    "linux", "daemon")
DAEMON = os.path.join(BUILD, "isotone-daemon")
STATE_TOOL = os.path.join(BUILD, "isotone-state")
CONF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spike",
                    "10-isotone-spike.conf")

VIRT = "isotone"        # the daemon's own virtual sink
HW = "isotone_hw"       # stands in for the hardware sink
STATE_DIR = "/tmp/isotone-measure-state"

BAND = (1000.0, -12.0, 1.0)   # the filter stages 1b and 1c were measured with


def start_daemon():
    proc = subprocess.Popen(
        [DAEMON, "--sink", HW, "--state-dir", STATE_DIR],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # The daemon is ready when its own graph is linked.
    m.wait_for_ports(VIRT, "-o", 2)
    m.wait_for_ports("isotone-core", "-o", 2)
    deadline = time.time() + 10
    while time.time() < deadline:
        if "isotone-core:out_FL" in m.sh("pw-link -l").stdout:
            return proc
        time.sleep(0.1)
    proc.terminate()
    sys.exit("the daemon never linked its graph")


def stop_daemon(proc):
    proc.send_signal(2)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def state_tool(*args):
    result = subprocess.run([STATE_TOOL] + list(args), capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"isotone-state {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def band_arg():
    return f"{BAND[0]},{BAND[1]},{BAND[2]}"


def case_flat(freq):
    shutil.rmtree(STATE_DIR, ignore_errors=True)
    proc = start_daemon()
    try:
        return m.play_and_capture(VIRT, HW, freq)
    finally:
        stop_daemon(proc)


def case_live(freq):
    shutil.rmtree(STATE_DIR, ignore_errors=True)
    proc = start_daemon()
    try:
        state_tool("set", "--sink", HW, "--band", band_arg())
        return m.play_and_capture(VIRT, HW, freq)
    finally:
        stop_daemon(proc)


def case_cold(freq):
    shutil.rmtree(STATE_DIR, ignore_errors=True)
    state_tool("save", "--sink", HW, "--dir", STATE_DIR, "--band", band_arg())
    proc = start_daemon()
    try:
        return m.play_and_capture(VIRT, HW, freq)
    finally:
        stop_daemon(proc)


def main():
    for path in (DAEMON, STATE_TOOL):
        if not os.path.exists(path):
            sys.exit(f"{path} not built")
    m.install_conf(os.path.normpath(CONF))
    m.wait_for_ports(HW, "-i", 2)

    print(f"band: peaking {BAND[0]:.0f} Hz {BAND[1]:+.0f} dB Q {BAND[2]:.0f}\n")
    print(f"{'freq':>8}  {'path':>6}  {'flat':>9}  {'with band':>10}  "
          f"{'measured':>9}  {'analytic':>9}  {'error':>8}")
    print("-" * 74)

    failures = 0
    for freq in (1000.0, 100.0):
        flat = case_flat(freq)
        analytic = m.analytic_peaking_db(freq, *BAND)
        for label, case in (("live", case_live), ("cold", case_cold)):
            got = case(freq)
            measured = got - flat
            error = measured - analytic
            if abs(error) > 0.05:
                failures += 1
            print(f"{freq:8.0f}  {label:>6}  {flat:9.3f}  {got:10.3f}  "
                  f"{measured:9.3f}  {analytic:9.3f}  {error:+8.3f}")

    shutil.rmtree(STATE_DIR, ignore_errors=True)
    print()
    print("PASS" if failures == 0 else f"FAIL ({failures} outside 0.05 dB)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
