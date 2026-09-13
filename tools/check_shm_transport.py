# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
"""Cross-process check of the shared-memory transport, without audiodg.

Runs the self-test build of IsoAPO in real time in one process
(`isotone-apo-selftest --serve`) and drives its region from another with
`isotone-shm`, the way the UI will: status and heartbeat, a parameter write, a
bypass write, and audio captured back out of the ring and measured. A separate
process matters, because it is the only way to exercise the mapping's DACL the
way the UI will meet it.

Windows only. Needs numpy.

Usage:
    python tools/check_shm_transport.py [build-dir]     default: build
"""

from __future__ import annotations

import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import time
import uuid

import numpy as np

failures = 0


def check(ok: bool, what: str, detail: str = "") -> None:
    global failures
    print(f"  {what:<64} {'ok' if ok else 'FAIL'} {detail}")
    if not ok:
        failures += 1


def find(build: pathlib.Path, name: str) -> pathlib.Path:
    # Single-config generators put executables straight in the target directory;
    # multi-config ones add a per-config subdirectory.
    hits = sorted(build.rglob(name))
    if not hits:
        sys.exit(f"{name} not found under {build}")
    return hits[0]


def level_1k_db(path: pathlib.Path, amplitude: float) -> float:
    """Level of channel 0 at 1 kHz relative to `amplitude`, by a Hann-windowed
    single-bin DFT, the same measurement the C++ self test uses."""
    data = path.read_bytes()
    fmt = data.find(b"fmt ")
    channels = struct.unpack_from("<H", data, fmt + 10)[0]
    rate = struct.unpack_from("<I", data, fmt + 12)[0]
    d = data.find(b"data")
    n = struct.unpack_from("<I", data, d + 4)[0]
    x = np.frombuffer(data, dtype="<f4", count=n // 4, offset=d + 8).reshape(-1, channels)[:, 0]
    i = np.arange(len(x))
    w = 0.5 - 0.5 * np.cos(2 * np.pi * i / (len(x) - 1))
    z = np.sum(x * w * np.exp(-2j * np.pi * 1000.0 * i / rate))
    return float(20 * np.log10(2 * abs(z) / w.sum() / amplitude))


def main() -> int:
    build = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
    selftest = find(build, "isotone-apo-selftest.exe")
    shm_exe = find(build, "isotone-shm.exe")

    def shm(*args: str, stdin: str | None = None):
        p = subprocess.run([str(shm_exe), *args], capture_output=True, text=True, input=stdin)
        out = p.stdout.strip()
        try:
            return p.returncode, (json.loads(out) if out else None)
        except json.JSONDecodeError:
            return p.returncode, {"_unparseable": out}

    guid = "{" + str(uuid.uuid4()).upper() + "}"

    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = pathlib.Path(tmp_name)

        print("argument handling")
        check(shm()[0] == 2, "no arguments exits 2")
        check(shm("status")[0] == 2, "missing endpoint exits 2")
        check(shm("capture", guid, "-1", "x.wav", "--local")[0] == 2, "negative capture length exits 2")
        check(shm("frobnicate", guid, "--local")[0] == 2, "unknown command exits 2")

        print("before any engine")
        code, js = shm("status", guid, "--local")
        check(code == 1 and js is not None and js.get("open") is False and js.get("error") == 2,
              "status reports engine idle")

        print("with an instance served from another process")
        server = subprocess.Popen([str(selftest), "--serve", guid, "9"], cwd=selftest.parent,
                                  stdout=subprocess.PIPE, text=True)
        time.sleep(1.3)

        code, js = shm("status", guid, "--local")
        check(code == 0 and js["open"] and js["heartbeat_advancing"], "status sees a live heartbeat")
        check(js.get("sample_rate") == 48000 and js.get("channels") == 2
              and js.get("host_state") == "running", "rate, channels and state are published")
        check(js.get("band_count") == 1 and js["ring"]["channels"] == 2
              and js["ring"]["writer"] != "0x0000000000000000", "seeded band and a claimed ring")

        code, js = shm("status", "{0.0.0.00000000}." + guid.lower(), "--local")
        check(code == 0 and js["open"], "a lower-case device id reaches the same region")

        cfg = tmp / "minus6.txt"
        cfg.write_text("Preamp: 0 dB\nFilter 1: ON PK Fc 1000 Hz Gain -6 dB Q 1\n")
        code, js = shm("write", guid, str(cfg), "--local")
        check(code == 0 and js["written"] and js["bands"] == 1 and js["warnings"] == [],
              "write a -6 dB config")

        time.sleep(0.3)   # the 20 ms smoother has long settled
        wav = tmp / "capture.wav"
        code, js = shm("capture", guid, "1", str(wav), "--local")
        check(code == 0 and js["captured"] and js["complete"] and js["channels"] == 2,
              "capture one second from the ring")
        if code == 0:
            lvl = level_1k_db(wav, 0.5)
            check(abs(lvl + 6.0) < 0.01, "ring audio measures -6 dB at 1 kHz", f"{lvl:+.3f} dB")

        code, js = shm("write", guid, str(cfg), "--bypass", "--local")
        check(code == 0 and js["bypass"], "write with --bypass")
        time.sleep(0.3)
        code, js = shm("capture", guid, "1", str(wav), "--local")
        if code == 0:
            lvl = level_1k_db(wav, 0.5)
            check(abs(lvl) < 0.01, "bypassed ring audio measures 0 dB", f"{lvl:+.3f} dB")
        else:
            check(False, "capture after bypass")

        code, js = shm("write", guid, "-", "--local", stdin="")
        check(code == 0 and js["bands"] == 0, "an empty config on stdin clears the bands")

        junk = tmp / "junk.txt"
        junk.write_bytes(b"Filter 1: ON ZZ Fc 1000 Hz\nChannel: BOGUS\n\x01\"quote\\\n")
        code, js = shm("write", guid, str(junk), "--local")
        check(code == 0 and "_unparseable" not in js and len(js["warnings"]) >= 2,
              "a malformed config still yields valid JSON with warnings")

        code, js = shm("write", guid, str(tmp / "missing.txt"), "--local")
        check(code == 1 and js["written"] is False, "a missing config file exits 1")

        lines = server.communicate(timeout=30)[0].strip().splitlines()
        levels = [json.loads(line)["level_db_1k"] for line in lines]
        print("  served instance, level at 1 kHz per second:", levels)
        check(server.returncode == 0, "served instance exited cleanly")
        check(len(levels) >= 8 and abs(levels[0] + 12.0) < 0.01, "its first second is the seeded -12 dB")

        code, js = shm("status", guid, "--local")
        check(code == 1 and js["error"] == 2, "with no holders left the region is gone")

    print("\nPASS" if failures == 0 else f"\nFAIL ({failures})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
