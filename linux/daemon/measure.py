#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# Stage 3's acceptance measurement on Linux: the daemon, not the spike. Every
# path is measured through real audio rather than inspected.
#
#   live      isotone-state set writes the shared region while audio is playing,
#             which is the path every UI edit takes
#   cold      isotone-state save writes the file, then the daemon is started
#             fresh and seeds its region from it, which is the path a sink takes
#             when it comes up before any UI runs
#   ring      the post-EQ ring the UI's spectrum drains, read back while the same
#             audio plays and compared against what reached the sink
#   44100     the same, with the graph forced to another rate, which makes the
#             daemon re-size its processor off the audio thread mid-stream
#   5.1       a six-channel core into a six-channel sink
#   stale     a daemon killed outright leaves its region and its ring claim
#             behind; the next one has to adopt the region and take the ring
#             over, or the UI's spectrum is dead for the whole of that run
#   capture   an application playing to the default sink, which is the hardware:
#             the daemon moves its stream into the virtual sink, so the band
#             applies without Isotone being the default
#   leave     the same with --leave-streams: the stream goes straight to the
#             hardware and the band does not apply, which is what says the
#             capture, and nothing else, put the EQ in the path
#   release   a stream still playing when the daemon exits goes back to the
#             hardware sink rather than following a sink that is gone
#
# Each figure is the difference from the same daemon with nothing written, so a
# fixed gain anywhere in the chain cancels and what is left is the filter.
#
# The daemon creates its own virtual sink, so the only thing the rig declares is
# the stand-in for the hardware.

import math
import os
import shutil
import subprocess
import sys
import time

import numpy as np

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

VIRT = "isotone"          # the daemon's own virtual sink
HW = "isotone_hw"         # stands in for a stereo hardware sink
HW51 = "isotone_hw51"     # and for a 5.1 one
STATE_DIR = "/tmp/isotone-measure-state"

BAND = (1000.0, -12.0, 1.0)   # the filter stages 1b and 1c were measured with


def band_arg():
    return f"{BAND[0]},{BAND[1]},{BAND[2]}"


def start_daemon(sink=HW, channels=2, keep_region=False, extra=()):
    args = [DAEMON, "--sink", sink, "--channels", str(channels), "--state-dir", STATE_DIR]
    if keep_region:
        args.append("--keep-region")
    args += list(extra)
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    m.wait_for_ports(VIRT, "-o", channels)
    m.wait_for_ports("isotone-core", "-o", channels)
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


def state_tool(*args):
    result = subprocess.run([STATE_TOOL] + list(args), capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"isotone-state {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def force_rate(rate):
    m.sh(f"pw-metadata -n settings 0 clock.force-rate {rate}")
    time.sleep(1.5)


def ring_level(sink, freq, seconds):
    """The tone's level in the post-EQ ring, read while the same audio plays."""
    raw = "/tmp/isotone-ring.f32"
    env = dict(os.environ, ISOTONE_CAPTURE_RAW=raw)
    if os.path.exists(raw):
        os.remove(raw)
    proc = subprocess.Popen([STATE_TOOL, "capture", "--sink", sink, "--seconds", str(seconds)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    return proc, raw


def ring_result(proc, raw, freq):
    proc.wait(timeout=60)
    if not os.path.exists(raw):
        sys.exit("the ring capture wrote nothing")
    x = np.fromfile(raw, dtype=np.float32).astype(float)
    if len(x) < 48000:
        sys.exit(f"only {len(x)} frames reached the ring")

    # The capture starts before the tone and ends after it, and the sink keeps
    # running in between, so the ring holds silence at both ends. Averaging over
    # that pulls the level down; the signal has to be found first, exactly as
    # level_dbfs does for the sink's own capture.
    loud = np.where(np.abs(x) > 0.002)[0]
    if len(loud) < m.RATE:
        sys.exit(f"the ring holds no tone (loudest sample {np.max(np.abs(x)):.6f})")
    seg = x[loud[0] + m.RATE // 4: loud[-1] - m.RATE // 4]
    k = np.arange(len(seg))
    amp = 2.0 * abs(np.sum(seg * np.exp(-2j * math.pi * freq * k / m.RATE))) / len(seg)
    return 20.0 * math.log10(amp) if amp > 0 else -999.0


def case(freq, sink=HW, channels=2, band=False, cold=False, with_ring=False):
    shutil.rmtree(STATE_DIR, ignore_errors=True)
    if cold and band:
        state_tool("save", "--sink", sink, "--dir", STATE_DIR, "--band", band_arg())
    proc = start_daemon(sink, channels)
    ring = None
    try:
        if band and not cold:
            state_tool("set", "--sink", sink, "--band", band_arg())
        if with_ring:
            ring = ring_level(sink, freq, m.DUR + 1.0)
        level = m.play_and_capture(VIRT, sink, freq)
        ring_db = ring_result(ring[0], ring[1], freq) if ring else None
        return level, ring_db
    finally:
        stop_daemon(proc)


def leave_stale_region(freq):
    """Kill a daemon mid-stream so its region and ring claim survive it.

    A ring another instance holds is only taken over once its write index has
    stood still for a while, so the claim has to be retried from the audio
    thread. Claiming once at startup left the ring dead for the whole run, and
    with it the UI's spectrum.
    """
    shutil.rmtree(STATE_DIR, ignore_errors=True)
    proc = start_daemon(keep_region=True)
    tone = os.path.join(m.WORK, "stale.wav")
    os.makedirs(m.WORK, exist_ok=True)
    m.make_tone(tone, freq)
    play = subprocess.Popen(["pw-play", "--target", VIRT, tone],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        # Long enough that the dying instance has written to the ring.
        time.sleep(2.0)
    finally:
        proc.kill()
        proc.wait(timeout=10)
        play.kill()
        play.wait(timeout=10)


def set_default_sink(name):
    # WirePlumber reads the value as JSON only when it is typed as JSON.
    m.sh(f"pw-metadata 0 default.configured.audio.sink '{{ \"name\": \"{name}\" }}' Spa:String:JSON")
    deadline = time.time() + 5
    while time.time() < deadline:
        if f'"{name}"' in m.sh("pw-metadata 0 default.audio.sink").stdout:
            return
        time.sleep(0.1)
    sys.exit(f"{name} never became the default sink")


def app_level(freq):
    """Plays as an application does, to the default sink, and captures the hardware.

    No target and no hand-made link: where the stream goes is up to the session
    manager and the daemon, which is the point.
    """
    os.makedirs(m.WORK, exist_ok=True)
    tone = os.path.join(m.WORK, "app.wav")
    cap = os.path.join(m.WORK, "app-cap.wav")
    m.make_tone(tone, freq)
    rec = subprocess.Popen(
        ["pw-record", "--target", "0", "-P", "{ node.name = isotone-rec }",
         "--rate", str(m.RATE), "--channels", "2", "--format", "s16", cap],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        rec_in = m.wait_for_named("isotone-rec", "-i", ["input_FL", "input_FR"])
        for src, dst in zip(m.wait_for_named(HW, "-o", ["monitor_FL", "monitor_FR"]), rec_in):
            m.sh(f"pw-link '{src}' '{dst}'")
        time.sleep(0.6)
        subprocess.run(["pw-play", "-P", "{ node.name = app-play }", tone], timeout=m.DUR * 4 + 20,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.4)
    finally:
        rec.send_signal(2)
        rec.wait(timeout=10)
    return m.level_dbfs(cap, freq)


def app_case(freq, band, extra=()):
    shutil.rmtree(STATE_DIR, ignore_errors=True)
    proc = start_daemon(extra=extra)
    try:
        if band:
            state_tool("set", "--sink", HW, "--band", band_arg())
        return app_level(freq)
    finally:
        stop_daemon(proc)


def linked_to(stream):
    """The sinks a stream's output ports are linked to."""
    out = m.sh("pw-link -l").stdout.splitlines()
    sinks = set()
    for i, line in enumerate(out):
        if line.startswith(f"{stream}:output_"):
            for follow in out[i + 1:]:
                if not follow.startswith(" "):
                    break
                if "|->" in follow:
                    sinks.add(follow.split("|->")[1].strip().split(":")[0])
    return sinks


def wait_linked(stream, sink, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if linked_to(stream) == {sink}:
            return True
        time.sleep(0.1)
    return False


def released():
    """A stream playing when the daemon exits goes back to the hardware."""
    shutil.rmtree(STATE_DIR, ignore_errors=True)
    os.makedirs(m.WORK, exist_ok=True)
    tone = os.path.join(m.WORK, "long.wav")
    m.make_tone(tone, 1000.0, seconds=20.0)   # still playing when the daemon has gone
    proc = start_daemon()
    play = subprocess.Popen(["pw-play", "-P", "{ node.name = app-play }", tone],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        moved = wait_linked("app-play", VIRT)
        stop_daemon(proc)
        back = wait_linked("app-play", HW)
        return moved, back
    finally:
        if proc.poll() is None:
            stop_daemon(proc)
        play.kill()
        play.wait(timeout=10)


def main():
    for path in (DAEMON, STATE_TOOL):
        if not os.path.exists(path):
            sys.exit(f"{path} not built")
    m.install_conf(os.path.normpath(CONF))
    m.wait_for_ports(HW, "-i", 2)
    m.wait_for_ports(HW51, "-i", 6)

    print(f"band: peaking {BAND[0]:.0f} Hz {BAND[1]:+.0f} dB Q {BAND[2]:.0f}\n")
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

    # Stereo: live, cold, and the ring, at both frequencies.
    for freq in (1000.0, 100.0):
        flat, _ = case(freq)
        live, ring = case(freq, band=True, with_ring=(freq == 1000.0))
        report("live", freq, flat, live)
        cold, _ = case(freq, band=True, cold=True)
        report("cold", freq, flat, cold)
        if ring is not None:
            # The ring is the same audio the sink got, so it must read the same.
            delta = ring - live
            ok = abs(delta) <= 0.05
            failures += 0 if ok else 1
            print(f"{'ring':>10}  {freq:6.0f}  {'':>9}  {ring:10.3f}  "
                  f"{'':>9}  {'vs sink':>9}  {delta:+8.3f}")

    # A rate change mid-stream: the processor is re-sized on the main loop.
    force_rate(44100)
    try:
        flat, _ = case(1000.0)
        got, _ = case(1000.0, band=True)
        report("44100", 1000.0, flat, got)
    finally:
        force_rate(0)

    # Six channels, into a six-channel sink.
    flat, _ = case(1000.0, sink=HW51, channels=6)
    got, _ = case(1000.0, sink=HW51, channels=6, band=True)
    report("5.1", 1000.0, flat, got)

    # A region and a ring claim left behind by a daemon that was killed.
    leave_stale_region(1000.0)
    flat, _ = case(1000.0)
    got, ring = case(1000.0, band=True, with_ring=True)
    report("stale", 1000.0, flat, got)
    if ring is None or abs(ring - got) > 0.05:
        failures += 1
        print(f"{'stale ring':>10}  {1000.0:6.0f}  {'':>9}  "
              f"{(ring if ring is not None else float('nan')):10.3f}  {'':>9}  "
              f"{'vs sink':>9}  {'dead' if ring is None else f'{ring - got:+.3f}'}")
    else:
        print(f"{'stale ring':>10}  {1000.0:6.0f}  {'':>9}  {ring:10.3f}  "
              f"{'':>9}  {'vs sink':>9}  {ring - got:+8.3f}")

    # Applications playing to the default sink, which is the hardware.
    set_default_sink(HW)
    flat = app_case(1000.0, band=False)
    got = app_case(1000.0, band=True)
    report("capture", 1000.0, flat, got)
    left = app_case(1000.0, band=True, extra=["--leave-streams"])
    left_error = left - flat
    if abs(left_error) > 0.05:
        failures += 1
    print(f"{'leave':>10}  {1000.0:6.0f}  {flat:9.3f}  {left:10.3f}  "
          f"{left_error:9.3f}  {0.0:9.3f}  {left_error:+8.3f}")
    moved, back = released()
    if not (moved and back):
        failures += 1
    print(f"{'release':>10}  stream into {VIRT}: {'yes' if moved else 'NO'}, "
          f"back to {HW} after exit: {'yes' if back else 'NO'}")

    shutil.rmtree(STATE_DIR, ignore_errors=True)
    print()
    print("PASS" if failures == 0 else f"FAIL ({failures} outside 0.05 dB)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
