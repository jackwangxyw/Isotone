#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# The Linux app measured through real audio: what linux/daemon/measure.py does
# with isotone-state, done with the app itself, so the Qt layer's own path is in
# the chain (Outputs finding the sink, EqSession, DeviceLink's region write).
#
#   app       the app, offscreen, adds a band the way its Add band button does;
#             the daemon plays it
#   saved     the app saves the output's state (Presets' save), the daemon is
#             started fresh, and seeds its region from the file the app wrote
#   tone      EQ by ear's tone, played by the app into Isotone's sink, reaches
#             the sink the daemon feeds through the same band
#
# Each figure is the difference from the same chain with nothing written, so a
# fixed gain anywhere cancels and what is left is the filter.
#
# Run under linux/ci-audio.sh, or in a session with the rig's sinks declared
# (linux/spike/10-isotone-spike.conf).

import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "linux"))
import measure_lib as m   # noqa: E402

BUILD = os.environ.get("ISOTONE_BUILD_DIR") or os.path.expanduser("~/build-linux")
APP = os.path.join(BUILD, "ui", "isotone")
DAEMON = os.path.join(BUILD, "linux", "daemon", "isotone-daemon")
CONF = os.path.join(ROOT, "linux", "spike", "10-isotone-spike.conf")

VIRT = "isotone"
HW = "isotone_hw"
# The daemon and the app find the saved state in the same place,
# $XDG_CONFIG_HOME/isotone/devices, so both run with this one.
CONFIG_HOME = os.path.join(tempfile.gettempdir(), "isotone-ui-measure-config")
DATA_DIR = os.path.join(tempfile.gettempdir(), "isotone-ui-measure-data")

# Add band's own Q (EqSession::addBand).
BAND = (1000.0, -12.0, 1.41)


def start_daemon():
    env = dict(os.environ, XDG_CONFIG_HOME=CONFIG_HOME)
    proc = subprocess.Popen([DAEMON, "--sink", HW], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    m.wait_for_ports("isotone-core", "-o", 2)
    deadline = time.time() + 15
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


def app(*args, seconds=3.0):
    """Runs the app offscreen on the rig's sink, with its own data directory."""
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=CONFIG_HOME)
    cmd = [APP, "--data-dir", DATA_DIR, "--output", HW, "--quit-after", str(seconds)] + list(args)
    result = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=seconds + 60)
    if result.returncode != 0:
        sys.exit(f"isotone {' '.join(args)} exited {result.returncode}: {result.stderr.strip()[-400:]}")
    return result


def capture_app_tone(freq, *extra):
    """Captures the sink the daemon feeds while the app plays EQ by ear's tone."""
    os.makedirs(m.WORK, exist_ok=True)
    cap = os.path.join(m.WORK, "tone-cap.wav")
    rec = subprocess.Popen(
        ["pw-record", "--target", "0", "-P", "{ node.name = isotone-rec }",
         "--rate", str(m.RATE), "--channels", "2", "--format", "s16", cap],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        rec_in = m.wait_for_named("isotone-rec", "-i", ["input_FL", "input_FR"])
        monitor = m.wait_for_named(HW, "-o", ["monitor_FL", "monitor_FR"])
        for src, dst in zip(monitor, rec_in):
            m.sh(f"pw-link '{src}' '{dst}'")
        time.sleep(0.6)
        app(*extra, "--ear-tone", f"{freq:.0f}", seconds=m.DUR + 1.0)
        time.sleep(0.4)
    finally:
        rec.send_signal(2)
        rec.wait(timeout=10)
    return m.level_dbfs(cap, freq)


def fresh():
    shutil.rmtree(CONFIG_HOME, ignore_errors=True)
    shutil.rmtree(DATA_DIR, ignore_errors=True)


def main():
    for path in (APP, DAEMON):
        if not os.path.exists(path):
            sys.exit(f"{path} not built")
    m.install_conf(os.path.normpath(CONF))
    m.wait_for_ports(HW, "-i", 2)

    print(f"band: peaking {BAND[0]:.0f} Hz {BAND[1]:+.0f} dB Q {BAND[2]}\n")
    print(f"{'case':>10}  {'freq':>6}  {'flat':>9}  {'with band':>10}  "
          f"{'measured':>9}  {'analytic':>9}  {'error':>8}")
    print("-" * 70)
    failures = 0

    def report(label, freq, flat, got):
        nonlocal failures
        analytic = m.analytic_peaking_db(freq, *BAND)
        error = got - flat - analytic
        if abs(error) > 0.05:
            failures += 1
        print(f"{label:>10}  {freq:6.0f}  {flat:9.3f}  {got:10.3f}  "
              f"{got - flat:9.3f}  {analytic:9.3f}  {error:+8.3f}")

    for freq in (1000.0, 100.0):
        # Flat: the same daemon, nothing written.
        fresh()
        proc = start_daemon()
        try:
            flat = m.play_and_capture(VIRT, HW, freq)
            # The app edits the live region, as a drag or a typed value does.
            app("--add-band", f"{BAND[0]:.0f},{BAND[1]:.0f}")
            live = m.play_and_capture(VIRT, HW, freq)
        finally:
            stop_daemon(proc)
        report("app", freq, flat, live)

    # Saved: the file the app writes is what a daemon starts from.
    fresh()
    proc = start_daemon()
    try:
        flat = m.play_and_capture(VIRT, HW, 1000.0)
        app("--add-band", f"{BAND[0]:.0f},{BAND[1]:.0f}", "--save-output")
    finally:
        stop_daemon(proc)
    proc = start_daemon()
    try:
        saved = m.play_and_capture(VIRT, HW, 1000.0)
    finally:
        stop_daemon(proc)
    report("saved", 1000.0, flat, saved)

    # Tone: EQ by ear's own stream, into Isotone's sink and out through the band.
    fresh()
    proc = start_daemon()
    try:
        flat = capture_app_tone(1000.0)
        tone = capture_app_tone(1000.0, "--add-band", f"{BAND[0]:.0f},{BAND[1]:.0f}")
    finally:
        stop_daemon(proc)
    report("tone", 1000.0, flat, tone)

    fresh()
    print()
    print("PASS" if failures == 0 else f"FAIL ({failures} outside 0.05 dB)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
