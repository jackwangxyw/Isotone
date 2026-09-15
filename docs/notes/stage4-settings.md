# Stage 4, settings package

Settings General, Appearance, Shortcuts and About; in-app and global shortcuts;
the tray; closing the window; launch at sign-in; following the default output.
Built on the foundation commit 9a7fc6e (the worktree started at 375094a and was
fast-forwarded to it first).

## What was built

**General** (`SettingsGeneral.qml`, `GeneralSettings.qml` singleton). Rows and
sections as the board. `GeneralSettings` holds every General value as a typed
property that follows `AppSettings` (settings.ini gives flags back as the text
"true"/"false", so each is read as its type and held to its choices), and pushes
the spectrum options into `EqSession`.
- Launch at sign-in: `Startup` (C++) writes or removes the REG_SZ value `Isotone`
  under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, `"<exe>"` plus
  ` --tray` when Start in the tray is on (`backend/startup_registration.*`). The
  toggle shows the Run value itself, not a copy; `general/launchAtSignIn` is
  written alongside for other readers. Changing Start in the tray rewrites the
  value when there is one. The key is `HKCU\<ISOTONE_RUN_KEY>` when that is set:
  `ui_qml_tests` sets it to `Software\Isotone-tests\Run-<pid>` and deletes it at
  the end; the doctest uses a key of its own.
- Gain range: `ResponseGraph.rangeDb` is now a real property whose change bumps
  `revision`, so handles and the readout follow.
- Frequency range: `ResponseGraph.minHz`/`maxHz` replace kFMin/kFMax. The prototype's
  popover with From and To (typed as frequencies, "1.2k" works); values clamp to
  10 Hz to 24 kHz; a From at or above To is refused and the field stays selected.
- Spectrum: `SpectrumAnalyzer::set_fft_size` (4096, 8192, 16384), `set_release_ms`,
  a peak-hold line (`peak_db`, `peak_levels_at`), `set_tilt`.

**Appearance** (`SettingsAppearance.qml`, `ColourPicker.qml`, `PreviewGraph.qml`,
`PreviewSession`). Theme, six accent swatches, Custom… with the colour popover
(saturation/value square, hue slider, hex field), band colour cards, the Custom
theme's five colour rows (swatch opens the same picker, hex is typed), and the
preview on the right.

**Shortcuts** (`SettingsShortcuts.qml`, `ShortcutRegistry`, `AppShortcuts.qml`,
`BandKeys`, `GlobalHotkeys`). The table, rebinding, the conflict state with
Replace, Escape, Global toggles, global keys another app holds.

**About** (`SettingsAbout.qml`, `About`, `backend/diagnostics.*`). Mark, name,
version, Copy diagnostics (Copied with a check for 1.6 s), Engine rows,
Protected audio.

**Tray** (`TrayMenu`, `logomark.*`, main.cpp). `QSystemTrayIcon` with the logo mark,
tooltip "Isotone" and "output · preset", menu EQ (checked, shortcut text), Mute,
Output submenu (working outputs, current checked), Preset submenu
(`Presets.names`, current checked), Open Isotone, Quit. Left click opens the window.

**Window** (Main.qml, main.cpp, `SingleInstance`). Close hides to the tray or
quits; `--tray` starts hidden; a second launch shows the running window.

**Version**: `project(isotone VERSION 0.1.0)` in the root CMakeLists, passed to
`isotone_ui` as `ISOTONE_VERSION`.

## Decisions where the spec and prototype were silent

- **Peak hold** falls 6 dB per second (`kPeakFallDbPerSecond`) and never sits
  under the level; with the 300 ms release the level itself falls about 14.5 dB a
  second, so the line stays visibly above it without sticking. Drawn as the
  spectrum's edge colour at twice its opacity, 1 px, no fill.
- **Tilt** is added to the shown levels only (`levels_at`, `peak_levels_at`):
  `level + tilt * log2(f / 1000)`, 0 dB at 1 kHz. The bins, the smoothing and the
  0 to -90 dBFS scale are unchanged, so a 1 kHz tone reads the same with any tilt.
- **Release** takes 10 to 5000 ms, typed with or without "ms".
- **Graph at other ranges**: grid lines at 1, 2, 3, 4, 5, 6 and 8 in each decade
  (major at 1, 2, 5), which is exactly the foundation's set at 20 Hz to 20 kHz;
  under three lines in range, even steps of the power of ten below the span
  instead (300 to 700 Hz draws 300, 400, 500, 600). Labels at the majors, or every
  line when fewer than three majors; a label that would touch the one before it
  is left out. A band outside the frequency range has no handle; a handle beyond
  the gain range sits on the plot's edge (the curve was already clamped there).
- **Preview**: follows the prototype (the graph in a card), not the board (card
  with preset name, EQ switch and band gains). `PreviewSession` is an `EqSession`
  subclass with no output, the prototype's eight sample bands and band 4
  selected; `spectrumLevels` became virtual so it can give a fixed music-like
  spectrum (not tilted) and peak line. The real session is untouched.
- **Custom… selected** (accent -1): a 2 px text-colour ring and a dot of the
  custom colour inside the button. The swatch row has 4 px left padding so the
  selected ring is not cut by the Settings page's clip.
- **"Start from Dark"** is kept as an action: it sets the five custom colours to
  Dark's values. As a plain note it would have been an explanation.
- **Selected band keys**: the arrow pairs, [ ] and Shift are fixed; Delete can be
  rebound. Any sequence on those keys, with or without Shift, conflicts with
  their row, and Replace is not offered. Shift+[ and Shift+] arrive as { and } on
  a US layout and are taken as [ and ] (the prototype's `e.key === ']'` never
  matched with Shift held).
- **Band keys and the compat contract**: `BandKeys` filters its window's key
  events. Each press and each auto-repeat is a live edit (EqSession's apply path,
  so IsoAPO outputs hear every step); the key's release calls `finishEdit` once
  (commit), and so does the window losing focus while a key is held. Width steps
  use a new `setWidth(row, width, commitNow = false)` so a held [ or ] does not
  commit per repeat. Nothing happens while a text field has focus or with Ctrl,
  Alt or Win held.
- **Replace** takes the keys from the other action, which is left with none (its
  row shows no key caps and can be clicked to bind again).
- **Rebinding** is cancelled by Escape or by focus leaving the row's keys cell.
  While waiting for keys, in-app shortcuts are off and global hotkeys are
  unregistered, so the keys reach the page.
- **Global keys another app holds**: `RegisterHotKey` fails
  (ERROR_HOTKEY_ALREADY_REGISTERED); the row's key caps turn red with "In use"
  beside them, and the Global toggle stays on. Hotkeys use `MOD_NOREPEAT`.
- **Protected audio**: `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio`
  `DisableProtectedAudioDG` = 1 is "Disabled" (amber dot), as upstream's
  `DeviceAPOInfo::checkProtectedAudioDG` and devicetool status's
  `protected_audiodg_disabled` read it; anything else is "Enabled" with the ok dot.
- **About engine rows**: IsoAPO is installed when HKCR has the post-mix CLSID's
  InprocServer32, and its version is that DLL's file version resource; outputs are
  active endpoints with IsoAPO installed (as the sidebar). Equalizer APO is
  installed when `HKLM\SOFTWARE\EqualizerAPO` exists; version from
  EqualizerAPO.dll in InstallPath (1.4.2 here), else the uninstall entry; outputs
  are active endpoints with Equalizer APO in a slot and no IsoAPO. **IsoAPO.dll has
  no version resource**, so the row shows only "1 output" on this machine.
- **Following the default output**: `Outputs` emits `defaultOutputChanged` when the
  active default console render endpoint differs from the previous refresh
  (refresh already runs on every device notification), and
  `DefaultOutputFollower` calls `Outputs.selectDefault()` when the setting is on;
  that selects it only if it is a working output.
- **Single instance**: `QLocalServer` (a named pipe, `UserAccessOption`) named from a
  hash of the user name and the data directory, so a `--data-dir` run for tests or
  screenshots is its own instance and never brings the owner's window forward. A
  later launch sends "show" (or "tray" when started with `--tray`, which shows
  nothing) and waits for the running one's answer before closing the socket: a
  first version closed at once and the line was lost (reproduced by the doctest).
  `AllowSetForegroundWindow` lets the running instance take the foreground. If the
  name is held but nothing answers, the launch exits with code 1 and says so.
- **Screenshot runs** (`--screenshot`) skip the single instance, the tray icon and
  global hotkeys. New option `--key <keys>` presses keys after the clicks (used for
  the conflict screenshot).
- **Tray menu** rebuilds its submenus on the models' count signals, queued, so an
  item is never deleted inside its own triggered signal; current checks update in
  place.

## Integration points for the lead

- **UnsavedDialog (presets package)** (superseded: `WindowClose` calls
  `PresetActions.confirmUnsaved`, see "Fixes after review"): `Main.requestClose()` looks up
  `Qt.createComponent("Isotone", "UnsavedDialog")` when `Presets.modified` and opens
  it with `UiState.openDialog(component, { closing: true, afterClose:
  window.finishClose })`. The dialog must declare `property bool closing` (title
  "Save changes to <name> before closing?") and `property var afterClose`, and call
  `afterClose()` after Save has saved or Don't save has put the output back to its
  saved preset; Cancel only closes the dialog. Without the component the window
  closes without asking and logs a warning; there is no local fallback.
- **Undo/redo and presets**: `AppShortcuts.perform` calls `EqSession.undo()/redo()`,
  `Presets.next()/previous()/save()`; the tray calls `Presets.load(name)` and reads
  `Presets.names`/`currentName`, rebuilding on `countChanged`/`currentChanged`.
- **Auto preamp for new presets**: the presets package reads
  `AppSettings.value("general/autoPreampForNew", true)` (or
  `GeneralSettings.autoPreampForNew`).
- **API added**: `ShortcutRegistry` (QML singleton; `activate(id)` from anywhere
  performs an app action), `GeneralSettings` (QML singleton), `Startup`, `About`,
  `PreviewSession`, `BandKeys`, `Outputs.selectDefault()` and
  `Outputs.defaultOutputChanged`, `EqSession.setSpectrumOptions`,
  `EqSession.spectrumPeakLevels`, `EqSession.setWidth(row, width, commitNow)`,
  `ResponseGraph.minHz/maxHz/peakHoldVisible`, `SpectrumAnalyzer` options.

## Verified, and how

Tests (all three suites pass: `ui_tests` 20 cases, `ui_model_tests` 20 cases,
`ui_qml_tests` 75 functions including the foundation's):
- `ui_tests` (`test_settings_backend.cpp`): a sine's level and bin at 4096, 8192
  and 16384; too few samples read the floor; release 100 ms falls 0.72 dB in one
  60 Hz frame; peak hold holds, falls 6 dB/s, never under the level, resets; tilt
  0, 3 and 4.5 at 498 Hz, 996 Hz and 1998 Hz on levels and peaks, bins untouched;
  the Run value written as REG_SZ, read back, rewritten without --tray, removed,
  removing twice; the diagnostics text's sections and the About rows; file
  version text.
- `ui_model_tests` (`test_settings.cpp`): graph mapping at 12, 15 and 24 dB and at
  100 Hz to 10 kHz, revision bumps, grid and labels at 20 Hz to 20 kHz (the
  foundation's exact set), 10 Hz to 24 kHz, 1100 to 1900 Hz and 300 to 700 Hz, an
  inverted range; shortcut defaults, key caps, sequences from key presses,
  rebinding, conflicts (other action, fixed keys with Shift, own keys), Replace,
  persistence across a new AppSettings; `RegisterHotKey` keys, registration of
  Global actions, a second holder of the same keys marked failed, turning Global
  off frees them, none while capturing, all freed on destruction, WM_HOTKEY
  activates; the tray menu's eight items, EQ and Mute checks both ways, shortcut
  text following a rebinding and a Replace, the Output submenu against the real
  `Outputs` (checked row, selecting another), the Preset submenu against the stub,
  Open and Quit signals, the tooltip; the preview session; spectrum options
  reaching the session; the single instance (a later launch on another thread
  shows the window; a --tray launch does not).
- `ui_qml_tests`: `tst_settingsgeneral` (every toggle persists and reads back from
  text, peak hold reaches the graph, launch at sign-in writes the test key with and
  without --tray, gain range moves the graph's scale and a handle, the frequency
  range popover including refusal, clamping, text that is not a frequency, a
  handle hidden out of range, and Escape; resolution, tilt, release typed and
  refused); `tst_settingsappearance` (theme, accent, band colours, custom rows and
  Start from Dark switch Theme tokens; the colour popover's hex field, square and
  hue bar; the preview has its own sample); `tst_settingsshortcuts` (key caps,
  rebinding, Escape, a press elsewhere, conflict and Replace, fixed-key conflict
  without Replace, fixed rows not rebindable, Delete rebinds, Global, "In use");
  `tst_appshortcuts` (activate performs EQ and Mute, window keys and a rebinding,
  nothing while capturing, band gain/frequency/Q steps with coarse, one commit per
  press and one per hold, nothing while typing or with Alt or when inactive,
  following the default output gated by the setting, on a stand-in `Outputs`);
  `tst_settingsabout` (version, rows, Copy diagnostics pasted back from the
  clipboard, Copied reverting); `tst_settingsminimum` (every page fits across at
  1120 x 760 with the rail, and scrolls to its end).

Mutation checks: 43 mutations, listed with their results at the end of this file.

Live, with the built app (scratch data dirs, `--output` CABLE Input, test Run key):
- While the app ran, `RegisterHotKey(Ctrl+E)` and `(Ctrl+Right)` from another
  process failed with 1409 (held); after it exited, Ctrl+E registered again.
- WM_CLOSE with Keep running in the tray on (default): the process stayed, the
  window hid. A second launch on the same data dir exited 0 in 555 ms and the
  hidden window was visible again.
- Keep running in the tray off: WM_CLOSE ended the process.
- `--tray`: no visible window, process alive; a second `--tray` launch exited 0 and
  showed nothing; a plain second launch showed the window.
- `QCoreApplication::quit()` (what the tray's Quit calls) ended the app with the
  window visible and refusing its close, checked with a temporary timer: a
  `quitting` flag added for a suspected cancelled quit was not needed and removed.
- The real Run key had no Isotone value before and after; no test key was left.

Screenshots (1440 x 900, compared with the boards and the prototype): General, the
frequency range popover, Appearance, Custom theme with the accent popover, Custom
rows, Shortcuts, Press keys, the conflict with Replace, About, the Appearance
preview with peak hold, and the Equalizer view at 10 Hz to 24 kHz with ±24 dB and
at 300 to 700 Hz with ±12 dB. Differences left: the tab bar sits about 12 px lower
than on the boards (SettingsView, foundation); the board's Appearance headers are
muted and its preview is a card with preset name and band gains, where the
prototype, which was followed, has h6 headers and the graph alone.

## Not verified

- Following the default output on a real default change: it would change the
  owner's audio settings. The gate on the setting is tested with a stand-in; the
  detection in `Outputs::refresh` is not.
- The tray icon and menu on the desktop: the menu's structure and actions are
  tested, the icon was not looked at.
- Launch at sign-in actually starting the app at sign-in (only the test key was
  written).
- Peak hold and tilt on the live graph with real audio (only the surround
  package plays audio); seen on the preview, measured in the analyzer tests.
- Signing out or shutting down with the window open: the window refuses its close
  (hides to the tray); whether that delays Windows' session end was not tried.
- `Outputs.selectDefault()` and `defaultOutputChanged` on this machine's devices.

## Problems found

- **Global defaults take keys from every other app.** Global on for EQ (Ctrl+E),
  Mute (Ctrl+M), Next and Previous preset (Ctrl+Right, Ctrl+Left), as the board
  shows, means while Isotone runs those keys do nothing elsewhere: word navigation
  in every text editor, Ctrl+E in browsers. Verified: another process could not
  register them. The owner should decide the defaults.
- **IsoAPO.dll has no version resource**, so About cannot show its version. Adding
  a VERSIONINFO built from `PROJECT_VERSION` to windows/apo (not this package's)
  would fill it.
- **TextBox.qml could not load** ("Type TextBox unavailable": a Column's
  `implicitWidth` is read-only in Qt 6.11). Fixed with `width: 240`; any package
  using TextBox needs this.
- A hand-written `[general]` section in settings.ini is read as top-level keys;
  QSettings itself writes the group as `[%General]`. Only matters when editing the
  file by hand.
- CABLE Input was 7.1 (the surround package's live work) during some of these
  screenshots.

## Shared files changed

Root `CMakeLists.txt` (project VERSION), `ui/CMakeLists.txt`, `ui/backend/spectrum.*`,
`ui/src/eqsession.*` (virtual spectrum reads, peak levels, spectrum options,
`setWidth` commitNow), `ui/src/outputs.*` (default output), `ui/src/responsegraph.*`
(ranges, peak hold, grid), `ui/src/main.cpp` (tray, hotkeys, single instance, --tray,
--key), `ui/qml/Main.qml` (shortcuts, band keys, follower, close), `ui/qml/GraphCard.qml`
(range and peak bindings, handle visibility and clamp, objectNames),
`ui/qml/TextBox.qml` (the width fix), `ui/tests/qml_main.cpp` (test Run key),
`ui/tests/test_model.cpp` (QApplication for the tray menu tests).

## Follow-ups

- Owner: the Global defaults (above).
- IsoAPO version resource.
- Presets package: UnsavedDialog per the contract above; `Presets.modified`.
- A `Presets.changed`-style signal would let the tray tooltip follow a preset rename
  (it follows `currentChanged` today).
- A way to observe commits (a signal on EqSession, which the undo work may add)
  would let a test catch a held [ or ] committing per repeat (mutation Q16 below).

## Mutation checks

Each: the code broken as described, the target rebuilt, its suite run, the code
restored (script `%TEMP%\iso-settings\mutate.py`). Suites: B `ui_tests`, M
`ui_model_tests`, Q `ui_qml_tests` on the named file.

| Mutation | Result |
|---|---|
| B1 peak hold never falls | caught (peak hold test) |
| B1b peak hold drops at once | caught (peak hold test) |
| B2 tilt pivots at 500 Hz | caught (tilt test) |
| B3 level scaled for 8192 at every size | caught (resolution test) |
| B4 release setting ignored | caught (release test) |
| B5 Run value without --tray | caught (Run value test) |
| B6 remove deletes another value name | caught (Run value test) |
| B7 diagnostics without "default" | caught (diagnostics test) |
| B8 About row shows a dot with no version | caught (About rows test) |
| M1 gain range does not bump revision | caught (graph mapping) |
| M2 x mapping ignores maxHz | caught (graph mapping) |
| M3 no fallback grid for a narrow range | caught (grid test) |
| M4 Right is no conflict | caught (rebinding test) |
| M5 Replace leaves the other its keys | caught (rebinding test) |
| M6 Global off by default | caught (defaults) |
| M7 taken hotkey not reported | caught (global hotkeys) |
| M8 hotkeys registered by a rebinding while capturing | not caught at first; test extended (a rebinding while capturing), then caught |
| M9 tray EQ always checked | caught (tray menu) |
| M10 tray Mute without its keys | caught (tray menu) |
| M11 running instance ignores "show" | caught (single instance) |
| M12 resolution not applied to the session | caught (spectrum options) |
| M13 preview selects no band | caught (preview session) |
| Q1 Start in the tray does not rewrite the Run value | caught (launch at sign-in) |
| Q2 graph ignores the gain range | caught (gain range) |
| Q3 From at or above To accepted | caught (range popover) |
| Q4 range not clamped | caught (range popover) |
| Q5 flags read as truthy text | caught (toggles persist) |
| Q6 handle shown outside the range | caught (range popover) |
| Q7 custom accent not selected | caught (accent popover) |
| Q8 invalid hex not selected | caught (accent popover) |
| Q9 conflicting keys rebound anyway | caught (conflict and Replace) |
| Q10 Escape does not cancel | caught (Escape) |
| Q11 Replace offered for fixed keys | caught (fixed-key conflict) |
| Q12 "In use" never shown | caught (keys another app holds) |
| Q13 release does not commit | caught (one commit per press and hold) |
| Q14 band keys act while typing | caught (not while typing) |
| Q15 no coarse steps | caught (band steps) |
| Q16 a held [ or ] commits per repeat | **not caught**: a commit has no observable effect without an output |
| Q17 shortcuts active while capturing | caught (keys in the window) |
| Q18 follows the default output with the setting off | caught (following) |
| Q19 Copy diagnostics copies nothing | caught (copy diagnostics) |
| Q20 invalid release submitted | caught (release typed) |
| Q21 `ColourPicker.load`'s parameter named `value` (the first version) | caught, by a test added after reading the code: the parameter shadowed the `value` property, so opening the picker never moved the square's knob to the colour |

Two first attempts (B1 as "no hold", B6 as "no delete") did not compile under
warnings as errors (an unused variable); they were replaced by B1, B1b and B6 above.
The Theme.qml custom colour read was suspected of not re-reading (a bare
`AppSettings.themeRevision` statement); run against the unchanged Theme.qml the
custom colour tests pass, so Theme.qml was left as it was.

## Fixes after review

A review confirmed these with a harness that copied Main's pieces. Main cannot be
loaded in a test (it attaches the real current output), so its key, press and close
logic moved into components Main and `tst_windowkeys.qml` both use: `WindowKeys`
(Delete, `AppShortcuts`, `BandKeys`), `PressWatch`, `WindowClose`. The test lays
them out as Main does with the real `BandMenu`, `PresetsMenu`, `PresetPrompts`, a
`Popover` and `FirstRun` in a Loader; the window is a stand-in with show and hide
counters, so nothing hides or quits.

1. **Keys acted on the band behind dialogs, popovers and first run.** `DialogFrame`
   and `Popover` give the focus to their card or panel, which does not stop window
   Shortcuts or `BandKeys`' window filter. Fix: `WindowKeys.held` is true while
   first run is active or the focus item is visible and inside the overlay; Delete,
   the in-app shortcuts (`AppShortcuts.keysActive`) and `BandKeys.active` follow it.
   `ShortcutRegistry.activate` (global hotkeys, tray) still performs. A closed
   popover's hidden panel keeps the focus (seen in a logged run: Qt leaves active
   focus on the invisible panel), so a hidden focus item holds nothing; without that,
   Escape on a popover left the band keys dead until a press.
   Tests: Delete, Shift+Up and Ctrl+Z change nothing under the unsaved dialog, the
   presets popover, the band menu, the outputs popover and first run; keys act with
   the focus in the window, come back after Escape on a dialog and on a popover and
   after a press closes a popover; global activation still toggles EQ under a dialog.
2. **A press inside a dialog moved the focus to content.** Fix (`PressWatch`): when
   the focus item is inside the overlay, the focus goes to its nearest ancestor that
   holds the press, below the dialog or popover item itself (which fills the
   window); a press outside the card or panel, or with the focus elsewhere, goes to
   content as before. Tests: a press on the Save as title keeps the focus in the
   dialog, Delete does nothing and Escape closes it; a press on the presets panel off
   its search field keeps Escape; a press elsewhere ends typing.
3. **A second close or tray Quit stacked another unsaved dialog.** Fix
   (`WindowClose`): one pending dialog (cleared on its `closed`); a repeat shows and
   raises the window and asks nothing; a Quit while it is open makes Save or Don't
   save quit instead of hiding; Cancel forgets the Quit. Tests: two closes, one
   dialog, one hide; close, hide, Quit: the window is shown, one dialog, Don't save
   quits with no second hide; Cancel then close asks again; nothing unsaved hides
   or quits at once.
4. **Ctrl+S did nothing on an untitled output.** Fix: `AppShortcuts` calls
   `PresetActions.save()`. Test: Ctrl+S on an untitled output opens Save as.
5. **A preset picked from the tray or a Next/Previous hotkey asked in the hidden
   window.** Fix (`PresetPrompts`): `UiState.showWindow(host)` (show, or showNormal
   when minimized; raise; requestActivate) before the dialog, and one unsaved dialog:
   a later pick while it is open changes the preset loaded after it. `host` is the
   item's window; the test gives the stand-in. Test: with the window hidden,
   `Presets.load` shows it while no dialog exists yet, `Presets.next()` adds no
   dialog, Don't save loads B.
6. **Tray menu key text.** "Ctrl+E" and "Ctrl+M" showed beside EQ and Mute with
   Global off (the default), where the keys do nothing. Fix: `TrayMenu` shows the
   keys only when `isGlobal`. Test (doctest "the tray menu"): no keys by default,
   keys after Global on, gone again after Global off.
7. **Custom theme.** Only five tokens followed the custom colours and `dark` was
   always true, so a light custom background kept dark pop, surface, border and
   segmented colours (text on them unreadable) and the dark accents and status
   colours; `textOnAccent` ignored a light custom accent. Fix (`Theme.qml`): Custom
   is dark when its background's luminance is not above its text's, so the tokens
   it does not set come from the matching built-in set; surface, gridMinor, muted,
   track, selectedColumn and border are mixed from background to text at the share
   Dark or Light uses (surface 4.3% or 5.6%, muted 60% or 66%, and so on); pop and
   segmentedSelected are the custom surface on a light Custom (Light uses its plot
   colour there) and mixed on a dark one. `textOnAccent` for a custom accent is
   whichever of the two text colours has the higher contrast. Still five user-set
   colours. Test (`tst_settingsappearance`): for a light (#f5efe6, #fffaf3, #2b2520)
   and a dark (#0c1722, #08111a, #dce6f0) set, `dark` follows; text at least 7:1 on
   background, plot, surface, pop, track, segmented and selected column; muted 4.5:1
   on background and pop; border and track visible; the mixed tokens lie between
   background and text; Start from Dark gives tokens within 16/255 (summed over
   channels) of Dark's; text on #ffe08a and #1a3a7a at least 4.5:1 in Custom and
   Light.

**Checked, not a bug: `--key` and Shortcuts.** On a sandboxed run (`--data-dir`,
`--compat-dir` scratch, `--output` CABLE Input) `--add-band 1000,3 --key Delete`
deleted the band (the "Band 1 deleted" toast shows), and `--key Ctrl+E --key
Ctrl+M` turned EQ off and Mute on: `sendEvent` to the QQuickWindow reaches QML
`Shortcut`s under QApplication. main.cpp is unchanged. After the fixes, `--click
320,38 --key Delete --key Up` (presets popover open) left the band and its gain as
they were. CABLE Input showed "Native" in the sidebar during these runs.

Mutation checks (script `mutate.py` in the session scratchpad; each built, its test
file run, restored):

| Mutation | Result |
|---|---|
| W1 `held` always false | caught (the five "not under" tests, press in a dialog) |
| W2 first run not held | caught (first run) |
| W3 a hidden focus item held | caught (keys come back when a popover closes) |
| W4 AppShortcuts ignores `keysActive` | caught (Ctrl+Z under each of the five) |
| W5 BandKeys ignores `held` | caught (Up under each of the five) |
| W6 Delete ignores `held` | caught (Delete under each of the five) |
| W7 Ctrl+S calls `Presets.save()` | caught (Ctrl+S on untitled) |
| P1 a press always moves the focus to content | caught (press in a dialog, press in a popover) |
| P2 a press may leave the focus on the popover item itself | caught (keys come back when a popover closes) |
| C1 no pending check, close stacks dialogs | caught (second close, Quit while closing) |
| C2 Quit while closing hides | caught (Quit while closing, nothing unsaved) |
| C3 a repeat does not show the window | caught (Quit while closing) |
| R1 a tray pick does not show the window | caught (tray pick) |
| R2 the window shown after the dialog is made | caught (tray pick) |
| R3 tray picks stack dialogs | caught (tray pick) |
| T1 tray keys without Global | caught (the tray menu) |
| H1 Custom always dark | caught |
| H2 built-in values on Custom, no mixing | **not caught at first** (Light's greys read fine on a light custom set); the "between background and text" check was added, then caught |
| H3 light Custom pop mixed instead of the surface | caught (muted on pop 4.39) |
| H4 `textOnAccent` ignores a custom accent | caught (#fafcfe on #ffe08a, 1.25) |
| H5 muted share 0.3 | caught (muted on background 1.84) |

Not verified: closing and tray Quit on the running app (the components are tested
with a stand-in window; Main with them loaded and took keys in the `--key` runs); a
Custom theme looked at in a screenshot.

Left as it is: the unsaved dialog for a preset pick and the one for closing are
separate, so closing while a pick's dialog is open still opens the close dialog on
top of it. A Global action whose keys another app holds ("In use") still shows its
keys in the tray menu.

Shared files changed: `ui/qml/Main.qml` (uses `WindowKeys`, `WindowClose`,
`PressWatch`; `requestQuit` kept for main.cpp; `requestClose` and `finishClose`
removed, nothing else called them), `ui/qml/UiState.qml` (`showWindow`),
`ui/qml/Theme.qml`, `ui/qml/PresetPrompts.qml`, `ui/qml/AppShortcuts.qml`,
`ui/src/traymenu.cpp`, `ui/CMakeLists.txt`, `ui/tests/test_settings.cpp`,
`ui/tests/qml/tst_settingsappearance.qml`.
