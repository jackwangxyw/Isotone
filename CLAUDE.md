# Isotone

A system-wide parametric EQ. A C++ DSP core runs inside IsoAPO (a Windows audio
processing object forked from Equalizer APO) or drives a stock Equalizer APO
through its config files (the compat backend). Stage 4, the Qt 6 Quick UI, is
next.

Read first:
- `docs/ui-spec.md`: the stage 4 build brief, including "Engine contracts the UI
  must keep".
- `docs/decisions.md`: every decision and measurement; "Where things stand" at
  the end.
- `docs/isotone-plan.md`: the original plan; the spec and decisions override it.

## Build and test (Windows, PowerShell)

Nothing is on PATH. MSVC and CMake come from VS Build Tools:

```powershell
$vs = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools'
$cm = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cm\CMake\bin\cmake.exe`" -S . -B build -G Ninja"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cm\CMake\bin\cmake.exe`" --build build"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cm\CMake\bin\ctest.exe`" --test-dir build"   # core, compat, measure, devices tests
Push-Location build\windows\apo; .\isotone-apo-selftest.exe; Pop-Location      # IsoAPO hosted in-process
python tools\check_shm_transport.py build                                       # cross-process transport
python tools\gen_reference.py --check                                          # scipy reference data
```

- A single core test: `build\core\tests\core_tests.exe -tc="name*"`.
- GCC (stand-in for the Linux CI job): WinLibs g++ under
  `%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_*\mingw64\bin`,
  on PATH for the call, building into `build-gcc` with the bundled Ninja.
- CI (`.github/workflows/ci.yml`) runs MSVC and GCC builds with
  `-DISOTONE_WARNINGS_AS_ERRORS=ON`, ctest, the APO self test, the transport
  check and the reference-data check. Pass the option locally too, or a warning
  only shows up in CI.

## Code map

```
core/                 portable C++20: biquad design, Processor (smoothing, crossfades,
                      speaker routing, bass management, delay), response curves,
                      Equalizer APO config parser/formatter, ParamBlock, audio ring
core/tests/           doctest; reference/ is scipy-generated response data
windows/transport/    named shared region (ParamBlock + ring), per-endpoint saved state
windows/apo/          IsoAPO.dll, IsoAPO-selftest.dll (Local\ namespace), isotone-apo-selftest
windows/compat/       isotone-compat and its library: Isotone.txt, config.txt attach, loopback
windows/devicetool/   isotone-devicetool: list/status/test/install/uninstall/repair/roundtrip; upstream code vendored
windows/devices/      isotone_devices: render endpoints, their format and engine, change notifications, engine probe
windows/shmtool/      isotone-shm: status/write/persist/forget/capture on a region
windows/measure/      isotone-measure: stepped-sine measurement between endpoints; analysis in measure.cpp
tools/                gen_reference.py, check_shm_transport.py
docs/design/          approved screens and their generator; gitignored, this machine only
```

## Rules

- **Commits and pushes only when the owner says so, each time.**
- **Audio and registry safety.** Live audio tests use only the VB-Cable pair:
  render CABLE Input `{798436d2-8c71-4834-9248-00ccbaaca00a}`, capture CABLE
  Output `{16d43645-3380-4055-9de6-f7349761051d}`. Never change the owner's live
  Equalizer APO config or audio settings without permission, and revert
  whatever was allowed. The shell is not elevated: HKLM writes, COM registration
  and staging the DLL are done by the owner. Never hand over a registry-changing
  command that has not been run: `isotone-devicetool install|uninstall|repair
  --dry-run` and `roundtrip` run every check with no side effects.
  `isotone-compat` reaches the real Equalizer APO directory only with
  `--real-install`.
- **Tests first.** A fix comes with a test that fails without it; revert the fix
  to confirm (mutation check). Behaviour copied from upstream Equalizer APO is
  checked against its source, not assumed.
- **Owner preferences.** No explainer microcopy in the UI. Short, direct
  replies; no em dashes, no emoji. A question about the design is a question,
  not a request to change it.
