# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
"""The installer's MoveAside against a DLL that is loaded, as audiodg holds
IsoAPO.dll during an upgrade. Builds move_aside_test.nsi and runs it silently,
unelevated, in a scratch directory.

    python windows/setup/tests/test_move_aside.py <build dir with IsoAPO-selftest.dll>
"""

import ctypes
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
from ctypes import wintypes

HERE = pathlib.Path(__file__).resolve().parent
MAKENSIS = pathlib.Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "NSIS" / "makensis.exe"
PAYLOAD = (HERE / "move_aside_payload.txt").read_bytes()

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.LoadLibraryW.restype = wintypes.HMODULE
kernel32.LoadLibraryW.argtypes = [wintypes.LPCWSTR]
kernel32.FreeLibrary.argtypes = [wintypes.HMODULE]


def run(exe, dir):
    return subprocess.run([str(exe), "/S", f"/D={dir}"], timeout=60).returncode


def main():
    dll = next(pathlib.Path(sys.argv[1]).rglob("IsoAPO-selftest.dll"), None)
    if dll is None:
        print("FAIL: no IsoAPO-selftest.dll under", sys.argv[1])
        return 1
    failures = 0

    def check(ok, what):
        nonlocal failures
        print(("ok   " if ok else "FAIL ") + what)
        failures += not ok

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        exe = tmp / "move_aside_test.exe"
        built = subprocess.run([str(MAKENSIS), "/V1", f"/DOUTFILE={exe}", str(HERE / "move_aside_test.nsi")])
        if built.returncode != 0:
            print("FAIL: makensis")
            return 1

        # An upgrade: the old engine is loaded while the new one goes in.
        inst = tmp / "loaded"
        inst.mkdir()
        shutil.copy(dll, inst / "IsoAPO.dll")
        handle = kernel32.LoadLibraryW(str(inst / "IsoAPO.dll"))
        check(bool(handle), "the old engine is loaded")
        code = run(exe, inst)
        check(code == 0, f"the install finishes over a loaded engine (exit {code})")
        check((inst / "IsoAPO.dll").read_bytes() == PAYLOAD, "the new engine is at IsoAPO.dll")
        result = (inst / "result.txt").read_text() if (inst / "result.txt").exists() else ""
        moved = pathlib.Path(result.removeprefix("moved=")) if result.startswith("moved=") and result != "moved=" else None
        check(moved is not None and moved.exists() and moved.parent == inst, f"the old one was moved aside in the same directory ({result!r})")
        check(moved is not None and moved.read_bytes() == dll.read_bytes(), "and is the old engine, still loadable where it is")
        kernel32.FreeLibrary(handle)

        # A first install: nothing to move.
        fresh = tmp / "fresh"
        fresh.mkdir()
        code = run(exe, fresh)
        check(code == 0, f"a first install finishes (exit {code})")
        check((fresh / "result.txt").read_text() == "moved=", "and moves nothing")
        check((fresh / "IsoAPO.dll").read_bytes() == PAYLOAD, "and writes the engine")

        # A file that cannot even be renamed stops the install instead of
        # failing later at the copy.
        held = tmp / "held"
        held.mkdir()
        shutil.copy(dll, held / "IsoAPO.dll")
        with open(held / "IsoAPO.dll", "rb"):   # no FILE_SHARE_DELETE: a rename fails
            code = run(exe, held)
        check(code != 0, f"an engine that cannot be moved aborts the install (exit {code})")
        check(not (held / "result.txt").exists(), "before anything else is written")

    print("PASS" if failures == 0 else f"FAIL ({failures})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
