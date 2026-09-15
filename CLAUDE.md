# Isotone

A system-wide parametric EQ. A C++ DSP core runs inside IsoAPO (a Windows audio
processing object forked from Equalizer APO) or drives a stock Equalizer APO
through its config files (the compat backend). The Qt 6 Quick UI (stage 4) is in
`ui/`; EQ by ear (stage 5) and packaging (stage 6) are next.

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
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cm\CMake\bin\ctest.exe`" --test-dir build"   # core, transport, devicetool, compat, measure, devices tests
Push-Location build\windows\apo; .\isotone-apo-selftest.exe; Pop-Location      # IsoAPO hosted in-process
python tools\check_shm_transport.py build                                       # cross-process transport
python tools\gen_reference.py --check                                          # scipy reference data
```

The UI builds separately (Qt 6.11.2 MSVC kit in `C:\Qt\6.11.2\msvc2022_64`), off by default so CI is unchanged:

```powershell
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cm\CMake\bin\cmake.exe`" -S . -B build-ui -G Ninja -DISOTONE_BUILD_UI=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/msvc2022_64 -DISOTONE_WARNINGS_AS_ERRORS=ON"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cm\CMake\bin\cmake.exe`" --build build-ui --target isotone ui_tests ui_model_tests ui_qml_tests"
$env:PATH = "C:\Qt\6.11.2\msvc2022_64\bin;$env:PATH"
.\build-ui\ui\ui_tests.exe; .\build-ui\ui\ui_model_tests.exe
.\build-ui\ui\ui_qml_tests.exe -o "$env:TEMP\qml.txt,txt"    # read the file; add -input ui\tests\qml\tst_x.qml for one
```

- Run the app for checks only with `--data-dir <scratch> --compat-dir <scratch>
  --output {798436d2-8c71-4834-9248-00ccbaaca00a}`: without them it uses the
  owner's `%APPDATA%\Isotone` and writes the real Equalizer APO directory on an
  Equalizer APO output. `--screenshot`, `--click`, `--key`, `--add-band`,
  `--view`, `--fake-devicetool`, `--first-run` are listed at the top of
  `ui/src/main.cpp`.
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
windows/devicetool/   isotone-devicetool: list/status/test/install/uninstall/repair/roundtrip, enable-enhancements,
                      restart-audio, layouts/set-layout, serve; DevicetoolSession (session.h); upstream code vendored
windows/devices/      isotone_devices: render endpoints, their format and engine, change notifications, engine probe,
                      speaker layouts (speaker_layout.h, the one write)
windows/shmtool/      isotone-shm: status/write/persist/forget/capture on a region
windows/measure/      isotone-measure: stepped-sine measurement between endpoints; analysis in measure.cpp
ui/backend/           the UI without Qt: DeviceLink (where edits go), spectrum, typed values, speaker setup,
                      test tone, config.txt attach, diagnostics
ui/src/               EqSession (the edited state, undo), Outputs, Presets, Devices and Devicetool, Speakers,
                      ResponseGraph, settings, shortcuts, tray
ui/qml/               the screens; Main.qml is the window, Theme.qml the tokens
ui/tests/             ui_tests, ui_model_tests (doctest), qml/ (Qt Quick Test)
tools/                gen_reference.py, check_shm_transport.py
docs/design/          approved screens, the prototype's source, their generator; gitignored, this machine only
docs/notes/           the stage 4 work packages' records (force-added: docs/* is gitignored)
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
