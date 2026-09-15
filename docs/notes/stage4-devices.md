# Stage 4, devices work package

Everything about outputs and engines: the Devices view, device operations through
devicetool, the Uninstall, Replace and Attach dialogs, Settings Outputs, first run
and the top bar's status pill.

## What was built

C++ (`ui/src`, QML singletons unless noted):

- `Devices` (`devicesmodel.*`): every render endpoint. Status and engine from
  `windows/devices` (`enumerate_render_endpoints`, `read_engine` in it); each
  output's effect slots, remedies and install modes from `isotone-devicetool status`
  run unelevated (about 40 ms each, measured on this machine). Read on a worker
  thread at start, on `DeviceWatcher` events (queued, 250 ms debounce), on Refresh,
  after every devicetool run (`Main.qml`), and when a 3 s poll of `read_engine`
  finds an engine changed. `row(guid)`, `operation(guid, action)`,
  `plan(guid, want)`, `copyDiagnostics(guid)`, `openEqualizerApoUninstaller()`,
  `defaultGuid`, `equalizerApoInstalled`, `equalizerApoVersion`, `equalizerApoUsed`,
  `revision`.
- `devicestatus.*` (not QML): the status, engine labels, actions and plans as pure
  functions of `DeviceFacts`, filled from status JSON or from an endpoint read.
- `Devicetool` (`devicetoolcontroller.*`): one runner on one worker thread for the
  app's life; `run(kind, guid, args)`, `requestApproval()`, `apply(plans)`,
  `retry()`, `clear()`, `copyDetails()`, `restartWindows()`; `phase`, `kind`,
  `target`, `reason`, `elevated`, `working`, `rowStatus`, `restarted`; `finished`.
- `devicetoolrunner.*` (not QML): `DevicetoolRunner` (the injectable interface),
  `SessionRunner` (`DevicetoolSession`, elevated or, for tests, a plain child;
  read-only commands as a direct child), `ScriptedRunner`, `devicetoolPath()` /
  `setDevicetoolPath()` (beside isotone.exe, else the build tree's), and
  `runDevicetoolDirect`.
- `EqualizerApoConfig` (`equalizerapoconfig.*`): the Attach dialog's `preview()` and
  `attach(removePeace)`, on `AppPaths::compatConfigDir()` or the install's
  ConfigPath.
- `ui/backend/config_attach.*` (no Qt): the preview (config.txt's lines, the Peace
  include, the lines `attach_include` appends, found by running it on a copy in a
  temporary directory) and the attach with the Peace include kept or removed.

QML: `DevicesView` (replaces the placeholder), `DeviceDetail`, `OperationBox`,
`UninstallDialog`, `ReplaceDialog`, `AttachDialog`, `OutputSetupTable`,
`SettingsOutputs` (replaces the placeholder), `FirstRun`.

App options (`main.cpp`): `--fake-devicetool <script>` (devicetool's answers and the
Devices outputs from a script: `ScriptedRunner` in `devicetoolrunner.h`, and
`devicesmodel.h`), `--first-run`, `--view <view>[/<tab>]`.

## Status mapping

First match wins (`devicestatus.h`):

| Status | From | Dot |
|---|---|---|
| Unplugged | endpoint not active (disabled or unplugged) | off |
| Interrupted | `isoapo.state` interrupted | warn |
| Unrecorded | unrecorded | warn |
| Conflict | alongside_equalizerapo | bad |
| Enhancements off | installed, `device.enhancements_disabled` | warn |
| Installed | installed | ok |
| Replaced by Equalizer APO | replaced_by_equalizerapo | warn |
| Detached | detached | warn |
| Active | not_installed, backend equalizerapo | ok |
| Not installed | not_installed | off |

Endpoints whose hardware is gone (`DEVICE_STATE_NOTPRESENT`, 25 of 36 render
endpoints here) are not listed.

Engine column: Native, Equalizer APO, Native + Equalizer APO from the backend;
Native for IsoAPO's record without its slot (detached, interrupted); a dash
otherwise. The detail's Engine row follows `devDetail` (a dash for not installed and
unplugged, "Native (IsoAPO)", "IsoAPO + Equalizer APO").

Actions and what they run:

| Status | Actions | Runs |
|---|---|---|
| Installed | Test, Uninstall | `test <g>` unelevated; `uninstall <g>` after the dialog |
| Active | Replace with IsoAPO, Test | the Replace dialog: `install <g> --replace-equalizerapo`, or Attach |
| Detached | Repair, Test, Uninstall | `repair`, or `repair --mode <m>` when the remedy is `repair --mode` (m from `default_install_mode`) |
| Detached, Equalizer APO in a slot | Take back, Keep Equalizer APO | `install <g> --replace-equalizerapo`; `uninstall <g>` |
| Conflict | Remove Equalizer APO, Uninstall IsoAPO | `install <g> --replace-equalizerapo`; `uninstall <g>` |
| Replaced by Equalizer APO | Take back, Keep Equalizer APO | as above |
| Interrupted | Undo | `repair` |
| Unrecorded | Copy diagnostics | the status JSON to the clipboard |
| Enhancements off | Turn on enhancements | `enable-enhancements <g>` |
| Not installed | Install | `install <g>` |

## Decisions where the spec and prototype are silent

- **Every change, then restart and test.** install, uninstall, repair, replace and
  enable-enhancements run `restart-audio`, then `test` on the output operated on and
  on every output `repair` reattached. enable-enhancements too: Windows loads the
  endpoint's effects again only when the audio service restarts. A failing
  restart-audio ends in reboot (nothing is tested); a failing test is `failed` with
  kind `test` ("Test failed" and its reason).
- **Test never asks for approval** (the prototype's `runOp('test', false)`): it runs
  as an unelevated child, also after a change.
- **Reason sentences.** devicetool's `reason` (for repair, the failing endpoint's
  `error`) with a capital and a full stop; a session failure is Windows' own message
  (FormatMessage, English). Busy shows "Another install is running", as the
  prototype.
- **Enhancements off and Undo** show Repairing / Repaired, as the prototype wires
  both to its repair operation.
- **Detached with Equalizer APO in a slot** gets Take back / Keep Equalizer APO:
  devicetool's remedies there are `install --replace-equalizerapo` and `uninstall`,
  the same as for replaced, and `repair` refuses it.
- **Uninstall dialog, Restores:** "Equalizer APO" where Equalizer APO is in a slot of
  the output (uninstall keeps it), "Driver effects" otherwise.
- **Attach dialog** shows the block `attach_include` really appends (three lines, or
  more with open Ifs or Stage lines), not only the Include line. The Peace row is
  shown only when config.txt has a Peace include. Remove include deletes that Include
  line with `write_file_atomically`; no extra backup is written (every new name in
  Equalizer APO's directory makes it reload), so `attach_include`'s own backup is of
  the file without it. Attach failing shows Windows' sentence in the dialog.
- **Attach needs no elevation.** Equalizer APO's config directory grants Users full
  control (`icacls` on this machine: `BUILTIN\Users:(OI)(CI)(F)`, inherited by
  config.txt), and the compat library only reads, appends to and atomically
  replaces files there. Where it is not writable the attach fails with "Access is
  denied."; devicetool has no attach command to elevate.
- **Settings Outputs choices** (`planChange`):
  IsoAPO: `install <g>`, with `--replace-equalizerapo` where Equalizer APO is on the
  output. Equalizer APO: `uninstall <g>` then attach (Peace kept, no dialog). Off:
  `uninstall <g>` where IsoAPO is or was, and the output's block removed from
  Isotone.txt where Equalizer APO is on it; Equalizer APO itself stays in the
  output's slots (only its uninstaller removes it). The table lists active outputs
  only. Defaults: what the output has, IsoAPO where both are on it (the prototype's
  `cfgDefaults(false)`). Apply runs the plans in order; a failed row is marked
  Failed and the rest still run; audio restarts once if any devicetool change
  succeeded, then every output a devicetool command changed is tested. The bar shows
  "Applied · audio restarted" (or "Applied" with nothing to restart), the first
  failure's reason, or "Applied · audio did not restart" with Later / Restart Windows.
- **First run** proposes IsoAPO for every active output (the prototype's defaults
  name mock outputs). It is a Loader over the whole window in `Main.qml` (z 1500),
  shown at start when `general/firstRunDone` is not set and `Outputs.count` is 0, or
  by `showFirstRun()` (`--first-run`). While Windows asks, step 1 shows "Waiting for
  administrator approval" in the button row; declined goes back to Outputs; done,
  failed or reboot go to Ready (a failure's reason or "Audio did not restart" with
  Restart Windows beside Open Isotone). Skip and Open Isotone set the setting.
- **Top bar pill: it can occur.** Engine changes raise no notification, so an output
  IsoAPO is detached from (a driver update) stays current until the next Outputs
  refresh. `Devices` polls `read_engine` every 3 s; the top bar shows the current
  output's status when it is not installed or active (and not unplugged), with
  Install for Not installed and Repair otherwise, which open Devices on it. With no
  working output at all it shows the default output's. After a repair, Outputs is
  refreshed and keeps the output; after an uninstall it moves to another.
- **Restart Windows and Equalizer APO's uninstaller** are signals
  (`restartWindowsRequested`, `equalizerApoUninstallerRequested`) connected only in
  `main.cpp`: `shutdown.exe /r /t 0`, and ShellExecute of `UninstallString`. With
  `--fake-devicetool` they print instead. Tests cannot reach either.
- **Ordering:** active outputs first, the default first among them, then by name.
- **Table layout:** Devices sizes its columns as the prototype's HTML table does
  (content widths, the rest shared in proportion, Engine and Status wrap when there
  is too little room); Settings Outputs uses fixed widths for Format 130, Now 190,
  Engine 250 and the status column 170 in its 1000 px.
- **Empty values** use U+2014, the glyph the prototype and Devices.png draw for "a
  dash". It is written as a code point, not typed, and there is none in comments or
  docs. Lead: say if the no-em-dash rule should cover this glyph too.
- **Detail rows:** Config shows "config.txt · Isotone.txt" once config.txt includes
  Isotone.txt for every device, else "config.txt". Effect slot lists IsoAPO's slots
  then Equalizer APO's ("MFX + SFX"); a detached IsoAPO shows the slot its record
  names.

## Changes to shared files

- `ui/CMakeLists.txt`: Test in `find_package`; the new sources and QML files; link
  `isotone_devicetool_session`, `ISOTONE_DEVICETOOL_BUILD_PATH`, a dependency on
  `isotone-devicetool`; `test_config_attach.cpp` in ui_tests, `test_devices.cpp` and
  Qt6::Test in ui_model_tests; `ISOTONE_FAKE_DEVICETOOL_SCRIPT` for ui_qml_tests.
- `ui/src/outputs.h`, `outputs.cpp`: `currentGuid`.
- `ui/src/main.cpp`: `--fake-devicetool`, `--first-run`, `--view`; the two signals
  connected; `--click` events stamped 10 s apart on their clock, because two
  `--click`s 100 ms apart were delivered as a double click and a button clicked
  right after another never fired (seen in the Replace dialog screenshot).
- `ui/qml/Main.qml`: first run Loader and `showFirstRun()`; Devices and Outputs read
  again on `Devicetool.finished`.
- `ui/qml/TopBar.qml`: the status pill, after the preset name.
- `ui/tests/qml_main.cpp`: `ISOTONE_FAKE_DEVICETOOL` (tests/devices/fake-devicetool.json)
  and a sandbox `ISOTONE_COMPAT_DIR` with a Peace config.txt for every QML test.

## Verified, and how

- **doctest, ui_tests** (+6 cases, `test_config_attach.cpp`): preview lines, Peace
  flags and the appended block (also after an open If and no final line break),
  attach with Peace kept and removed, both run twice, what counts as a Peace include,
  a missing config.txt. Sandbox directories under %TEMP%, checked not to be the live
  install.
- **doctest, ui_model_tests** (+21 cases, `test_devices.cpp`): every status from status
  JSON (14 samples in `ui/tests/devices`: 4 captured here on 2026-09-15 with
  `isotone-devicetool status`, read-only, and 10 `status-derived-*` made from them
  with a Python script that changes only the fields each state reads);
  name, format, default and remedies; every action's command; every Settings Outputs
  plan; the in-process read against devicetool status on all 11 present render
  endpoints here (status, engine, format, GUID, present agree); the controller
  through a scripted runner: success with approval, restart and test, no second
  approval, failure with reason and Retry, repair's reason and tests, busy, declined
  and Retry, a failing approval, restart-audio failing (reboot, nothing tested), a
  failing test, unelevated Test, a broken session, Change's approval, apply (order,
  row states, attach and block removal on a sandbox, one restart, tests), a failed
  plan, no overlap while working, the script format; and the controller on a real
  `serve` launched unelevated: `enable-enhancements` on CABLE Input answers exit
  code 3 and shows as failed with devicetool's reason, and `status` runs directly
  (the test refuses to run elevated).
- **Qt Quick Test** (4 new files): `tst_devices.qml` (selection, dimming, actions per
  state, labels and kinds, config row, preset row, the op box through approval,
  running, restarting and done, one output only, failure with reason and Retry, busy,
  declined, reboot and Later, enhancements, both Uninstall dialogs, Replace and its
  preselection, Keep opening Attach, Attach on the sandbox removing Peace),
  `tst_settingsoutputs.qml` (locked table, Change with approval, declined, Equalizer
  APO offered only where present, counts and Cancel, applying to done with row
  states, a failed row), `tst_firstrun.qml` (proposals, approval to Installing to
  Ready and the setting, nothing to install, declined, Skip), `tst_devicespill.qml`
  (Repair and Install pills, none for a working output, the Equalizer APO card and
  its uninstaller signal).
- **A bug the QML tests found:** the Attach dialog was created from a component made
  in the Replace dialog's context; once the Replace dialog closed, the Attach
  dialog's buttons did nothing. It is now opened by DevicesView.
- **Mutation checks:** 36 mutations, each applied by a script, built, the guarding
  test run, then restored; all 36 made a test fail. One first survived (counting a
  replaced output as having Equalizer APO even with none in its slots): that clause
  could change nothing the tests or the states reach, and was wrong for a replaced
  output Equalizer APO has since left, so it was removed and the check replaced by
  one on the clause that remains. Two first failed to compile under /WX and were
  rewritten so they build. Broken on purpose: the Peace key check, Remove include
  ignored, the preview's leading line break, Peace lines unmarked; Enhancements off
  and Unplugged as statuses, Equalizer APO in a slot, `repair --mode`, install
  without `--replace-equalizerapo`, detached with Equalizer APO repairing,
  unplugged read as present, whole kHz; a failed restart ignored, exit 4 not busy,
  declined as a failure, repaired outputs not tested, no approval, overlapping
  operations, reasons not sentences, apply without attach or block removal, a failed
  plan stopping the rest, Test run in the session, exit 3 taken as success; in QML,
  selection keeping a finished operation, Restores always driver effects, Equalizer
  APO preselected, Peace never removed, the Attach dialog made in the closing
  dialog's context, first run counting like Settings, Change without approval, the
  pill for working outputs, the Equalizer APO card ignoring its outputs, first run
  ignoring a decline, the busy text, actions ignoring the state.
- **Screenshots** of every Devices, Settings Outputs and first run state the
  prototype's SCREENS lists, and the top bar's Detached pill, with
  `--fake-devicetool`, sandbox data and compat directories and `--output` CABLE
  Input, compared with the prototype and Devices.png. The outputs are the prototype's
  plus an interrupted and an unrecorded one, so every state has a row.
- All three suites pass after a full build: ui_tests 17 cases, ui_model_tests 28
  cases, ui_qml_tests 85 passed (counting each file's init and cleanup).

## Not verified

- The real approval prompt and its decline, and any real install, uninstall, repair,
  enable-enhancements, restart-audio or test run from the app (the rules forbid it
  here).
- Restart Windows and launching Equalizer APO's uninstaller (never clicked).
- Attach against the installed Equalizer APO's directory.
- A DeviceWatcher event or the 3 s poll catching a real engine change.
- Closing the app while a real command runs: the destructor joins the worker, which
  waits for the command (an install can wait a minute for the machine lock).
- Presets integration: the stub returns no names, so Preset shows a dash or None.

## Follow-ups

- `status` runs one endpoint at a time on refresh; with many endpoints it could run a
  few in parallel.
- `Devicetool.loadScript` and `Devices.loadScript` are test hooks in the QML API.
- A devicetool reason is technical ("pass --replace-equalizerapo"); the prototype's
  sentence was user-facing. The UI shows devicetool's as the brief asks.
