# Isotone

A system-wide parametric EQ. A C++ DSP core runs inside IsoAPO (a Windows audio
processing object forked from Equalizer APO) or drives a stock Equalizer APO
through its config files (the compat backend). Stages 0 to 4 are done: the Qt 6
Quick UI is in `ui/`, every screen of the prototype except EQ by ear, and the
owner has used it on his own outputs. EQ by ear (stage 5) is done: the sweep; A/B
is set aside for later. On Linux the daemon (`linux/`) hosts the same core and the
same UI runs against it, checked on Cinnamon under X11 in a VM and on the owner's
own laptop (decisions.md, the entries of 2026-09-19).

Packaging (stage 6) is built and measured on all three targets (2026-09-20): an
NSIS installer, a `.deb` and a Flatpak, each run for real and measured against
the analytic filter rather than declared working. **0.1.0 is not tagged yet.**
Left in stage 6, and both need the owner: Windows' launch at sign-in wants one
sign-out to confirm, and `v0.1.0` wants the repository public first, because the
AppStream metadata points at it. Everything else in stage 6 is closed. GNOME and
KDE now have a VM each, on Wayland; the Flatpak starts at sign-in through the
Background portal and starts its own daemon, and one daemon at a time is
enforced by a lock rather than by luck. See decisions.md, "Where things stand".

A full review pass over the tree on 2026-09-21 found five defects, each fixed
with a test that fails without it: the response graph designed every band at a
fixed 48 kHz instead of the output's rate; a pre-mix IsoAPO reported the
post-mix class; the installer and uninstaller did not notice a running Isotone
and left Program Files half emptied; the Windows side took the shared header's
sample rate on trust; and `devices_tests` asserted that the registry and the
audio API agree about every render endpoint, which Windows does not guarantee.
The same day the application ID was renamed to
**`io.github.jackwangxyw.Isotone`**, the account the repository is under; the
old one survives only in `autostart_xdg.h`'s list of entry names to clean up.
See decisions.md, "A review pass over everything, and five fixes".

The UI had an outside review on 2026-09-20 and two parts of it were acted on:
the icon set is Phosphor Bold, and status is a bar rather than a coloured dot
(decisions.md, "The icons, the status mark and the output dropdown"). What was
recorded and deliberately not changed: the palette, the type scale, the
toggle component and the empty space in Settings.

Read first:
- `docs/ui-spec.md`: the UI build brief, stage 4 and the EQ by ear screens for
  stage 5, including "Engine contracts the UI must keep".
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
python tools\gen_icons.py --check                                              # icons still match the mark
```

The UI builds separately (Qt 6.11.2 MSVC kit in `C:\Qt\6.11.2\msvc2022_64`), off by default so CI is unchanged:

```powershell
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cm\CMake\bin\cmake.exe`" -S . -B build-ui -G Ninja -DISOTONE_BUILD_UI=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/msvc2022_64 -DISOTONE_WARNINGS_AS_ERRORS=ON"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cm\CMake\bin\cmake.exe`" --build build-ui --target isotone ui_tests ui_model_tests ui_qml_tests"
$env:PATH = "C:\Qt\6.11.2\msvc2022_64\bin;$env:PATH"
.\build-ui\ui\ui_tests.exe; .\build-ui\ui\ui_model_tests.exe
.\build-ui\ui\ui_qml_tests.exe -o "$env:TEMP\qml.txt,txt"    # read the file; add -input ui\tests\qml\tst_x.qml for one
```

The installer is NSIS 3.12 (`winget install NSIS.NSIS`), built from a staged
install tree rather than from the build directory. `cmake --install` runs
windeployqt, which stages the Qt runtime:

```powershell
$stage = "$env:TEMP\isotone-stage"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cm\CMake\bin\cmake.exe`" --install build-ui --prefix `"$stage`""
& "${env:ProgramFiles(x86)}\NSIS\makensis.exe" /DSTAGE="$stage" /DVERSION=0.1.0 windows\setup\isotone.nsi
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
                      Equalizer APO config parser/formatter, curve import (GraphicEQ
                      and FilterCurve fitted to bands), ParamBlock, audio ring
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
linux/transport/      POSIX shared region (ParamBlock + ring), saved state under XDG,
                      daemon_lock (one daemon at a time, an flock in the shared /dev/shm)
linux/daemon/         isotone-daemon: the virtual sink, the core in a PipeWire filter node,
                      links to the sink being fed, the post-EQ ring, following the default sink
                      through WirePlumber metadata, capturing applications' streams, stereo (--channels to 7.1 for the rig),
                      systemd user unit;
                      isotone-state (show/set/save/capture)
linux/spike/          stage 1c: the topology spike and the null sinks the rig declares
linux/measure_lib.py  the shared measurement rig; linux/ci-audio.sh runs it under its own PipeWire
ui/backend/           the UI without Qt: DeviceLink (where edits go), spectrum, typed values, speaker setup,
                      test tone, config.txt attach, diagnostics; *_posix and pipewire_outputs, autostart_xdg,
                      daemon_region for Linux
ui/src/               EqSession (the edited state, undo), Outputs, Presets, Devices and Devicetool, Speakers,
                      ResponseGraph, settings, shortcuts, tray
ui/qml/               the screens; Main.qml is the window, Theme.qml the tokens,
                      Icon.qml the icon set (Phosphor Bold, filled on a 256 box;
                      the mark and the sidebar toggle are ours, stroked on 24)
ui/tests/             ui_tests, ui_model_tests (doctest), qml/ (Qt Quick Test); test_rig.h (an engine region of the
                      test's own, either platform); measure_linux.py; mock_portal.py (GlobalShortcuts)
windows/setup/        isotone.nsi (NSIS), and the welcome and header bitmaps
linux/packaging/      the .deb's maintainer scripts, desktop entry, AppStream
                      metadata, icon theme, man pages, and flatpak/ with the manifest
tools/                gen_reference.py, check_shm_transport.py, gen_icons.py
docs/design/          approved screens, the prototype's source, their generator; gitignored, this machine only
docs/notes/           the stage 4 work packages' records (force-added: docs/* is gitignored)
```

## Build and test (Linux, over WSL)

Linux work happens in a WSL2 Ubuntu 24.04 distro, not a VM (decisions.md, "Linux,
the environment as built"). It matches CI's compiler exactly, g++ 13.3. Drive it
from Git Bash: `MSYS_NO_PATHCONV=1` stops Git Bash rewriting Linux paths into
Windows ones, and `wsl.exe` emits UTF-16.

```bash
export MSYS_NO_PATHCONV=1
wsl.exe -d Ubuntu-24.04 -e bash -c '<command>' 2>&1 | tr -d '\0'
```

Source stays on the Windows side so there is one tree; build into the Linux
filesystem, because compiling across `/mnt/c` is slow:

```bash
cd /mnt/c/Users/jackw/OneDrive/Documents/GitHub/Isotone
cmake -S . -B ~/build-linux -G Ninja -DISOTONE_WARNINGS_AS_ERRORS=ON -DISOTONE_BUILD_UI=ON
cmake --build ~/build-linux
ctest --test-dir ~/build-linux                            # core, transport, the UI's suites (X11 under Xvfb, the portal on a private bus)
python3 linux/spike/measure.py                            # stage 1c, in the session's PipeWire
python3 linux/daemon/measure.py                           # stage 3, the daemon
python3 ui/tests/measure_linux.py                         # the app through the daemon
ISOTONE_BUILD_DIR=~/build-linux bash linux/ci-audio.sh    # all three, under a PipeWire of its own
```

The `.deb` and the Flatpak, both built in WSL, both unelevated apart from the
`.deb`'s install:

```bash
cd ~/build-linux && cpack -G DEB && lintian --tag-display-limit 0 isotone-0.1.0-Linux.deb
# The Flatpak wants the source in the Linux filesystem: flatpak-builder copies
# the tree, and doing that across /mnt/c is very slow.
rm -rf ~/flat-src && mkdir ~/flat-src
cd /mnt/c/Users/jackw/OneDrive/Documents/GitHub/Isotone && git ls-files -z | tar --null -T - -cf - | tar -xf - -C ~/flat-src
cd ~/flat-src
flatpak-builder --user --force-clean --disable-rofiles-fuse --repo=$HOME/flat-repo \n    ~/flat-build linux/packaging/flatpak/io.github.jackwangxyw.Isotone.yml
flatpak build-bundle $HOME/flat-repo $HOME/isotone-0.1.0.flatpak io.github.jackwangxyw.Isotone master
```

- `linux/spike` and `linux/daemon` are added only when pkg-config finds
  `libpipewire-0.3`. The transport and its tests build on any Linux.
- The rig needs no audio hardware: both ends of the chain are null sinks, declared
  in `linux/spike/10-isotone-spike.conf`. `pactl` is not to be trusted for this;
  it reports sinks that never become PipeWire nodes.
- Do not use the repository's `build/` from Linux. That is the Windows build
  directory, and CI's Linux job is the only thing that builds into it.
- The UI uses the distribution's Qt, 6.4.2. What 6.4 lacks is bridged where it is
  used; decisions.md, "The Qt layer on Linux", lists each. Test with it, not only
  with Windows' 6.11.
- The app runs offscreen in WSL (`QT_QPA_PLATFORM=offscreen`); anything that needs
  a desktop (the tray, global hotkeys, windows) is checked in the Mint VM below.

## The owner's laptop (the second desktop)

`ssh isotone-laptop` over Tailscale (docs/notes/linux-vm-setup.md, local): his
own Linux Mint 22.3 Cinnamon machine on X11, where he watches the screen while
a check runs. The tree is copied to `~/Isotone` (git ls-files over tar), built
into `~/Isotone/build`, and everything else lives in `~/Isotone/.work`:
`show.sh <name> <cmd>` runs a command in a terminal window on his screen,
`rig-up.sh`/`rig-down.sh` make and remove a test sink in the running PipeWire
(nothing written to his config), `x.py` clicks, presses keys and captures the
Isotone window through XTEST. Capture the window or the panel, never the whole
screen: his desktop is his. sudo there needs his password, always: hand him the
command.

The `.deb` built from this tree is installed there (2026-09-21) and enabled, so
the daemon starts at his next sign-in; for a check, run one from a build
directory instead and let the lock refuse the second. `~/Isotone` is a copy of
the tree as of 2026-09-19 and wants a resync before it is built again.

## The GNOME and KDE VMs (Wayland)

`ssh isotone-gnome` (Ubuntu 26.04.1, GNOME on Wayland) and `ssh isotone-kde`
(Kubuntu 26.04.1, Plasma 6 on Wayland), both VirtualBox, both left powered off;
start them with `VBoxManage startvm "Isotone GNOME" --type headless`. They carry
the tree in `~/Isotone`, a build in `~/build`, the daemon's user unit pointing
at it, the rig's null sinks and the 0.1.0 Flatpak, which is
`io.github.jackwangxyw.Isotone` since 2026-09-21 and has launch at sign-in on,
so the app and its daemon are up as soon as either VM boots. `~/desk.sh <cmd>`
runs a command in the logged-in session on either. Their `~/build` predates the
rename and wants a resync before a build-tree check.

Power them off with `VBoxManage controlvm <vm> acpipowerbutton`, and answer
Plasma's confirmation with `keyboardputscancode 1c 9c`. Not `systemctl reboot`
over ssh: polkit refuses it and returns 0, so it looks like it worked and
nothing happens.

They are for the desktop, not for audio: keys go in with
`VBoxManage controlvm <vm> keyboardputscancode` (XTEST does not reach a Wayland
compositor) and screenshots with `VBoxManage controlvm <vm> screenshotpng`.
docs/notes/linux-vm-setup.md has how they were built and what it costs to build
another. Measure audio in WSL.

## The Mint VM (the desktop)

`ssh isotone-vm` (docs/notes/linux-vm-setup.md, local). Linux Mint 22.3 Cinnamon
under X11, the owner's desktop. The tree is copied there with tar over ssh (git
ls-files) into `~/Isotone`, built into `~/build`. `~/desk.sh <cmd>` runs a command
in the logged-in session (DISPLAY, the session bus); `~/rig.sh` declares two null
sinks and restarts the daemon, whose user unit points at `~/build`. Screenshots:
`VBoxManage controlvm "Linux Mint Development" screenshotpng <file>` from Windows,
or the app's `--screenshot`. It stalls now and then (decisions.md, "The Mint VM,
and why it is slow"): measure audio levels in WSL, not here. Guest audio output to
the host is off in VirtualBox.

## Rules

- **Commits and pushes only when the owner says so, each time.**
- **Installers change the owner's machines.** The Windows installer, the `.deb`
  and the Flatpak are all installed on his real machines now (decisions.md,
  "Where things stand"). Running one again, or uninstalling, changes what he is
  listening through: ask first, and never run `machine-uninstall` without
  saying which outputs it will take IsoAPO off, which it lists in its dry run.
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
