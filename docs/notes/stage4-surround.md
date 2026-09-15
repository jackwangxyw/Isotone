# Stage 4, surround package

Everything for outputs with more than two channels: the Speakers panel, band
targets, the Showing picker, the Speakers view with its dialogs, test tones, solo,
and saving the speaker setup. Branch `worktree-agent-a095d016ac6398094`, from
the foundation commit 9a7fc6e.

## What was built

**Backend, no Qt** (`ui/backend`)
- `speaker_setup.h/.cpp`: the speakers of a layout (code, name, channel), the
  groups as masks (All, Front L C R, Surround SL SR RL RR, Sub LFE, then the
  user's), a band's target label, the solo mute mask with the LFE rule, the live
  overrides (solo, test tones), distance and delay, bass management steps,
  `saved_with_speakers` and `save_speaker_setup` (the saved state's speaker
  part), and the graph's channel choice for a view.
- `pink_noise.h/.cpp`: the test tone. A periodic loop (at least 4 s, a power of
  two) built in the frequency domain: every bin from 20 Hz to 20 kHz at power
  1/f with a random phase from a fixed seed, scaled to exactly -30 dBFS RMS.
- `test_tone.h/.cpp`: `TestTone`, WASAPI shared mode on one channel of an
  endpoint, every other channel silent, on its own thread; the render loop
  follows `windows/measure`'s `RenderStream` (mix format, 200 ms buffer, topped
  up to 50 ms every 10 ms). Failures come back through a callback.
- `typed_value`: two units, `Metres` ("m") and `Milliseconds` ("ms").

**Qt** (`ui/src`)
- `Speakers` (QML singleton, list model over the current output's speakers):
  levels, distance or delay, polarity, mute, solo, small speakers, crossover,
  LFE low-pass, upmix, swaps, lip sync, test tones, groups, the Showing picker's
  items and key, supported layouts and `setLayout`. Every change goes through
  `EqSession::editSpeakers`.
- `SpeakerStore`: `speakers.json` in the data directory, per output GUID: the
  user's groups (by speaker code) and the farthest speaker's distance.
- `EqSession` (shared, additive): `useTarget` (the body of `useOutput`, which
  now calls it), remapping on a layout change and on load, `ChannelMaskRole`,
  `setChannelMask`, surround `TargetRole` labels, `showingMask`,
  `setUserGroups`, `editSpeakers`, `setLiveOverrides`, `engineState` (what the
  output gets) and `savedState` (without overrides), `setSavedStateDir` for
  tests, `speakerSaveFailed`, units `Metres` and `Milliseconds`. `push` and
  `commit` now write `engineState()`.
- `ResponseGraph` (shared, additive): the surround branch of `viewChannel`,
  `handleDb`, `onView`, and dashed lines for other channels in view.

**QML** (`ui/qml`)
- `SpeakersPanel.qml`, shown by `BandStrip.qml` instead of `ChannelsPanel` when
  `EqSession.outputChannels > 2`.
- `TargetChips.qml`, the Target row in `BandMenu.qml` on surround outputs.
- `ShowingPicker.qml`, in `TopBar.qml` instead of L / R / L+R on surround.
- `SpeakersView.qml` (replaces the placeholder), `NewGroupDialog.qml`,
  `LayoutDialog.qml`.

## Decisions where the spec and prototype were silent

- **Distances are read back from the delays.** The engine keeps delays only. The
  store keeps one number per output, the farthest speaker's distance; each
  speaker's distance is `farthest - delay * 343 m/s`. Editing a distance
  recomputes every delay from the distances, and the farthest becomes the
  largest distance. A typed delay keeps the other distances; when it puts the
  speaker nearer than 0 m, the farthest grows. With nothing kept, the farthest is
  3.00 m, so an output that was never set up shows every speaker at 3.00 m and
  0 ms. Reason: no second copy of the delays that could disagree with what the
  engine plays.
- **Bass management is on while any speaker is small.** The view has no on/off
  control; ticking the first small speaker turns it on, clearing the last turns
  it off. The panel's Crossover row reads "Off" while it is off. The LFE is never
  small.
- **Speaker chips add or remove a speaker; group chips set the target.** The
  prototype's chips were single-select. A band keeps at least one speaker; every
  speaker is stored as all channels (mask 0). A group is lit when the band is
  exactly that group, a speaker when the band is on it.
- **Target labels:** "All", a group's name when the mask is exactly a group
  (built-in groups first), a speaker's full name for one speaker (the board's
  "Centre"), else the codes ("L SL").
- **The graph on a surround output** draws the composite of the channel in view
  with the most bands on it (the first on a tie). A band's handle sits on that
  channel when the band is on it, else on the first channel in view it is on;
  such a channel's composite is drawn dashed, as the right is in L+R, where it
  differs, with the speaker codes at the right edge. Bands not in view are faded.
  A single speaker draws that speaker. Reason: every handle sits on a line that is
  a real output path, never a sum of bands no channel plays.
- **Solo and test tones end when the Speakers view closes, and on another output
  or layout.** Neither is saved or visible elsewhere, so a solo left behind would
  silently mute speakers.
- **Test tones start on the first speaker** (as the prototype does). A row's
  play button plays that speaker; pressing the playing one pauses, and test
  tones stay on.
- **Routing controls during test tones** show what plays (Upmix Off, swaps off)
  and are disabled.
- **The layout picker** greys layouts the output does not support
  (`supported_speaker_layouts`); the current layout is never greyed.
- **User groups can be removed**: a trash icon on the hovered row. The prototype
  showed no way to delete one.
- **Groups are stored by speaker code**, so a group keeps its speakers across
  layouts; a group whose speakers the layout lacks is not listed.
- **Ranges:** level -24 to +12 dB, distance 0 to 50 m, delay 0 to 1000 ms, lip
  sync 0 to 500 ms; crossover 40 to 250 Hz and LFE low-pass 80 to 250 Hz in 10 Hz
  steps (the spec's).
- **-30 dBFS RMS** is an RMS of 0.0316 of full scale (0 dBFS RMS is an RMS of
  1.0, not the AES17 sine convention).
- **Saving writes the file only, with the saved bands kept.** The contract says
  `DeviceLink::save`, which writes the given state to the file and then the
  region. Saving the whole edited state would save unsaved band edits with a
  speaker change, and writing the saved bands to the region would play them over
  the edited ones. So `save_speaker_setup` reads the saved file (flat when there
  is none), replaces the speaker setup, levels and layout, writes it with
  `write_persisted_state`, and `commit()` writes the edited state to the region.
  An idle engine that starts in between reads the file (speakers already right),
  and the session writes the region when it finds it. Equalizer APO outputs have
  no separate saved state: `commit` persists Isotone.txt.
- **Showing picker codes** list built-in groups in the prototype's order
  (L C R, SL SR RL RR) and user groups in channel order.
- **Collapsed panel** shows the layout in muted 11 px text, as the prototype;
  the board had an icon and bold text.

## Verified

**doctest** (`ui_tests` 23 cases, `ui_model_tests` 18, all passing):
delay from distance and back, the reference moving, clamping; speaker codes and
names for stereo, 2.1, 5.1, 7.1; group masks for 7.1, 5.1, 2.1 with user groups;
target labels; solo masks with and without bass management and the LFE rule;
live overrides with solo alone and with tones; bass management snapping and
ranges; the saved state's speaker part, remapped, to a file and back with the
saved bands kept; the graph's channel choice; pink noise RMS (-30 dBFS within
1e-4 relative over the loop, every 3 s window within 0.2 dB), Welch slope
-3.01 +/- 0.25 dB/oct with no octave more than 1 dB off the line, peak below
0.5, other rates; typed metres and milliseconds; a tone on a missing endpoint
reports ERROR_NOT_FOUND and plays nothing. On the session: target masks and
labels, a layout change and a load remapping values, solo and test tones in
`engineState` but never in the saved file (native, sandbox saved directory),
tones and solo reaching an Equalizer APO output's Isotone.txt and the real state
coming back (sandbox config directory), a failing tone turning tones off, a stale
failure not ending a later session, bass management through `Speakers`,
distances kept per output, groups kept per output, the Showing picker moving the
drawn channel, the layout setter called with tones stopped.

**QML** (`ui_qml_tests` 66 functions, 33 new): the panel's rows, Speaker setup,
Mute, collapsing, and Channels on stereo; Target chips (groups, speakers joining
and leaving, user groups, stereo keeps Channels); the Showing picker's list,
position, a speaker and a group changing what the graph draws; the Speakers
view's rows, typed level, distance and delay switch, polarity, mute, solo, test
tones (pill, disabled routing, row tones), routing and bass management, New group
dialog, layout dialog with a replaced setter (Cancel, Change, greyed options),
and leaving the view ending tones and solo. The QML tests get a 7.1 session with
no output behind it through a `TestHooks` context object in `qml_main.cpp`.

**Mutation checks:** 53 mutations, scripted (break, build, run the guarding
suite, restore). 51 failed a test at once; two did not (solo applied only during
tones; the stale tone failure check), so tests were added and both then failed.

**Live, CABLE Input at 7.1** (set with `isotone-devicetool set-layout --layout
7.1`, dry run first; restored to stereo through the app's own layout dialog). A
scratch program drove `EqSession` and `Speakers` (the UI's classes) against IsoAPO
on CABLE Input, saved state redirected to a scratch directory, while
`isotone-measure play --amplitude 0` held the region open:
- Level L -6 dB and delay R 2 ms, measured with `isotone-measure` into CABLE
  Output at 100, 125, 200 and 1000 Hz: channel 0 -18.0412 against a baseline of
  -12.0412 dB (-6.0000 dB); channel 1 phase against channel 0 -72.000, -89.999,
  -144.000, 0.000 degrees (2 ms exactly). Mute R: channel 1 at -200 dB, channel 0
  unchanged.
- Test tone on C, with a +6 dB band in the state: IsoAPO's ring
  (`isotone-shm capture`, 5 s, 8 channels) had channel 2 at -29.994 dBFS RMS and
  the other seven exactly silent, slope -3.04 dB/oct; the band did not raise it,
  so the bypass applied. With C's level at -3 dB: -33.003. On L: channel 0
  -29.999, the rest silent.
- Screenshots of the app on CABLE Input at 7.1 (scratch data and compat dirs):
  surround main with Sub and Front targets, the band popover with Target chips,
  the Showing picker, the panel collapsed with the sidebar open and collapsed,
  the Speakers view in distance and delay mode, test tones playing on the
  centre, solo on SL, the New group dialog and the Change layout dialog. Compared
  with Surround.png, SurroundCollapsed.png, Speakers.png and the prototype; the
  fixes found (panel row spacing, target chips wrapping, group code order, mute
  and solo boxes invisible on a hovered row, delay text alignment) are in.
- Restoring stereo from the app: Stereo in the picker, Change; `devicetool
  layouts` then read 2 channels, 24-bit, 48 kHz, mask 0x3, as before. The app
  switched to the Equalizer view with the Channels panel and "Native" in the
  outputs list, so the device notification path works.

## Not verified

- The saved state file written in ProgramData by the app (tested against a
  scratch directory only; no ProgramData write was wanted on the owner's machine).
- 5.1 and 2.1 live; a test tone on an Equalizer APO output; a mix format other
  than float32 in `TestTone` (int16, int24 and int32 writing are untested).
- The QML tests' `mouseClick` on the real layout picker options with supported
  layouts (in tests no output supports any, so only greying is tested).
- The Showing picker's dashed lines and labels are only checked by screenshot.

## Found, not fixed (for the lead)

- `Outputs::data` BackendLabelRole labels any output above 2 channels that is not
  8 as "5.1", so a 2.1 output reads "Native · 5.1" (outputs.cpp, shared).
- `TextBox.qml` set `implicitWidth` on a Column, which is read-only in Qt 6.11:
  the component failed to load. Changed to `width: 240` (the first user is the New
  group dialog). Another package may hit the same.
- `GraphCard.qml` warns "Model size of -6 is less than 0" when created before it
  has a size (the readout's dashed-line Repeater).
- A crash while test tones play leaves the region bypassed with upmix and swaps
  off until the engine restarts or the state is written again; the next session
  loads that from the region as the edited state. The saved file is not affected.
- `CompatWriter` fails a write when another process holds Isotone.txt open without
  delete sharing, and the edit is not retried until the next commit (found when a
  test's reader blocked it; the test now reads with delete sharing).
- During a mutation check (saving on a `none` backend) a model test wrote
  `%ProgramData%\IsoAPO\devices\{8f4d2a10-0000-4000-8000-00000000d157}.bin`, a
  saved state for a GUID no endpoint has. It is harmless but should be deleted;
  the session's permissions did not allow deleting it.

## Follow-ups

- Presets: loading a preset must keep `EqState::speakers`, `channel_gain_db` on
  surround outputs, and the layout (`EqSession::loadState` replaces the whole
  state today).
- Undo: `editSpeakers` is one committed step each.
- A distance mode kept across sessions (now per session).
