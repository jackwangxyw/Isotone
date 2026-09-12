# Decisions log

Decisions that bind later work, with the evidence behind them. The scope and
architecture document (`docs/isotone-plan.md`, untracked) is the source of truth
for *what* is being built; this file records *what was settled while building it*
and, where the two differ, which one wins.

Format: newest section last. Each entry says what was decided, what it was
checked against, and the date.

---

## From the plan, section 2 (settled before implementation)

1. **Windows has two backends behind one interface.** Primary: a fork of
   Equalizer APO (IsoAPO) keeping the APO shell, device registration and
   installer, replacing the filter engine and config transport. Secondary: a
   compatibility backend that drives a stock Equalizer APO install by writing its
   config files. The compat backend is built first.
2. **License: GPL.** See the 2026-09-11 entry below for the version.
3. **DSP core is C++** (C++17 minimum, C++20 where MSVC and GCC agree).
4. **UI is Electron + TypeScript** with a C++ N-API addon. Reopened; see
   2026-09-11 below.
5. **Linux backend is a user-space daemon over libpipewire**, modelled on
   EasyEffects' topology. No PulseAudio support.
6. **Live parameter transport is shared memory**, with a file for persistence and
   cold start. Not file-watching.
7. **Smoothing is done in the parameter domain in the core**, coefficients
   recomputed on a short block cadence.
8. **One main screen plus a sidebar.**
9. **One repository.**

---

## 2026-09-11: Licence is GPL-2.0-or-later

Upstream Equalizer APO's per-file headers say "either version 2 of the License,
or (at your option) any later version". Checked in `FilterEngine.cpp`,
`DeviceAPOInfo.cpp`, `filters/BiQuadFilter.cpp`, `filters/BiQuad.cpp` and
`filters/BiQuadFilterFactory.cpp` on the `mirror/equalizerapo` master branch.

Consequences:

- `LICENSE` holds the verbatim FSF GPL v2 text; new files carry
  `SPDX-License-Identifier: GPL-2.0-or-later`.
- The plan's constraint "no GPLv3-only dependency may be linked" does **not**
  bind, because "or later" permits relicensing the combined work to v3.

Still to do at vendoring time: sweep every file actually copied into
`/windows/apo`, not just the five read here, and keep upstream's `License.txt`.

## 2026-09-11: Repository scaffolding

- `docs/` is ignored except `decisions.md`. The ignore rule is `/docs/*` with a
  `!/docs/decisions.md` negation, because Git cannot re-include a file whose
  parent *directory* is excluded.
- `sim/reference/` holds a read-only snapshot of the owner's live Equalizer APO
  configuration and endpoint registry values, taken 2026-09-11 as ground truth
  for format work. Ignored: it is personal configuration, not project data.
- Test framework is **doctest 2.4.12**, vendored as a single header under
  `core/tests/third_party/`. MIT, compatible with GPL-2.0-or-later. Chosen over
  Catch2 for compile speed and for having no build-system footprint.
- Reference data is a flat whitespace-separated text format, not JSON, so the
  C++ tests need no JSON dependency and the files stay diffable.

## 2026-09-11: Never write to the live Equalizer APO install

Every code path that writes an Equalizer APO config takes its root directory as a
parameter. Development and test builds default to a sandbox under `sim/`; there
is no default that points at `C:\Program Files\EqualizerAPO`. Reaching the real
install requires an explicit flag.

Testing proceeds in three tiers: offline measurement against scipy references;
upstream's own `FilterEngine` built from vendored source and run offline over WAV
files; and only then live system audio, which needs the owner's approval per
step. The machine already has VB-Audio Virtual Cable with Equalizer APO
registered on it, which is the endpoint live testing will use.

## 2026-09-11: Filter model corrections to plan section 4.2

Read from upstream `filters/BiQuad.cpp`, `filters/BiQuadFilter.cpp` and
`filters/BiQuadFilterFactory.cpp`. Where these contradict the plan's table, the
source wins, because the stated requirement is that imported configs measure the
same here as in Equalizer APO.

- **`LS`/`HS` are not "fixed 12 dB/oct" shelves.** The trailing `C` in
  `LSC`/`HSC` selects whether fc is taken literally. Without the `C`, upstream
  shifts the design frequency by `10^(|gain|/80/S)` (multiplying for a low shelf,
  dividing for a high shelf) before designing, which moves the knee. This is
  upstream's "frequency adjustment for DCX2496". Modelled as `Band::shelf_corner`.
- **Three width parameterisations, not one.** A config may give `Q`, `BW Oct x`,
  or a bare `x dB` slope for shelves; each produces a different alpha term.
  Modelled as `Band::width` plus `Band::width_mode`.
- **Defaults when the width field is absent:** low/high/band pass get
  Q = 1/sqrt(2); notch gets Q = 30; shelves get slope 0.9 dB (both of the latter
  are upstream constants "found out by experimentation with RoomEQWizard");
  peaking and all-pass are errors and the filter is dropped.
- **Band pass is the constant 0 dB peak gain variant** (`b0 = alpha`), not the
  constant-skirt variant whose peak is Q. Verified numerically as well as by
  reading the source.
- **Extra tokens the plan's table omits:** `PEQ` and `Modal` are accepted as
  aliases for `PK`. Gain is parsed but ignored for `LP`, `HP`, `NO` and `AP`.
- **Two parser quirks to reproduce**, both in `BiQuadFilterFactory`: a comma is
  normalised to a period as decimal mark, and an `Fc` value of five or more
  characters with a period four from the end and no exponent is multiplied by
  1000, because Room EQ Wizard writes "1.000" for 1 kHz.

## 2026-09-11: Band model extends the plan's struct

`Band` carries `width` + `width_mode` + `shelf_corner` where plan section 4.1 has
a single `q`. Without those two extra fields the importer cannot round-trip
`LS` versus `LSC`, or `BW Oct`, and the owner's own live `peace.txt` uses `HS`.

## 2026-09-11: UI framework decision is deferred

Plan section 2 item 4 fixes the UI as Electron + TypeScript. The owner reopened
this. Nothing in stages 0 to 3 depends on it: the APO config parser lives in
`/core` as C++ (plan 7.1 exposes it through the addon, plan stage 2 places it in
the core), and the only Electron-specific artifact is `/bridge`, a thin N-API
wrapper. `/app` and `/bridge` are not scaffolded until the choice is made.

## 2026-09-12: Core processing, smoothing and transport

Stage 2 built. Decisions taken while building it:

- **Transposed direct form II**, one pair of state words per band per channel, all
  arithmetic in double. Upstream uses direct form I; the plan specifies TDF-II
  and the two are equivalent at double precision.
- **Control block cadence.** Coefficients are recomputed every
  `control_block_frames(rate)` samples: 32 at 48 kHz, scaled to hold the interval
  near 0.67 ms at every rate. Parameters move by a one-pole smoother with a 20 ms
  time constant, fc and width in the log domain, gain in dB.
- **Crossfade state handling.** When a band changes discontinuously (type,
  width mode, channel mask, enable, identity, appearance or removal) the live
  filter state is handed to the outgoing filter and the incoming filter starts
  from rest, with its output weighted from zero. An earlier version left the old
  state in the new filter, which produced a click of over four times full scale
  when a band was disabled. Caught by `test_smoothing.cpp`.
- **Bypass mix is interpolated per sample** across the control block, not held
  at one value for it. Holding it produced a staircase at the block rate that
  measured as a click.
- **Every click test proves the detector first.** `test_smoothing.cpp` begins by
  synthesising an actual gain step and asserting the metric catches it, so none
  of the no-click assertions can pass vacuously.
- **`%.12g` for numbers in exported configs.** The default `%g` gives six
  significant digits, which rounds a computed 1419.857 Hz to 1419.86 and breaks
  the round trip. Short values still print short.
- **Seqlock uses `std::atomic_ref`**, not a cast to `std::atomic*`, so the block
  stays a plain copyable POD for the mapped layout.
- **FTZ and DAZ are guarded separately.** Flush-to-zero is SSE1;
  denormals-are-zero is SSE3, which GCC only exposes with the right target flags.
  MSVC on x64 has both unconditionally.

## 2026-09-12: Stage 1 scope

- **Linux (stage 1c) is deferred** at the owner's direction. No Linux
  environment exists on this machine: WSL is not installed, there is no
  VirtualBox, VMware or Docker, and Windows 11 Home has no Hyper-V.
- **The owner accepted system impact for the Windows path**, including installing
  the forked APO on a test endpoint, with the standing condition that they are
  told before their audio device is taken over.
- **Peace must be closed before anything writes `config.txt`**, at the owner's
  direction, so the two never write the same file concurrently.
- **Device scoping is by endpoint GUID.** Upstream's `DeviceFilterFactory::
  matchDevice` splits a `Device:` pattern on `;` into alternatives and on spaces
  into words; every word must be a case-insensitive substring of the device
  string, and a word containing `{` is matched against the string *with* its GUID
  rather than the GUID-stripped form. A full brace-wrapped endpoint GUID is
  therefore the narrowest match available and cannot collide with another device.
  This is what makes writing to the live config safe.

## 2026-09-12: Known gaps

- **GCC is unverified.** Stage 2's acceptance criterion is "tests green on MSVC
  and GCC". Only MSVC 19.51 has been run, because no GCC, Clang, Docker or WSL
  is present. Closing this needs either a toolchain installed locally or a push
  to GitHub so the CI workflow runs. Nothing has been committed or pushed.
- The audio ring (`AudioRingHeader`) is declared but has no implementation yet;
  it is needed by stage 3, not stage 2.

## 2026-09-12: Shelf defaults, corrected by measurement

The first reading of `BiQuadFilterFactory.cpp` got the no-width shelf case wrong,
and a measurement through the owner's real Equalizer APO install caught it.

Upstream's code is:

```cpp
if (bandwidthOrQOrS == 0) {
    ...
    else if (type == LOW_SHELF || type == HIGH_SHELF) {
        bandwidthOrQOrS = 0.9;      // S, used directly
        isBandwidthOrS = true;
    }
    ...
} else if (type == LOW_SHELF || type == HIGH_SHELF) {
    if (isBandwidthOrS) bandwidthOrQOrS /= 12.0;
    if (typeString.back() != L'C') isCornerFreq = true;
}
```

Both the `/= 12.0` and the `isCornerFreq = true` live in the `else if`, so they
run **only when a width was actually given**. Therefore:

| Config line | S | corner correction |
|---|---|---|
| `LS Fc 1000 Hz Gain 6 dB` | 0.9 | no |
| `LSC Fc 1000 Hz Gain 6 dB` | 0.9 | no |
| `LS ... Q 0.7` | derived from Q | **yes** |
| `LSC ... Q 0.7` | derived from Q | no |
| `LS 6 dB Fc ...` | 6/12 = 0.5 | **yes** |
| `LSC 6 dB Fc ...` | 6/12 = 0.5 | no |

The bug made a bare `HS` twelve times gentler than it should be (S = 0.075
instead of 0.9) and wrongly shifted its design frequency. It is stored as a
10.8 dB slope, since `design()` divides by 12 and 10.8/12 = 0.9, which avoids
adding a fourth `WidthMode` to the wire format.

`shelf_corner` therefore depends on the token spelling **and** on whether a width
was given, not on the spelling alone.

## 2026-09-12: Equalizer APO can apply one config more than once per endpoint

Measured on the VB-Audio Virtual Cable endpoint
`{798436d2-8c71-4834-9248-00ccbaaca00a}`, which carries the Equalizer APO CLSID
in three `FxProperties` slots (5, 6 and 7 under property key
`{d3993a3f-99c2-4402-b5ec-a92a0367664b}`).

Playing a stepped sine into "CABLE Input" and capturing from the "CABLE Output"
capture endpoint, the transfer function fits the owner's `peace.txt` applied
**twice**, with a flat offset equal to the played amplitude:

```
  k    offset      rms      max
  1    -22.08    7.893   24.687
  2    -13.98    0.000    0.000     <-- exact
  3     -5.88    7.893   24.687
```

31 frequencies, 20 Hz to 20 kHz, rms residual 0.000 dB. That single number
validates the filter design, the config parser, the measurement tool and the
claim that the capture is downstream of the APO, all at once.

Consequences:

- `devicetool status` must report how many slots hold the APO CLSID, and the
  compat backend must warn when a device would double-process. A user on such a
  device gets twice the EQ they asked for, which is upstream behaviour, not ours.
- The measurement rig for Windows is render to "CABLE Input" and capture from
  "CABLE Output", not WASAPI loopback. The virtual cable is a real loop, so there
  is no question about where in the chain the tap sits. `--loopback` remains
  available in the tool for endpoints without a cable, but it is the weaker path.

## 2026-09-12: Stage 1a complete (compat backend, measured)

Ran against the owner's live Equalizer APO 1.4.2.0 install with Peace closed.
`config.txt` was backed up first, one `Include: Isotone.txt` line was appended,
and everything was reverted afterwards and verified byte-identical.

Three experiments, render to "CABLE Input", capture from "CABLE Output":

1. **Negative control.** `Isotone.txt` scoped to a *different* endpoint's GUID.
   Measured response identical to baseline at every frequency, so the `Device:`
   line is genuinely honoured and a scoped file cannot leak onto other devices.
2. **Positive test.** Scoped to the CABLE Input GUID, one `PK Fc 1000 Hz Gain
   -12 dB Q 1`. The measured difference from baseline matched the analytic
   response of that filter to **0.001 dB maximum error** over seven frequencies
   from 250 Hz to 4 kHz, and to 0.000 dB at 1 kHz.
3. **Explanation check.** Scoped to the *capture* endpoint's GUID instead. The
   dip appeared identically, proving Equalizer APO processes on both the render
   and the capture endpoint of the virtual cable. That is why `peace.txt`, which
   uses `Device: all`, measured as applied twice through this loop.

Conclusions that bind later work:

- The compat backend's file format, device scoping and filter maths are correct
  against a real Equalizer APO, not just against our own tests.
- `Device: {endpoint-guid}` is a safe, verified way to scope a config to exactly
  one endpoint.
- A measurement loop through a virtual cable applies any `Device: all` config
  twice, once per endpoint. Test configs must be scoped, and baselines must be
  differential.
- Atomic replacement must use `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` as
  the plan says. Windows PowerShell 5.1 has no three-argument `File.Move`, which
  silently no-ops; that cost one confusing measurement during this work.

## 2026-09-12: GCC verified, toolchain installed

MinGW-w64 GCC 16.1.0 (WinLibs UCRT, via winget, user scope) was installed so the
"green on MSVC and GCC" acceptance criterion could actually be met rather than
assumed. The core builds clean under `-Wall -Wextra -Wpedantic` and all 89 test
cases pass on both compilers.

It is a stand-in for the Linux CI job, not a substitute for it: MinGW shares
libstdc++ and the language front end with Linux GCC, which is what matters for
the core, but not the platform layer.

Two real problems it caught that MSVC did not:

- `windows/measure/main.cpp` used `uint32_t` with no `<cstdint>`, working on MSVC
  only because a Windows header pulls it in transitively.
- The Windows subdirectories were gated on `WIN32`, which is true for MinGW as
  well. They link SDK import libraries that only exist for MSVC, so the gate is
  now `WIN32 AND MSVC`.

`-msse3` is now passed on GCC/Clang x86 targets. Without it the denormals-are-zero
intrinsic is compiled out (see the earlier FTZ/DAZ entry) and a decaying filter
tail costs roughly a hundred times as much per sample.

## 2026-09-12: A test that only failed on a loaded machine

`at a realistic update rate almost every read succeeds` paced its writer thread
with a busy-wait spin count. That is a fine proxy for elapsed time on an idle
machine and a bad one on a loaded machine, where the OS deschedules threads at
will. Under twelve competing CPU-bound processes it failed about one run in
three, which is exactly the profile that produces an intermittently red CI and
gets a test disabled rather than fixed.

It now paces on wall-clock time (`sleep_for(1ms)`, modelling a UI that writes
tens to hundreds of times a second). Verified by re-running both compilers under
the same twelve-process load that reproduced the failure.

The maximum-contention test alongside it asserts only that no torn read is ever
accepted, which is load-independent and stays as it was.

## 2026-09-12: Stage 1b, IsoAPO built and verified offline

`windows/apo` now holds a working APO: `IsoAPO.dll`, built against the Windows
SDK alone. No WDK, no libsndfile, FFTW, muParserX, TCLAP or Qt, because the
config language and convolution the plan removes are exactly what needed them.

Written fresh rather than vendored. The plan (5.2) says to keep upstream's
`EqualizerAPO.cpp` and modify it, and that remains right for the shipping fork,
because its child-APO wrapping and its device-registration code carry years of
edge cases. For the stage 1 spike a minimal shell reaches a measurable result
without first making five third-party dependencies build. What is deferred:
child-APO wrapping, the AVRT code-locking pragmas, logging, and the
shared-memory transport.

Build notes worth keeping:

- `DEFINE_GUID` only declares; exactly one translation unit must include
  `<initguid.h>` first or the CLSIDs do not link.
- `CreateAudioMediaTypeFromUncompressedAudioFormat` lives in
  `audiomediatypecrt.lib`, which carries `/DEFAULTLIB:atls.lib`. ATL is not part
  of a default Build Tools install, and none of its symbols are actually
  referenced, so the APO links with `/NODEFAULTLIB:atls.lib`.
- `audiobaseprocessingobject.lib` needs `legacy_stdio_definitions.lib` for
  `_vsnwprintf`.
- Static CRT linking is deferred to packaging: it has to apply to
  `isotone_core` too, or the two disagree over `lround` and `copysign`.

**`isotone-apo-selftest` is the important part.** It loads the DLL in an ordinary
process, calls `DllGetClassObject` by hand, and drives the full lifecycle:
class factory, `QueryInterface` for all four interfaces, `GetLatency`,
`Initialize`, `LockForProcess` with a real `IAudioMediaType`, `APOProcess` over a
second of audio, silent-buffer handling, `UnlockForProcess`, and the
`DllCanUnloadNow` refcount transitions. It then measures the output and compares
it against the analytic filter response.

It registers nothing and opens no audio device, which matters: a fault found here
is an exit code, while the same fault inside `audiodg.exe` takes down every sound
on the machine until the audio service restarts. Result:

```
  measured level at 1 kHz    -12.000 dB (analytic -12.000)
  measured level at 100 Hz    -0.161 dB (analytic  -0.161)
  PASS (0 failures)
```

Remaining for stage 1b: registering the DLL and pointing an endpoint at it, which
needs elevation. `windows/apo/install.ps1` does both against one named endpoint,
backing the endpoint's `FxProperties` key up to a `.reg` file first. On the
VB-Cable endpoint `BUILTIN\Administrators` already holds `SetValue` on that key,
so no TrustedInstaller ownership takeover is needed there; that is not true of
every endpoint, which is why the shipping path is `devicetool` wrapping
upstream's `RegistryHelper` rather than this script.

## 2026-09-12: Stage 1b complete, measured in audiodg

IsoAPO registered on the VB-Cable render endpoint and measured through the cable
loop. The fit, over seven frequencies from 250 Hz to 4 kHz:

```
 n_iso  n_peace       rms       max
     2        1    0.0002    0.0004   (offset +0.000)
```

Our APO, hosted by audiodg.exe as LocalService, reproduces the analytic response
to 0.0002 dB rms with a fitted offset of exactly zero. Stage 1's acceptance
criterion ("a measured -12 dB dip at 1 kHz, captured by an automated script") is
met on Windows for both backends.

### Four failures getting there, and what each one teaches

Every one was a registry or packaging mistake, not a DSP one, and every one was
avoidable by testing before writing to a live machine.

1. **Wrong property key.** `{d3993a3f-...},5/6/7` are
   `PKEY_SFX/MFX/EFX_ProcessingModes_Supported_For_Streaming`, REG_MULTI_SZ lists
   of signal-processing modes. They hold
   `{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}` = `AUDIO_SIGNALPROCESSINGMODE_DEFAULT`
   on every endpoint. That was mistaken for Equalizer APO's CLSID and overwritten,
   which disabled the endpoint's effects instead of replacing them. The APO
   CLSIDs are under `PKEY_FX_*EffectClsid`, `{d04e05a6-...},1/2/5/6/7`, exactly
   as plan 5.2 says.
2. **`Set-ItemProperty` asks for too much.** The PowerShell registry provider
   opens for write with `KEY_WRITE` (SetValue + CreateSubKey +
   STANDARD_RIGHTS_WRITE). An endpoint's `FxProperties` grants Administrators
   only `SetValue, ReadKey`, so the open is refused and the error reads like a
   privilege problem. `reg.exe import` fails identically. Open via
   `RegistryKey.OpenSubKey(path, ReadWriteSubTree, SetValue|QueryValues)`.
3. **`RegisterAPO` is not idempotent.** It fails when the CLSID is already
   registered, so a second `regsvr32` returns failure even though the machine is
   already in the desired state. `DllRegisterServer` now calls `UnregisterAPO`
   for both CLSIDs first.
4. **The DLL must not live in a user directory.** audiodg hosts APOs as
   LocalService. A build-tree DLL has an ACL of SYSTEM/Administrators/user, so
   the load fails and `IAudioClient::Initialize` returns `E_ACCESSDENIED`
   (0x80070005) for the **entire endpoint**, not just the effect. The installer
   now stages the binary to `%ProgramFiles%\Isotone\` and verifies something in
   Users/Everyone/LOCAL SERVICE can read it. Plan 5.4 states this; it was applied
   to the config path in `isoapo.cpp` and not to the binary.

Two PowerShell traps also cost a round trip each, both worth remembering:

- `2>&1` on a native command: in Windows PowerShell 5.1 stderr becomes
  `NativeCommandError` records, which `$ErrorActionPreference = 'Stop'` promotes
  to terminating. That killed the very fallback path added to handle a failure.
- `[ordered]@{5=...; 6=...}` is an `OrderedDictionary`, which indexes by
  **position** for an integer subscript. `$plan[5]` on a two-element plan yields
  `$null` silently, and `$null.Clsid` is an empty string that then sails through
  a `Test-Path`. Iterate with `.GetEnumerator()`.

The real fix was structural: `install.ps1` gained a `-DryRun` switch that runs
every check and prints every intended write without elevation or side effects.
It should have existed before the first live run, not after the fourth failure.

### IsoAPO applies twice when installed in two slots

The fit says `n_iso = 2`: the CLSID is in both the SFX (`,5`) and MFX (`,6`)
slots and both instances process. Equalizer APO occupies both slots on the same
endpoint and measured as applying only once, so its pre-mix and post-mix classes
must not both do the filtering; ours are the same object registered twice.

For the product, install into one slot. MFX (`,6`, mode effect) is the right
default: it is per-endpoint rather than per-stream, so one instance sees the
mixed output. This also means `devicetool status` must report every slot an APO
occupies, and warn when the same CLSID appears in more than one.

---

# Where things stand (end of 2026-09-12)

## Done

| Stage | State | Evidence |
|---|---|---|
| 0. Repo, CI, decisions log | complete | CI workflow builds MSVC + GCC, runs tests and the APO self test |
| 1a. Compat backend spike | complete | measured differential matched the analytic filter to 0.001 dB |
| 1b. Fork spike (IsoAPO) | complete | measured in audiodg to 0.0002 dB rms |
| 1c. Linux spike | deferred | no Linux environment on this machine; owner's decision |
| 2. Core | complete | 89 cases / 1,973,709 assertions green on MSVC 19.51 and GCC 16.1.0 |

CI is green on GitHub for all three jobs: `core (windows-latest)`,
`core (ubuntu-latest)` and `reference data is reproducible`. The first push
failed two of them, both environmental:

- The reference-data job compared regenerated files with `git diff`. Two scipy or
  libm builds disagree in the last bit or two of a 17-significant-digit double,
  so a textual comparison fails anywhere but the machine that generated the
  files. `gen_reference.py --check` now compares numerically at 1e-9 dB, five
  orders of magnitude tighter than the 0.01 dB the C++ tests assert. Confirmed it
  still catches a deliberate 1e-6 dB perturbation and names the case and field.
- The APO self test looked for its exe at a fixed path. The Windows runner uses a
  multi-config generator, which adds a per-config subdirectory. The exe is now
  located rather than assumed, and run from its own directory so `LoadLibrary`
  finds `IsoAPO.dll` beside it.

## What exists

```
core/                  the DSP core: types, biquad design, response evaluation,
                       processor (TDF-II + smoothing + crossfades), APO config
                       parser/formatter, param block schema
core/tests/            7 test files; reference data from scipy at 4 sample rates
tools/gen_reference.py independent scipy implementation that generates it
windows/apo/           IsoAPO.dll, its offline self test, install.ps1
windows/measure/       isotone-measure: endpoint list, stepped-sine measurement
docs/decisions.md      this file
```

## Not started, in dependency order

**Stage 3** is next and needs no new decisions:

1. **Shared memory transport.** `param_block.h` defines the layout and the
   seqlock, both tested. What is missing is the mapping itself:
   `Global\IsoAPO.<endpoint-guid>` created by the APO with a DACL granting
   Authenticated Users read/write and LocalService/SYSTEM full control
   (plan 5.3), opened read/write by the UI side. The APO currently reads a
   config file at `LockForProcess` instead.
2. **Audio ring.** `AudioRingHeader` is declared and unimplemented. Single
   writer (host), single reader (UI), write index published after the samples,
   about 250 ms of frames.
3. **Heartbeat and host state** in the param block header, so the UI can tell a
   live engine from an idle one.
4. **`devicetool`.** The CLI wrapping upstream's `DeviceAPOInfo` and
   `RegistryHelper` for list/status/install/uninstall/repair/test with JSON
   output. This replaces `install.ps1`, which is a spike. It must report every
   effect slot an APO occupies and warn on duplicates (see the 2x finding).
5. **Compat backend as a real `EqBackend`**: atomic writes via `MoveFileExW`,
   rate limiting to about 30 writes/second, `Device:`/`Channel:` emission,
   WASAPI loopback as the spectrum source.

**Stage 4 onward** is the UI, still framework-undecided. Nothing in stages 0-3
depends on that choice.

## State of the owner's machine

**Clean.** IsoAPO was uninstalled by the owner after the stage 1b measurement.
Verified afterwards:

- The VB-Cable "CABLE Input" endpoint is back on Equalizer APO, and the
  processing-mode declarations are REG_MULTI_SZ as they should be.
- Both IsoAPO CLSIDs are gone from `AudioEngine\AudioProcessingObjects` and from
  `HKLM\SOFTWARE\Classes\CLSID`.
- Re-measuring the endpoint gives -26.384 dB at 1 kHz, identical to the original
  baseline taken before any work started.
- `config.txt` and `peace.txt` are byte-identical to the pre-work snapshot.

One inert leftover: `C:\Program Files\Isotone\IsoAPO.dll` is still on disk.
It is unregistered and nothing references it; audiodg still had it mapped when
the uninstall ran. Stage 3 will overwrite it on the next install anyway.

To reinstall for stage 3:
`install.ps1 -Endpoint '{798436d2-8c71-4834-9248-00ccbaaca00a}' -Dll <path> -DryRun`
first, then without `-DryRun`, then `Restart-Service Audiosrv -Force`. The
pristine endpoint values are in `windows/apo/IsoAPO-backup-Render-798436d2-....reg`.

## Standing rules

- No commits or pushes without the owner saying so each time.
- Never modify the owner's live Equalizer APO configuration.
- Never hand the owner registry code that has not been run. `install.ps1 -DryRun`
  executes every check unelevated with no side effects.
