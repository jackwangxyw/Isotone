# Isotone UI spec

The build brief for stage 4. Decisions behind it are in `decisions.md`; where this
file and `isotone-plan.md` section 7 disagree, this file wins.

## Start here (next session)

1. Read this file, then "Where things stand" at the end of `decisions.md`.
2. Look at every PNG in `docs/design/screens/`. They are the approved design.
3. Verify the open points under "Framework" below, then ask the owner before
   installing Qt (not installed; nothing is on PATH, see the build-environment
   memory).
4. Build the first deliverable: the `Main` board as a running Qt Quick app, real
   core curve, draggable handles. Screenshot it at 1440 × 900 and compare with
   `docs/design/screens/Main.png` before building anything else.

## Sources

- **Screens (source of truth for look):** `docs/design/screens/<Board>.png`, one
  1440 × 900 render per board below, dark theme with the default blue accent
  except `Light*`.
- **Generator:** `python docs/design/generator/gen_screens.py` regenerates the
  boards as `docs/design/mockups/*.dc.html` (it runs `gen_mockups3.py`; both need
  `mock_base.py`, numpy, scipy and `tools/gen_reference.py`). Exact colours,
  sizes and spacing are in its source when a PNG is not precise enough. Every
  curve is computed from the real filter maths.
- `docs/design/` is gitignored and lives only on this machine (inside the
  OneDrive-synced repo folder).

| Board | Shows |
|---|---|
| `Main` | Dark, stereo, sidebar open, per-band colours, manual order |
| `Collapsed` | Dark, sidebar and Channels collapsed, outputs popover open, by frequency |
| `Light` | Light, sidebar open, accent colours, by frequency |
| `LightCollapsed` | Light, sidebar and Channels collapsed, per-band colours |
| `Surround` | Dark, 7.1 output, Speakers panel, bands target groups |
| `SurroundCollapsed` | Dark, 7.1 output, sidebar and Speakers collapsed |
| `Speakers` | Speakers view for outputs with more than two channels |
| `Appearance` | Settings, Appearance |
| `EqByEar` | EQ by ear, sweep: cursor and S/T/E marks on the graph, play, nudge, log slider, Start/Top/End buttons (each captures the current frequency), Peak/Dip, Clear. Setting the third mark creates the band in the preset; no create button |
| `EqByEarAB` | EQ by ear, A/B: reference and test tones, alternate, test level, recorded points on the graph, threshold, Clear points. Recording a point refits the bands automatically; no fit button |
| `Devices` | Outputs table (engine, status, format, preset) and a detail panel with Repair, Test, Uninstall |
| `SettingsGeneral` | Startup, presets, graph range, spectrum options |
| `SettingsShortcuts` | Key bindings, with a Global toggle for app-wide ones |
| `SettingsAbout` | Version, Copy diagnostics, engine status, protected audio |
| `PresetsMenu` | Presets popover from the preset name: search, list with assigned output, rename/duplicate/delete icons on the hovered row, New, Save, Save as, Import, Export |
| `ImportDialog` | Import preview: curve, preamp, filter count, skipped lines, name, assign to |
| `BandMenu` | Band popover, opened from the type name in a band column (shown above it) or by right-clicking the band's handle (shown above the handle): filter type tiles drawn from each filter's real response, Channels, Enabled, Duplicate, Reset gain, Delete |

EQ by ear: graph (shorter), sweep or A/B controls, then a vertical band list
filling the rest of the height, instead of the main screen's strip. Each row:
number, type icon and name, frequency, gain, Q, channels, enable; values
click-to-edit. The list scrolls; a band EQ by ear creates is added at the end,
selected, scrolled into view, and drawn on the graph.

Settings pages share one header with tabs: General, Appearance, Shortcuts, About.

## Framework: Qt 6 Quick

- QML for the UI, C++ for everything else. One process.
- Links `isotone_core`, `isotone_transport`, `isotone_compat` and
  `isotone_devices` directly. The plan's `/bridge`
  N-API addon, IPC layer and JSON serialisation are dropped.
- Curves come from `magnitude_db` / `band_magnitude_db` in the core, never
  reimplemented in QML.
- Parameters go out through `param_block_write` on the shared region; spectrum
  audio comes in through `audio_ring_read`. Compat-backend devices use
  `windows/compat` (`CompatWriter`, Isotone.txt) and WASAPI loopback
  (`LoopbackCapture`). Each device's block is written for its current channel
  count, speaker mask and sample rate (`DeviceConfig`), and rewritten when the
  device format changes. Until then a stale block still plays safely: bands,
  trims, delay, polarity and speaker mute follow speaker names, which Equalizer
  APO resolves for the current layout; routing is skipped on another channel
  count; bands are skipped below the lowest rate they are stable at. IsoAPO
  does the same with a stale region or saved file: per-channel values move by
  speaker role (`remap_channels`).
- The EQ-by-ear tone is native audio (WASAPI render, as `windows/measure` does),
  not Web Audio.
- Endpoints, their current format (channels, rate, speaker mask), their engine
  and change notifications come from `windows/devices` (`isotone_devices`):
  `enumerate_render_endpoints`, `read_render_endpoint`, `DeviceWatcher`
  (callbacks arrive on an MMDevice thread; queue them to the GUI thread), and
  `probe_engine` for the status dot (blocks about 200 ms; run it off the GUI
  thread). A device's format comes from there, not from the region header,
  which exists only while a stream is open.
- Endpoint identifiers: every API takes the braced GUID, the bare GUID or the
  full device ID `IMMDevice::GetId` returns (`canonical_endpoint_guid`).
- Device operations shell out to `isotone-devicetool` elevated, with
  `--output <new file>` so the elevated process's JSON can be read back (the
  UI waits for exit, then reads the file). Exit codes: 0 ok, 1 refused or
  failed (`reason`), 2 bad arguments, 3 not elevated, 4 another run holds the
  lock. `status` reports `isoapo.state` (`not_installed`, `installed`,
  `alongside_equalizerapo`, `replaced_by_equalizerapo`, `detached`,
  `unrecorded`, `interrupted`) and `isoapo.remedies`, the commands that fix
  it; Repair, Uninstall and the replace choice follow those. Engine changes
  raise no device notification: re-read the engine after devicetool exits.
- Tray presence for device auto-switch (plan 7.6).

Verify before building: the current Qt 6 LTS and that its open-source modules
are LGPLv3 (compatible with GPL-2.0-or-later via "or later"); how a QML app gets
a tray icon (`QSystemTrayIcon` needs Qt Widgets); the graph approach (Qt Quick
Shapes or a custom scene-graph item) holds 60 fps with the spectrum; the
Instrument Sans licence (OFL) for bundling. Qt is not installed on this machine.

First deliverable: the `Main` board as a running prototype with the real core
curve and draggable handles, compared side by side with the mockup. Only then
the rest.

## Rules

- No explainer microcopy anywhere. Labels and values only.
- Negative numbers use U+2212 minus. Numbers use tabular figures.
- Surround UI appears only when the active output has more than two channels.
- Stereo and surround share one layout; only the right-hand panel and the band
  target labels change.

## Themes

System, Dark, Light, Custom. Custom overrides Background, Surface, Text, Grid,
Spectrum, starting from Dark. Accent is chosen separately.

| Token | Dark | Light |
|---|---|---|
| background | `#121519` | `#f4f6f8` |
| plot / sidebar | `#0e1115` | `#fcfdff` |
| surface (selected nav, rows) | `#1b1e23` | `#e7eaed` |
| grid major | `#26292e` | `#dbdee2` |
| grid minor | `#1a1d22` | `#edeff1` |
| zero line | `#4b4f54` | `#a0a5ab` |
| text | `#e8ebf1` | `#1b2025` |
| muted | `#90969d` | `#646970` |
| track (slider, segmented bg) | `#23272b` | `#e0e3e7` |
| segmented selected | `#383c41` | `#fcfdff` |
| selected band column | `#191c20` | `#eef0f3` |
| spectrum fill | `rgba(126,135,146,0.10)` | `rgba(106,114,125,0.08)` |
| spectrum edge | `rgba(156,165,177,0.22)` | `rgba(92,100,111,0.18)` |
| band bell (accent mode) | `rgba(213,223,235,0.17)` | `rgba(39,46,56,0.15)` |
| text on accent | `#080e16` | `#fafcfe` |
| knob | `#f3f5f8` | `#fcfdff` |
| engine running | `#7ccd8e` | `#3b9555` |

Accents (blue is the default): dark `#6aa7f4 #a495f0 #00bcc5 #62bb78 #d8953d
#ea808a`; light `#3072c1 #7260bd #008892 #1c8742 #a45f00 #b44957`.

Band colours, per band in order: dark `#7cb4fc #30c8cf #76c788 #c9b04f #ec9c63
#f49191 #dc95d5 #b0a4f8 #51bfee #4acaad #a7bc61 #dda552`; light `#467cc0 #008e96
#3d8e53 #917800 #b0652a #b65a5c #a15e9c #7a6cbc #0086b3 #009176 #728426
#a36e09`. A band keeps its colour when it moves.

Curve fill: vertical gradient in the accent, 30% at top and bottom, 4% at the
middle (dark); 20% and 2% (light). Curve stroke 2.5 px.

## Type

Instrument Sans 400/500/600. Preset title 24/600; view titles 26/600; body 13–14;
small labels 11–12.

## Layout (1440 × 900 reference)

**Sidebar:** 248 px open, 72 px collapsed, toggled by the panel icon.
- Open: logo, nav (Equalizer, Speakers when surround, EQ by ear, Devices),
  Outputs list (name, backend, status dot), Settings.
- Collapsed: icon rail; Outputs becomes a button with a status dot that opens the
  same list as a popover; the device name moves under the preset name in the top
  bar.

**Top bar** (76 px): preset name + chevron, which opens the presets popover
(switch, rename, duplicate, delete, new, save, import, export). Under the name
only the output name, and only when the sidebar is collapsed. Right side: Preamp value + Auto (Auto sets
preamp to `auto_preamp_db` with the device's speaker mask, which covers routing and bass management and never boosts); L / R /
L+R (stereo) or "Showing: All speakers" group picker (surround); Spectrum On /
Off; EQ toggle. The spectrum is the processed output; there is no pre-EQ view.

**Graph:** log 20 Hz–20 kHz, ±15 dB view with labels at ±12/±6/0, spectrum
behind, per-band bells, composite curve and fill. Handles r 12, selected r 14
with a ring at r 20; number inside, numbered by position. Hover readout chip
(frequency and composite dB).

**Band strip:** header "Bands 12" and a Manual / By frequency segmented control, with no label or icon. Columns
112 px: number badge + type, vertical gain slider 132 px (±12 dB, fill from 0),
gain, fc, Q, target (L+R, or a speaker group) + enable toggle. Gain, fc and Q
are click-to-edit values: click, type, Enter. The type name opens the band
popover. Double-click a slider resets it to 0 dB (plan 7.2). Scrolls sideways
with a right-edge fade and a thumb. Add band and the right-hand panel stay pinned.

**Channels panel** (stereo): 240 px, collapses to a 52 px strip with a vertical
label and the balance value.
- Balance: number −1.0 to +1.0, step 0.1, plus slider filled from centre.
  Only the opposite side is turned down, linearly: at balance b its gain is
  1 − |b| (0.5 is −6.02 dB), and at ±1.0 it is muted with the speaker mute bit,
  since a trim cannot reach silence. The favoured side never changes. Written as
  the two channel trims.
- Mute.

**Speakers panel** (surround): 240 px, collapses to a 52 px strip showing the
layout. Rows Layout, Crossover, Upmix, Lip sync; Speaker setup button; Mute.

**Speakers view:** layout picker (Stereo, 2.1, 5.1, 7.1) and Test tones. Table:
speaker, level dB, distance (m) or delay (ms) by toggle, polarity, test tone,
mute, solo. Distance sets delay: (farthest − this) / 343 m/s. Test tone: pink
noise, one speaker at a time, −30 dBFS RMS (the level home-theatre test discs
use, calibrated to 75 dB C at the seat), played by the UI through WASAPI on that
channel only, so it passes through the engine with the speaker's level, delay,
polarity and bass management applied. While test tones are on, the UI writes
the device's state with bypass on (bands and preamp off) and swaps and upmix
off, so nothing else moves or re-routes the tone, and writes the real state back
when they end. Solo mutes the other speakers, except the LFE when the soloed
speaker is small, so its redirected bass still plays. Solo is never saved: the
saved state always carries the mute mask without it. Cards: Speaker
groups (All, Front L C R, Surround SL SR RL RR, Sub LFE, New group); Bass
management (crossover, small speakers, LFE low-pass); Routing (upmix Off
/ All / No centre, swap front and rear, swap left and right, lip sync delay). No
room map.

**Appearance:** theme, accent swatches + custom, band colours (Accent / Per
band), custom colour rows with hex values, live preview.

**Installing IsoAPO on an output that has Equalizer APO** (Devices, and the
setup wizard in stage 6): the user chooses between replacing Equalizer APO on
that output with IsoAPO, or keeping Equalizer APO and using the compat backend.
Replacing runs `isotone-devicetool install <guid> --replace-equalizerapo`, which
removes Equalizer APO from every effect slot of that output and takes over its
install record; uninstalling IsoAPO later restores the output as it was before
either EQ (the driver's own APOs), not Equalizer APO.

**Bass management** follows AV receivers: no slope control, Linkwitz-Riley
24 dB/oct. Crossover 40–250 Hz, default 80 Hz; LFE low-pass 80–250 Hz, default
120 Hz; both in 10 Hz steps.

## Engine contracts the UI must keep

From the pre-UI review (`decisions.md`, 2026-09-13). Each is enforced or tested
in the engine; the UI must not break it.

- **Band ids are unique within a state.** The processor matches bands to filter
  slots by `Band::id`: give a new band a fresh id, keep a band's id when it is
  edited, moved or re-sorted. Bands that share an id are matched to slots in
  list order, so an edit can land on the other band's filter. Nothing in the
  engine checks this.
- **At most 64 bands** (`kParamMaxBands`). `to_param_block` returns false when
  it dropped bands; the UI must not create more (Add band disabled at 64). The
  compat backend has no limit, but a device can switch engines.
- **The EQ toggle is bypass: bands and preamp only.** Mute, balance and trims,
  and the whole speaker setup stay on. The curve drawn while bypassed is
  `magnitude_db` with `bypass` set, which already follows this.
- **Saved state.** Whenever a device's state is saved (preset assigned or saved,
  speaker setup changed), write it with `isotone::win::write_persisted_state`
  to `persisted_state_path(persisted_state_dir(false), guid)`, as well as to the
  region. That file is what IsoAPO starts with when no UI is running: a file
  that exists is the state, flat included; no file is flat. When the engine is
  idle (no region), write only the file, then try to open the region again and
  write it too if it now exists: IsoAPO reads the file before it creates the
  region, so a stream that started in between would otherwise play the old
  state. A block needs no `init_param_block` before `write_persisted_state`,
  which stamps the header. The first write creates
  `%ProgramData%\IsoAPO\devices`; a file one Windows account writes cannot be
  replaced by another until the installer sets the directory's ACL (stage 6).
- **A state knows its layout.** `EqState::layout_channels` and
  `layout_speaker_mask` name the layout its band channels, trims, delays,
  polarity, speaker mute and small speakers address (`parse_apo_config` sets
  them). Set them whenever the UI builds per-channel values for a device. When
  the device's layout changes, call `remap_channels` before showing or editing
  speaker values; the curve functions and both engines already remap. SL and RL
  (SR and RR) stand in for each other when a layout has only one pair; values
  that land on one channel combine (masks as a union, trims and delays summed);
  a band left with no channel is disabled and keeps its mask.
- **Import parses for the device the preset is for.** Call `parse_apo_config`
  with that device's layout (channels and speaker mask from `windows/devices`). Show `warnings` in the import dialog's skipped lines:
  every line the import does not apply is there (GraphicEQ, Include, Delay,
  Copy, Convolution, VST, unknown commands), as are lines Equalizer APO would
  ignore (lower-case `on`, a frequency without `Hz`), `Device:` sections and
  `If:` branches, which import regardless. `Filter N: OFF` lines with a whole
  filter import as disabled bands.
  Mute read from a file is mute only for the layout it was written for.
- **Auto preamp** is `auto_preamp_db` with the device's channels and speaker
  mask: −`composite_peak_db`, never above 0 dB, so it only cuts.
- **The curve is each output's own path.** `magnitude_db` and `phase_deg` take
  the device's channels and speaker mask and draw bands, preamp, trim, mute,
  speaker mute (silence), and with bass management the small speaker's
  high-pass and the LFE's low-pass. Cross-feeds (swaps, upmix, bass sent to the
  LFE) are not drawn; delay is not in the phase.
- **Speaker mute silences the speaker everywhere**, including the bass that
  bass management would send from it to the LFE.
- **Compat writes need the format.** `DeviceConfig::layout` and `sample_rate`
  must be the device's; `apply` and `persist` refuse a config without them
  (`ERROR_INVALID_PARAMETER`).
- **Compat devices hear an edit when it is committed.** Every write to the
  config directory makes every Equalizer APO device on the machine rebuild its
  filters from rest (measured 2026-09-13): a delayed channel goes silent for the
  delay, a high-Q bass band swells up to +10 dB for about 130 ms. So during a
  drag or a held key the UI calls `CompatWriter::apply`, which writes nothing,
  and calls `persist` once on release; discrete changes (a toggle, a typed
  value) are `persist`. The curve still moves live. `persist`, `remove` and
  `flush` can block up to 200 ms while Equalizer APO holds the file, so drive
  `CompatWriter` from one worker thread. Call `flush()` at shutdown: its result
  is the last edit's. Several writers (another UI, `isotone-compat`) share
  `Isotone.txt.lock` in the config directory and keep each other's blocks.

## Engine support for the speaker controls

Built (decisions.md, "Multichannel speaker features in the engine"): speaker
delay, lip sync, polarity, mute, both swaps, upmix and bass management, carried
in `EqState::speakers` and param block v5. Groups are channel masks on bands.
Solo is the UI muting the other speakers. The per-speaker test tone is UI work
(above); the engine needs nothing for it.
