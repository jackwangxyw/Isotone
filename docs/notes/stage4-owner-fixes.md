# Stage 4: six fixes after the owner tried the UI

Branch `worktree-agent-af1eed7bceb8a9d38`, on top of d25455a.

## 1. The undo toast: shorter, and it fades

The toast moved out of `Main.qml` into its own `Toast.qml`, which is how the
other pieces of the window (`WindowKeys`, `PressWatch`, `WindowClose`) are
already split out, and is what makes it testable: the QML tests never build the
window. `Main.qml` now holds one line, `Toast { centreOffset: sidebar.width / 2 }`.

- 3 s instead of 5 s.
- `opacity`, not `visible`: a `Behavior` fades it over 250 ms, in and out.
  `showing` keeps `visible` true from the moment it is raised until the fade-out
  ends, so it is clickable the whole time it is on screen.
- A second toast sets `showing` (already true), replaces the text and restarts
  the 3 s wait; if one was fading out it comes back from where it had got to.

Test `ui/tests/qml/tst_toast.qml`. `tst_undotoast.qml` still covers what the
undo action does; this one covers the panel.

## 2. The band strip's wheel

`BandStrip.qml`. The old handler read `angleDelta` only and assigned `contentX`
on every event, so a high-resolution wheel (the MX Master's side wheel sends
many small deltas instead of one 120 notch) stepped once per delta.

- `pixelDelta` is used where the event carries it, `angleDelta` otherwise.
- One notch (120) is 118 px, a column (112) and its spacing (6).
- The deltas add into `flick.scrollTarget` and a `NumberAnimation` eases
  `contentX` there, restarted from the current position on each delta, so a spin
  glides and settles instead of stepping.
- A press takes over: `onDragStarted` stops the glide, and the thumb and its
  track now go through `flick.scrollTo()`, which stops it and clamps in one
  place (it replaces the clamp that was written out twice).

## 3. Refresh in Devices

`DevicesView.qml`, and a `busy` property on `Button.qml` that puts a `Spinner`
where the icon goes. The signal that says a read finished is
`DevicesModel::revisionChanged` (`devicesmodel.h`), which the button listens to
only while it is working, so the 3 s engine poll does not end the state.

Working (spinner, label unchanged) until both the read is in and 400 ms have
passed, then "Refreshed" with the check for 1.5 s, then back. A click while it
is working is ignored.

## 4. Right-click a band's column

`BandColumn.qml`. The column's own MouseArea takes the right button as well and
opens the popover through the new `openMenu()`, which the type name's click also
calls, so both open it in the same place, centred on the column and above the
type name. The gain slider and the value fields take the left button only, so a
right press anywhere in the column reaches this one; a left press still selects,
and the slider drag and the strip drag are untouched. The graph's handles
already did this (`GraphCard.qml`), and that code is not touched.

## 5. Add band, centred

`BandStrip.qml`. It had `anchors.verticalCenterOffset: 16`, which pushed the
plus and its label 16 px below the middle of the box.

Across, the box a reader sees runs from the item's own line to the panel's line,
and the Row's 12 px spacing sits between the two: the item is 92 px wide, the
gap 104. So the content is centred on the gap, `horizontalCenterOffset:
spacing / 2`, not on the item. This is a decision the boards do not settle: in
`Main.png` the content is centred on the item, 6 px left of the middle of the
gap, and the owner asked for it centred, so it is centred on what the two lines
enclose.

Down, the offset is gone and the content is centred in the item.

Measured on the 1440 x 900 screenshots, ink bounding boxes, box 1064..1168 by
510..899: before 1109 x 718.5, after 1115 x 702.5, middle 1116 x 704.5. The
1 to 2 px that remain are the label's ink against its line box ("Add band" has
no descender); the layout boxes line up, which is what the test checks.

Not matched to the board: the board's two lines stop 43 px above the window's
bottom edge and the app's run to it. The owner did not raise it and it is not
part of centring, so it is left alone.

## 6. Renaming a preset

`PresetsMenu.qml`: the current-preset check on the left is hidden while that row
is being renamed, so only the save check shows. The prototype's rename row has
an empty `.ck` slot, which is the same thing.

## Verified

- All three suites: `ui_tests` 40 cases, `ui_model_tests` 98 cases,
  `ui_qml_tests` 252 of 254.
- Mutation checks, each fix broken once and the named test seen to fail:
  toast 5000 ms, toast without the Behavior, wheel without the ease, wheel
  ignoring `pixelDelta`, `busy: false`, a 1 ms hold, the column taking the left
  button only, the old Add band offset, the check not hidden.
- Screenshots (scratch data and compat directories, CABLE Input, or the fake
  devicetool): Add band before and after, the popover from a right press low in
  a column, Refresh idle, working and done, the delete toast.

## Not verified

- The two `ui_qml_tests` failures in a full run are the suite's own: they are in
  the Devices operation box and Settings Outputs, they pass when their file runs
  alone, and d25455a with nothing of mine on it fails 4, 2 and 0 of the same
  tests over three full runs. Timing race in those tests, not in this work, but
  it is worth someone's time.
- The Refresh spinner's animation while `Outputs.refresh()` runs on the UI
  thread. Both calls were there before; the button now shows that something is
  happening, but if that call blocks, the spinner will not turn while it does.

## Shared files touched

`ui/qml/Main.qml` (the toast block only), `ui/qml/BandStrip.qml`,
`ui/qml/BandColumn.qml`, `ui/qml/Button.qml` (a `busy` property),
`ui/qml/PresetsMenu.qml`, `ui/qml/DevicesView.qml`, `ui/CMakeLists.txt`
(`qml/Toast.qml`), `ui/tests/qml_main.cpp` (`TestHooks.pixelWheel`, which
QtTest's `mouseWheel` cannot send).
