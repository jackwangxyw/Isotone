# Stage 4 work package: presets

Presets, import and export, unsaved changes, undo and redo. Built on the
foundation commit 9a7fc6e (the worktree was created at 375094a and fast-forwarded
to it first).

## What was built

**C++ (`ui/src`)**
- `presetstore.*`: the files. One preset per file in `<dataDir>/presets/<id>.json`,
  the id a UUID that never changes (so a rename rewrites one file and no
  assignment), and `<dataDir>/outputs.json` with each output's preset id and its
  name as last seen. Schema version 1:
  `{"format": "isotone-preset", "version": 1, "name", "preampDb", "autoPreamp",
  "layout": {"channels", "speakerMask"}, "bands": [{"id", "type", "fc", "gainDb",
  "width", "widthMode", "shelfCorner", "channels", "enabled"}]}`, types and width
  modes as words. Writes go through `QSaveFile` (temporary file, replacing rename).
  A file is read whole or not at all: wrong format or a newer version, a missing
  or mistyped field, a non-finite number, fc or width not above 0, an id or mask
  that is not a 32-bit whole number; such a file is skipped. A broken
  `outputs.json` reads as no assignments. `same_eq` compares two EQ parts.
- `presets.*`: the `Presets` singleton (the stub's API kept, more added, below).
- `importpreview.*`: `ImportPreview`, a file being imported, parsed with
  `parse_apo_config` for the layout of the output it is for, and again when that
  output changes.
- `curvepreview.*`: `CurvePreview`, the import dialog's curve from the core
  (grid, labels, bells, composite and fill, as ResponseGraph draws them).
- `eqsession.*` (shared, additive): undo and redo, `eqPart`, `setEqPart`,
  `adoptEqPart`, `saveToOutput`, `useTarget`, a constructor taking a `DeviceLink`
  (tests), and the signals `targetChanged`, `committed`, `historyChanged`,
  `bandDeleted(position, step)`.

**QML (`ui/qml`)**
- `PresetsMenu.qml`: search, rows with the check and the assigned outputs,
  rename/duplicate/delete on the hovered row, inline rename (Enter or the check
  saves, Escape cancels) and inline delete (Cancel, Delete), New, Save (greyed
  unless modified), Save as, Import (native open dialog), Export.
- `UnsavedDialog.qml`, `SaveAsDialog.qml`, `ImportDialog.qml`,
  `ExportDialog.qml` (native save dialog), `DropOverlay.qml`.
- `PresetActions.qml` (singleton): the flows that open dialogs.
- `PresetPrompts.qml`: the "Band N deleted" toast with Undo, and the unsaved
  dialog whenever `Presets.load` meets unsaved changes. One instance in Main.

## Decisions where the spec and prototype were silent

1. **An output with no preset is "Untitled"**, playing what it plays. Nothing is
   turned into a preset on first use and no "Flat" preset is seeded: the
   prototype's "Flat" is an ordinary preset, and creating presets behind the
   user's back would fill the list. It becomes a preset with Save (which asks for
   a name) or Save as.
2. **`modified` is a comparison, not a flag**: what the output plays (bands in
   order without ids, their fields within 1e-5 relative / 1e-4 dB, the Auto mode,
   the preamp unless in Auto) against its preset, moved to the output's layout;
   an untitled output is compared with what it played when it was first shown
   this run. Why: a flag is lost when the output is switched or the app restarts,
   so an output left with unsaved edits would come back looking saved; with a
   comparison it shows the dot, and an undo back to the saved EQ clears it. It is
   recomputed on every commit (not during a drag) and when the output changes.
   Balance, mute, trims, bypass and the speaker setup are not compared.
3. **The playing preset is adopted**: when an output is shown and plays its
   preset within those tolerances, the session takes the preset's exact values,
   ids and Auto mode (the region holds float32 and no ids or mode; Isotone.txt no
   ids). Nothing is written. A preset with a hand-set preamp equal to Auto's is
   recognised too.
4. **New** makes the current output untitled and flat (Auto per
   `general/autoPreampForNew`, default on) and modified until saved; the
   assignment stays, so Don't save goes back to the preset it had. New, loading
   and importing onto the current output ask first when there are unsaved changes.
5. **Unsaved changes are asked about in one place**: `Presets.load/next/previous`
   with unsaved changes load nothing and emit `unsavedChanges(name)`;
   `PresetPrompts` opens the dialog and loads after Save or Don't save. So a
   shortcut or the tray gets the dialog without doing anything. Save on an untitled
   output asks for a name (the unsaved dialog hides until it is given or
   cancelled). Don't save calls `Presets.revert()`: back to the preset, or for an
   untitled output back to what it played when first shown.
6. **A preset assigned to several outputs changes on all of them when it is
   saved**, not on every edit: the owner's "when edited" read as the safer
   "when the edit is saved". Live edits stay on the output being edited, so an
   experiment or Don't save never touches another output. The other outputs get
   the saved EQ through their own `DeviceLink` (commit, and the saved state for
   IsoAPO), each keeping its balance, mute, trims and speaker setup, remapped to
   its layout. To propagate live edits instead, call `propagate(id)` from
   `Presets::refresh`; that is one line, and the mutation check below shows the
   test that would then fail. Only working (connected) outputs are written; one
   that was away shows as modified when it is selected again. Saving overwrites
   unsaved edits another output assigned the same preset was left with.
7. **Loading, saving, Save as and assigning write the saved state** (IsoAPO:
   `DeviceLink::save`, file then region); for an Equalizer APO output the commit
   to Isotone.txt is its saved state. New and edits do not.
8. **Names** are unique case-insensitively; a clash on Save as, rename, duplicate
   or import takes " 2", " 3", ... (replacing a number the name already ends
   with). Names are trimmed; an empty one is refused. The list is sorted by name,
   case-insensitively with numbers by value; `names`, `next` and `previous` use
   that order. Duplicate does not load the copy.
9. **Removing a preset** leaves its outputs untitled, playing what they play (not
   modified).
10. **Undo** (per output, cleared when the output changes or its state is loaded):
    a step is recorded when a commit finds anything the session holds changed
    (bands, preamp, Auto, balance, mute, bypass, the rest of the state), so a drag
    is one step, a commit that changes nothing is none, and a preset load, New and
    Don't save are steps. Undo restores the band selected before the edit (the
    deleted band, for a delete); redo the one selected after. 500 steps. The
    toast's Undo calls `undoStep(step)`, which undoes only while that deletion is
    still the last step (the toast stays 5 s whatever else is done meanwhile).
11. **Import**: the skipped list shows each line with a warning once, with the
    file's own text (the prototype shows the lines, not the parser's messages);
    `Device:` and `If:` lines are there as the contract asks. Usable means at least
    one filter; the Skipped count is amber when nothing is. The name is the file's
    name without its extension. The imported preset is in Auto when its preamp is
    what Auto sets (within 0.01 dB), else manual. A UTF-8 byte order mark is
    dropped and UTF-16 with a mark converted before parsing (Notepad writes both;
    this is file decoding, not checked against upstream's reader). Imported for
    another output than the current one, it is written there and assigned; the
    session is untouched.
12. **Export** writes the current output's EQ as it is edited (what you see),
    bands and preamp only. Layouts: none for a stereo output; for more channels
    the output's own, then 5.1 and stereo below it, as the prototype's 7.1, 5.1,
    Stereo. The Name is the file name offered in the save dialog (characters
    Windows refuses become "-").
13. **The import curve has axis labels**: the prototype's `drawGraph` draws them
    for the import canvas; the ImportDialog board has none. The prototype wins.
14. **Saved state in tests**: a `DeviceLink` whose region namespace is not
    `Global\` now writes and reads the saved state in the self test's directory
    (`%LOCALAPPDATA%\IsoAPO-selftest\devices`), as `isotone-shm` pairs `Local\`
    with it. The app always uses `Global\`, so it is unchanged; tests can no
    longer write `%ProgramData%\IsoAPO\devices`.
15. **`TextBox.qml` (foundation) could not be created**: `implicitWidth` on its
    Column is read-only ("Invalid property assignment"), which failed Main's load
    once a dialog used it. It is now `width: 240`.

## API for the other packages

- `Presets` (C++ singleton): `names`, `count`, `currentName`, `modified`,
  `untitled`, `next()`, `previous()`, `load(name)` (asks first, see 5),
  `save()`, `saveAs(name)`, `rename(name, to)`, `duplicate(name)`,
  `remove(name)`, `newPreset(autoPreamp)`, `revert()`, `assignedName(guid)`,
  `assign(guid, name)` (writes the preset to that output; on the current output as
  `load`; empty name unassigns), `outputChoices()`, `openImport(url)`,
  `importPreset(preview, name)`, `exportLayouts()`, `exportText(channels, mask)`,
  `exportFile(url, channels, mask)`; signal `unsavedChanges(name)`. List model
  roles `name`, `assigned`, `current`.
- `PresetActions` (QML singleton): `confirmUnsaved(closing, proceed, cancelled)`
  runs `proceed()` at once when nothing is unsaved, else opens the unsaved dialog
  ("... before closing?" with `closing`) and runs `proceed()` after Save or Don't
  save, `cancelled()` on Cancel, Escape or the scrim; returns the dialog or null.
  The close-to-tray and quit paths should call it. Also `save(done, cancelled)`
  (Save as when untitled), `saveAs(done, cancelled)`, `newPreset()`,
  `showImport(url)`, `openExport()`.
- `UnsavedDialog.qml`: `closing`, `name`, signals `saved()`, `discarded()`,
  `cancelled()` (exactly one, then it closes).
- `EqSession`: `undo()`, `redo()`, `canUndo`, `canRedo` (for Ctrl+Z / Ctrl+Y),
  `undoStep(step)`, `saveToOutput()` (the speaker setup's "saved state" write can
  use it), `bandDeleted(position, step)`.

## Verified, and how

- `ui_model_tests` (doctest, 29 cases, 14 new for presets and 8 for undo): a
  preset file keeps every field of 24 bands exactly (every type with every width
  mode, corner, masks up to bit 31, disabled); 18 malformed variants refused; the
  store skips a broken file and a text file, numbers a copied file's clash, sorts
  "Preset 9" before "Preset 10", leaves no temporary file; assignments and output
  names persist, and go with a removed preset; `same_eq` sees each field and
  ignores ids, float32 rounding and a layout move. On outputs that are `Local\`
  regions (created by the test as the engine) and a sandbox Isotone.txt
  directory: untitled and Don't save; loading keeps mute, bypass, balance and the
  speaker setup and writes both the region and the saved-state file, as one undo
  step; modified follows committed EQ edits and not balance, mute or EQ off,
  clears on undo back, save and revert; load and next ask over unsaved changes;
  next and previous wrap; the playing preset recognised after a restart with
  float32 values replaced by the preset's, and modified when something else
  changed the region; saving reaches a second IsoAPO output (region, saved state,
  its own trim kept) and a 7.1 Equalizer APO output, and an unsaved edit does not;
  rename, duplicate, New, Don't save after New, Save as numbering, removing the
  current preset; import (counts, skipped lines by line, re-parse for 7.1, import
  for another output, nothing usable, missing file, UTF-8 and UTF-16 marks);
  export for 5.1 against `format_apo_config`, the layout list, stereo has none.
  Undo: a drag of 20 moves is one step, the order of steps, every edit kind, no
  step for no change, balance and preamp, a deleted band's id, place and
  selection, `undoStep` only while last, an added band, history reset on load, a
  preset's EQ as one step keeping mute, bypass and balance.
- `ui_qml_tests` (Qt Quick Test, 5 new files, 30 new test functions): the popover
  (list, check, search, load and close, icons only on the hovered row, rename
  with Enter, Escape and the check, duplicate, delete confirm with Cancel then
  Delete, Save greyed until modified, New, a load over unsaved changes opening the
  dialog); the unsaved dialog (Cancel, Escape, Don't save, Save, the closing
  title and width, nothing unsaved proceeds at once, Save on untitled asking a
  name, cancelling the name); the import dialog on `ui/tests/qml/data` (a file
  with 6 filters and Include, GraphicEQ and Convolution lines; a GraphicEQ-only
  file with Import greyed and the count amber; an empty name; import over unsaved
  changes asking; an unreadable file; the drop overlay's steps); export writing a
  scratch file equal to `exportText`, which imports back with the same filters and
  preamp, and for a layout picked; the toast's Undo restoring the band and doing
  nothing after another edit.
- All three suites pass: `ui_tests` 11 cases, `ui_model_tests` 29, `ui_qml_tests`
  73 totals.
- Mutation checks: see the table below.
- Screenshots (1440 x 900, dark): the popover with a hovered row, rename, delete
  confirm, the unsaved dialog and its closing variant, import with skipped lines
  and with nothing usable, export (surround layouts and stereo), Save as and the
  drop overlay, taken from a Qt Quick Test harness laid out as Main with no output
  (on the Windows platform, so fonts render as in the app) and a data directory
  seeded with the prototype's presets; the popover and the "Band 2 deleted" toast
  also in the app itself (`--data-dir`/`--compat-dir` scratch, `--output` CABLE
  Input, `--add-band`, `--click`), which added and deleted bands on CABLE Input
  only (live edits, no preset loaded, no saved state written). Compared with
  PresetsMenu.png, ImportDialog.png and the prototype: layout, sizes and text
  match.

### Mutation checks

Each behaviour broken once with a script (source edited, built, its suite run,
source restored and rebuilt): 38 mutations, every one failed a test. Two needed
work first: "unsaved edits propagate" was first written as a mutation that
re-sent the saved preset, which changes nothing and so passed (rewritten to send
the live edit); "removing the current preset leaves it modified" passed because
the code it broke was redundant (removed; the mutation now breaks the path that
does the work). "export ignores the layout" passed with only a stereo test, so a
test with a layout picked was added.

| Broken | Caught by |
|---|---|
| `same_eq` ignores band channels | same_eq test |
| a newer schema version is read | malformed files |
| name clash compared by case | store: unique names |
| an assignment is not written to disk | assignments persist, and 2 more |
| the file drops shelfCorner | round trip |
| loading writes no saved state | loading keeps the output's parts |
| a preset load replaces mute | 3 cases |
| load does not ask over unsaved changes | modified test |
| Don't save keeps the edits | modified, New |
| saving does not propagate | propagation |
| live edits propagate | propagation, and 3 more |
| another output loses its own trims | propagation |
| every commit is a step | undo (the undo loop never ends; killed) |
| a drag move is a step | drag is one step, and 1 more |
| undo selects the first band | deleted band, added band |
| `undoStep` ignores which step | deleted band |
| the playing preset is not adopted | recognised after restart |
| no byte order mark handling | import |
| skipped lines show the parser's message | import |
| export offers no 5.1 | export |
| import for another output loads on this one | import |
| removing the current preset leaves it modified | rename, duplicate, remove and New |
| New is not unsaved | rename, duplicate, remove and New |
| popover Save always active | Save greyed |
| search matches everything | search |
| the trash icon deletes at once | delete asks inline |
| rename not committed | rename, rename current |
| no unsaved dialog on a load | load over unsaved changes |
| dialog Don't save does not revert | Don't save, closing |
| no "before closing" title | closing, untitled Save |
| Save on untitled does not ask a name | untitled Save, cancelling the name |
| toast Undo undoes the last edit | Undo after another edit |
| Import active with nothing usable | nothing usable |
| Skipped count not amber | nothing usable |
| import over unsaved changes does not ask | import over unsaved |
| drop overlay shows with no file | drop steps |
| export file name not cleaned | export writes |
| export ignores the layout picked | both export tests |

Not mutation-checked: the `DeviceLink` saved-state directory for `Local\`
(breaking it would write `%ProgramData%\IsoAPO\devices` on this machine); the
tests that read the file from the self test's directory pass with it. The
`TextBox` fix: without it the app failed to load Main ("Invalid property
assignment: implicitWidth is a read-only property"), seen before the fix.

## Not verified

- The native open and save dialogs (`QtQuick.Dialogs`): never opened; the export
  test calls the dialog's `writeTo`, and import goes through `showImport`.
- A real drag from Explorer: the overlay's steps are tested, not the platform's
  drag events.
- Loading or saving a preset on a real output: every write was to `Local\`
  regions, the self test's saved-state directory and a sandbox Isotone.txt. No
  file was written to `%ProgramData%\IsoAPO\devices` (checked empty afterwards).
- GCC: the UI is not in the GCC build.

## Differences left for the lead

- The presets popover opens at x 268, y 60; the prototype puts it at 280, 58.
  TopBar passes the name's position minus 12 (shared file, not changed).
- The toast's Undo is in the text colour; the prototype colours it accent (Main's
  toast, foundation).
- Popovers and dialogs have no shadow (Popover and DialogFrame, foundation).

- `Presets.assign(guid, name)` writes the preset to that output. A QML test in
  another package must not assign a preset to a real output's GUID: the QML
  singleton writes through `Global\`.
- `C:\ProgramData\IsoAPO\devices\{8f4d2a10-0000-4000-8000-00000000d157}.bin`
  appeared at 14:12 while these tests ran. That GUID is in the surround
  package's `test_speakers_model.cpp`, not here: its test writes the real saved
  state directory. Not deleted (not this package's).

## Follow-ups

- An assigned output that is not connected when its preset is saved keeps the
  old EQ and shows modified when selected; applying the saved preset then would
  need a decision on what wins.
- The native file dialogs and a real drag from Explorer, by hand.
