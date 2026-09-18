#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# Shared measurement rig for the Linux side: generate a tone, play it into a
# sink, capture another sink's monitor, and report the level at the tone's
# frequency. Used by linux/spike/measure.py (stage 1c) and
# linux/daemon/measure.py (stage 3).
#
# Nothing here needs audio hardware. Both ends are null sinks, which is what
# makes this runnable in a container and on a CI runner.

import math
import os
import subprocess
import sys
import time
import wave

import numpy as np

RATE = 48000
AMP = 0.5
DUR = 4.0

# Where the tone and capture WAVs go. CI points this at the runner's scratch.
WORK = os.environ.get("ISOTONE_MEASURE_WORK") or os.path.expanduser("~/spike")


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def ports(direction):
    """direction: '-o' for output ports (sources), '-i' for input ports (sinks)."""
    return sh(f"pw-link {direction}").stdout.split()


def wait_for_ports(prefix, direction, count, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = [p for p in ports(direction) if p.startswith(prefix + ":")]
        if len(found) >= count:
            return sorted(found)
        time.sleep(0.1)
    sys.exit(f"ports {prefix} ({direction}) never appeared; saw {ports(direction)}")


def wait_for_named(node, direction, names, timeout=10.0):
    """The node's ports with these exact suffixes, in the order given.

    Not sorted order: a 5.1 sink sorts playback_FC before playback_FL, so taking
    the first two would send the left channel to the centre speaker.
    """
    wanted = [f"{node}:{name}" for name in names]
    deadline = time.time() + timeout
    while time.time() < deadline:
        have = set(ports(direction))
        if all(w in have for w in wanted):
            return wanted
        time.sleep(0.1)
    sys.exit(f"{node} never showed {names} ({direction}); saw "
             f"{[p for p in ports(direction) if p.startswith(node + ':')]}")


def install_conf(conf_path):
    """Declare the rig's null sinks in a PipeWire drop-in.

    pactl load-module reports success and lists a sink that never becomes a
    PipeWire node once the daemon has been restarted, so the Pulse compat layer
    cannot be trusted for this. Declaring the nodes makes them come back with
    the daemon and a run reproducible.
    """
    config_home = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    dst_dir = os.path.join(config_home, "pipewire", "pipewire.conf.d")
    dst = os.path.join(dst_dir, os.path.basename(conf_path))
    wanted = open(conf_path).read()
    if os.path.exists(dst) and open(dst).read() == wanted:
        return
    os.makedirs(dst_dir, exist_ok=True)
    with open(dst, "w") as f:
        f.write(wanted)
    # A headless rig (CI) drops the file in before it starts PipeWire at all, so
    # there is nothing to restart and no systemd user manager to ask.
    if os.environ.get("ISOTONE_MEASURE_NO_RESTART"):
        return
    sh("systemctl --user restart pipewire wireplumber pipewire-pulse")
    time.sleep(2.0)


def make_tone(path, freq):
    t = np.arange(int(RATE * DUR)) / RATE
    x = AMP * np.sin(2 * math.pi * freq * t)
    pcm = np.clip(x * 32767.0, -32768, 32767).astype("<i2")
    stereo = np.repeat(pcm[:, None], 2, axis=1).tobytes()
    w = wave.open(path, "wb")
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(stereo)
    w.close()


def level_dbfs(path, freq):
    """Amplitude of the tone at `freq`, in dBFS.

    Projected onto the reference phasor rather than taken as a broadband RMS, so
    the noise floor and any DC offset do not enter the figure.
    """
    w = wave.open(path, "rb")
    channels, rate, frames = w.getnchannels(), w.getframerate(), w.getnframes()
    data = np.frombuffer(w.readframes(frames), dtype="<i2").astype(np.float64) / 32768.0
    w.close()
    if len(data) == 0:
        sys.exit(f"{path}: empty capture")

    left = data.reshape(-1, channels)[:, 0]
    loud = np.where(np.abs(left) > 0.002)[0]
    if len(loud) < rate:
        sys.exit(f"{path}: no signal (loudest sample {np.max(np.abs(left)):.6f})")

    # Drop a quarter second at each end, so parameter smoothing and the fade in
    # and out are outside the window.
    seg = left[loud[0] + rate // 4: loud[-1] - rate // 4]
    k = np.arange(len(seg))
    ref = np.exp(-2j * math.pi * freq * k / rate)
    amp = 2.0 * np.abs(np.sum(seg * ref)) / len(seg)
    return 20.0 * math.log10(amp) if amp > 0 else -999.0


def play_and_capture(play_into, capture_from, freq, work=None, positions=("FL", "FR")):
    """Play a tone into `play_into` and capture `capture_from`'s monitor.

    Every link is explicit. Passing "<sink>.monitor" to pw-record is a Pulse-ism:
    no PipeWire node carries that name, the target silently fails, the stream
    falls back to the default source, and the measurement then reads the tone
    with the graph under test nowhere in the path.
    """
    work = work or WORK
    os.makedirs(work, exist_ok=True)
    tone = os.path.join(work, "tone.wav")
    cap = os.path.join(work, "cap.wav")
    make_tone(tone, freq)

    rec = subprocess.Popen(
        ["pw-record", "--target", "0", "-P", "{ node.name = isotone-rec }",
         "--rate", str(RATE), "--channels", "2", "--format", "s16", cap],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    play = None
    try:
        rec_in = wait_for_named("isotone-rec", "-i", ["input_FL", "input_FR"])
        monitor = wait_for_named(capture_from, "-o", [f"monitor_{p}" for p in positions])
        for src, dst in zip(monitor, rec_in):
            sh(f"pw-link '{src}' '{dst}'")
        time.sleep(0.6)

        # Playback is linked by hand too. --target <sink> would leave the link to
        # the session manager's policy, and a CI runner has no WirePlumber to
        # apply one. An unlinked stream is simply not scheduled, so nothing of
        # the file is lost between starting it and linking it.
        play = subprocess.Popen(
            ["pw-play", "--target", "0", "-P", "{ node.name = isotone-play }", tone],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        play_out = wait_for_named("isotone-play", "-o", ["output_FL", "output_FR"])
        sink_in = wait_for_named(play_into, "-i", [f"playback_{p}" for p in positions])
        for src, dst in zip(play_out, sink_in):
            sh(f"pw-link '{src}' '{dst}'")

        play.wait(timeout=DUR * 4 + 20)
        time.sleep(0.4)
    finally:
        if play is not None and play.poll() is None:
            play.kill()
            play.wait(timeout=5)
        rec.send_signal(2)   # SIGINT: on SIGKILL the WAV header is never written
        rec.wait(timeout=10)

    return level_dbfs(cap, freq)


def analytic_peaking_db(freq, f0, gain_db, q, fs=RATE):
    """RBJ peaking EQ, the design core/src/biquad.cpp implements."""
    a = 10.0 ** (gain_db / 40.0)
    w0 = 2 * math.pi * f0 / fs
    alpha = math.sin(w0) / (2 * q)
    b = [1 + alpha * a, -2 * math.cos(w0), 1 - alpha * a]
    d = [1 + alpha / a, -2 * math.cos(w0), 1 - alpha / a]
    z = np.exp(-2j * math.pi * freq / fs)
    h = (b[0] + b[1] * z + b[2] * z ** 2) / (d[0] + d[1] * z + d[2] * z ** 2)
    return 20.0 * math.log10(abs(h))
