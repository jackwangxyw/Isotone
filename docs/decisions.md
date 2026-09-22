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

## 2026-09-12: Stage 3, shared region, audio ring and heartbeat (offline)

Built and verified offline; not yet run inside audiodg.

- **One mapping per endpoint**, `Global\IsoAPO.{guid}`, holding
  `[ParamBlock][AudioRingHeader][32768 * 8 float32]`, about 1 MB.
  `kParamVersion` is now 2 and covers the whole region.
- **The GUID in the name is lower-cased.** Measured: kernel object names are
  case-sensitive (a `Local\` mapping created as `...ABC` does not open as
  `...abc`), and the two sides see different cases: `PKEY_AudioEndpoint_GUID`,
  which the APO reads, is upper case, while `IMMDevice::GetId` is lower case
  (checked on all six active endpoints here).
- **Endpoint identity** comes from `APOInitSystemEffects::pAPOEndpointProperties`
  via `PKEY_AudioEndpoint_GUID`, as upstream `EqualizerAPO::Initialize` does.
  `Initialize` now refuses a payload that is not `APOInitSystemEffects` or
  `APOInitSystemEffects2`, as upstream does.
- **DACL** is `D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GRGW;;;AU)`. Read back from a live
  mapping as `(A;;CCDCLCRC;;;AU)`: query, map-read and map-write only.
- **Hosts open before creating.** `CreateFileMappingW` on an existing object
  requests full access, which the DACL gives only to SYSTEM and LocalService.
  The first version created-or-opened in one call and every second instance
  failed with `ERROR_ACCESS_DENIED`; the self test caught it.
- **Self test uses `Local\`.** Creating a `Global\` object needs
  `SeCreateGlobalPrivilege`, and `whoami /priv` confirms an unelevated shell
  lacks it. CMake builds the same sources twice: `IsoAPO.dll` (Global) and
  `IsoAPO-selftest.dll` (Local). That constant is the only difference, and the
  self-test DLL must never be installed.
- **Ring protocol.** Capacity is a power of two, so frame counters wrap at 2^32
  with no special case. The writer publishes `pending_index` before writing and
  `write_index` after; the reader drops any copied frame more than `capacity`
  behind `pending_index`. With that check disabled, the threaded test received
  741,954 torn chunks; with it enabled, zero out of 1.05 million.
  A layout change (channel count, new owner) runs under an `epoch` seqlock.
- **Ring writer election** (plan 5.3): the token is `pid << 32 | instance`.
  First to claim writes; the rest stay quiet. A claim whose process id differs
  from the claimant's is treated as left by a dead audiodg and taken over,
  because the UI keeps the region alive across an engine restart.
- **The host never trusts layout fields in shared memory.** Any authenticated
  user can write the mapping, so the ring writer uses private copies of capacity
  and channel count; a test scribbles on the shared ones and checks a guard band.
- **Two bugs fixed at the trust boundary, each with a test that failed first:**
  - `Processor::set_target` passed NaN and infinity to the smoothers, and a NaN
    smoother never recovers, so one bad write silenced the device until the
    stream restarted. Non-finite targets now keep the previous target.
  - `param_block_write` used `seq + 1`/`seq + 2`. A UI killed mid-write leaves
    seq odd, after which every write inverts the lock: finished writes rejected,
    torn ones accepted. Now `seq | 1` and `(seq | 1) + 1`.
- **Heartbeat** increments once per `APOProcess`; `host_state` becomes Running at
  `LockForProcess` and is not reset at unlock, because other instances may still
  be running. The heartbeat is the liveness signal.
- **Cold start is unchanged from stage 1b.** A newly created region is seeded
  from `%ProgramData%\IsoAPO\config.txt` or the default -12 dB band; an existing
  region is never re-seeded. If the region cannot be created, audio keeps
  flowing on that seed and the failure goes to `OutputDebugString`.

## 2026-09-12: Every channel of the stream is processed; channel addressing fixed

`kMaxChannels = 8` was doing two jobs: how many channels can carry a trim (a
wire-format limit, plan 4.7) and how many channels the processor runs. The APO
handed a 12-channel buffer to a processor clamped to 8, which then walked the
interleaved buffer with a stride of 8. Measured in the APO self test on a 7.1.4
stream before the fix: channel 2 at -2.924 dB and channel 11 at -0.745 dB where
-3.000 was expected, and channel 10's own band never applied.

- **The processor now runs the stream's channel count.** `kMaxChannels` stays 8
  and means trims and ring storage only; channels past it have a 0 dB trim.
- **`kMaskChannels = 32`.** A `ChannelMask` names channels 0 to 31; channels past
  that get all-channel bands only. The mask tests in `band_affects_channel` and
  the processor were shifting a `uint32_t` by the channel number, which is
  undefined from 32 up and wraps on x86. Measured: `magnitude_db` for channel 33
  returned channel 1's -12 dB band.
- **`composite_peak_db` covers every channel**, not the first 8, so auto preamp
  protects height channels.
- **Importer, matched to upstream `ChannelFilter`/`ChannelHelper`:**
  - Numbered channels are accepted up to 32; they were capped at 8.
  - A `Channel:` line that selects nothing now scopes the following filters to
    nothing, with a warning. It used to mean all channels, which put a filter
    for a nonexistent or misspelt channel on every speaker. Upstream starts from
    an empty selection.
  - The exporter writes mask bits past the eight named channels as numbers, so
    they survive a round trip.
- **`Preamp:` lines are gain stages on the current selection**, per upstream's
  `PreampFilterFactory` ("Adjusting preamp by"). They now add, and inside a
  `Channel:` scope they become trims. Before, the last line overwrote the global
  preamp, so the exporter's own `Channel: R` / `Preamp: -3 dB` trim came back as
  a -3 dB global preamp and no trim.

Channel names were still a fixed table at this point; resolved in the next
entry.

## 2026-09-12: isotone-shm, a cross-process check, and two install.ps1 fixes

- **`windows/shmtool` builds `isotone-shm`**, the UI side of a region from the
  command line: `status` (header, heartbeat sampled 200 ms apart, ring),
  `write <config.txt | ->` with optional `--bypass`, and `capture <seconds>
  <out.wav>` from the ring. JSON on stdout; exit 1 when the region cannot be
  used, 2 on bad arguments. `--local` targets the self-test DLL's namespace.
  This is the "tiny CLI" of the stage 3 acceptance test.
- **`isotone-apo-selftest --serve <guid> <seconds>`** hosts the self-test DLL in
  real time (1 kHz sine, 10 ms blocks) and prints the level at 1 kHz once a
  second.
- **`tools/check_shm_transport.py`** runs the two against each other in separate
  processes, which is the only way to meet the mapping's DACL as the UI will.
  Local result: a written -6 dB band measured -6.000 dB in audio captured back
  from the ring, bypass measured 0.000 dB, malformed and empty input produce
  valid JSON, and the region disappears when the last holder exits. Added to the
  Windows CI job; not yet run on GitHub.
- **`install.ps1` now installs into MFX (`,6`) only**, as the stage 1b entry
  concluded. The script still had SFX and MFX, which would have repeated the
  applied-twice result.
- **`install.ps1 -DryRun` works on a clean machine.** It skipped `regsvr32` and
  then required the CLSID to be registered, which only held while a previous
  install was still in place. After the owner's uninstall it threw. The dry run
  now reports the missing registration; a real run still fails on it.

## 2026-09-12: Channel names resolve through the device's speaker layout

Owner's direction: use the standard, do not invent one. The standard is
Windows' speaker mask as Equalizer APO applies it. Read from upstream
`helpers/ChannelHelper.cpp` and `FilterEngine::initialize`:

- A layout is a channel count plus the stream's `dwChannelMask`. Walking bits
  0 to 30, each present position is named if upstream has a name for it (L R C
  LFE RL RR RC SL SR), otherwise numbered by its channel index; channels past
  the mask are numbered.
- A mask of 0 falls back to `getDefaultChannelMask`: mono, stereo, quad, 5.1
  surround, 7.1 surround.
- Lookup: a word starting with a digit is a 1-based number that must be within
  the channel count; otherwise a name, with SL/RL, SR/RR and SUB/LFE standing
  in for each other.

`parse_apo_config` and `format_apo_config` now take a `ChannelLayout`,
defaulting to 7.1 surround, which is exactly the fixed table they used before.
So `SL` is channel 5 on either 5.1 layout and channel 7 on 7.1, and numbered
channels are valid up to the device's count rather than a fixed 32.
`isotone-shm write` parses against the published channel count with upstream's
default mask; the header does not carry the speaker mask. The UI will have the
real mask from the device's mix format.

## 2026-09-12: Stage 3 measured in audiodg

IsoAPO installed by the owner into MFX (`,6`) only on CABLE Input, with the
build from before the channel-name entry above. That entry changed only the
config importer, which the running engine uses solely for a seed file that does
not exist, so the transport under test is the same code.

- **The region exists and is reachable.** `isotone-shm status`, unelevated,
  opened `Global\IsoAPO.{798436d2-...}` created by audiodg: 48 kHz, 2 channels,
  running, heartbeat advancing, ring claimed by audiodg's process id. This
  settles the two inferred points: audiodg can create `Global\` objects, and it
  passes an `Initialize` payload IsoAPO accepts. The ring token's instance
  serial was 3, then 6 after a stream restart, so audiodg constructs several
  instances; exactly one wrote the ring.
- **The acceptance measurement.** Render to CABLE Input, capture from CABLE
  Output, 14 frequencies from 31.5 Hz to 16 kHz. A bypass block was written
  through the region and measured as a baseline; then a block of preamp -3 dB,
  LSC 105 Hz +6.4 dB Q 0.7, PK 1 kHz -12 dB Q 1 and HSC 8 kHz -4 dB Q 0.7. The
  difference matched `tools/gen_reference.py`'s independent scipy design of that
  curve to **0.0001 dB rms and 0.0001 dB maximum**, on both channels.
- **The ring is exact.** A steady 1 kHz tone captured from the ring measured
  -6.203 dB in bypass, -21.202 dB with the curve, -6.203 dB in bypass again:
  -14.999 dB applied, against -14.999 dB designed. The ring does not read -15
  absolute because the signal already arrives at MFX 6.203 dB below the played
  amplitude. That loss is before IsoAPO and was not attributed; endpoint volume
  and Equalizer APO's pre-mix class still in SFX are the candidates.
- The region lasts only while a stream is open, so the measurement held a
  silent stream open. With nothing playing the UI will see "engine idle", as
  plan 5.3 expects.

## 2026-09-12: devicetool, wrapping upstream's registration code

`windows/devicetool` builds `isotone-devicetool`: list, status, install,
uninstall, repair, test and roundtrip, with JSON output and exit codes 0/1/2.
It vendors `DeviceAPOInfo`, `AbstractAPOInfo`, `RegistryHelper` and
`StringHelper` from `mirror/equalizerapo` at
`53d885f7f1a097b457e17a5206b7d60f647877a8` (2024-09-27), as plan 5.2 says.
`windows/devicetool/upstream/VENDORED.md` lists every change.

- **Licence.** Every vendored file is GPL "or later". `ScopeGuard.h` is not
  vendored: it includes folly's Apache-2.0 `UncaughtExceptions.h`. `stdafx.h`
  defines the one macro it provided.
- **Dry run.** Upstream's install and uninstall logic runs unchanged.
  `RegistryHelper` sends every write (values, keys, ownership, ACLs and the
  `.reg` backup file) to a `RegistryDryRun` when one is set, returning before any
  system call, and that sink answers the reads that follow those writes.
  `--dry-run` needs no elevation. Checked: every write function is gated at its
  first line, and nothing outside `RegistryHelper` writes the registry.
  `roundtrip` chains install, a simulated driver update, repair and uninstall in
  one dry run and checks the effect slots come back exactly.
- **What gets installed.** The post-mix class only, in MFX by default (the stage
  1b finding). Install refuses when Equalizer APO is on the endpoint unless
  `--replace-equalizerapo` is passed, and never records an Equalizer APO class as
  the child.
- **Install records.** devicetool writes upstream's record under
  `HKLM\SOFTWARE\IsoAPO\Child APOs\{guid}`, so uninstall restores the original
  slot values. `install.ps1` writes none; devicetool reports such an install as
  unrecorded and will not uninstall it.
- **Upstream behaviour to know.** `INSTALL_SFX_MFX` deletes LFX/GFX and
  `INSTALL_LFX_GFX` deletes SFX/MFX/EFX while installed (restored on uninstall).
  The extra effect lists `,13`/`,14`/`,15` are never touched. `saveToFile` writes
  a `.reg` with a doubled `HKEY_LOCAL_MACHINE` root that would not import (read,
  not run). `checkAPORegistration(true)` shells out to `regsvr32` on a DLL next to
  the exe, a build-tree path audiodg cannot read; nothing calls it with `true`.
- **Verified on this machine, read-only.** `status` on all 46 endpoints matches
  the registry, including CABLE Input as `conflict` (IsoAPO unrecorded in MFX,
  Equalizer APO pre-mix in SFX) and Equalizer APO's `ConfigPath`. A dry-run
  install on CABLE In 16ch writes the same FxProperties value as `install.ps1
  -DryRun`, plus the record. A fingerprint of all 893 FxProperties values on
  every endpoint was identical before and after a status, roundtrip and dry-run
  install session. No real install has been run.
- **Not handled yet.** IsoAPO does not wrap a child APO, so a vendor APO it
  replaces stops processing while installed; devicetool warns.
  `--replace-equalizerapo` replaces the target slot only, so the endpoint still
  reports a conflict. Realtek's `,19` and `,20` values are not reported.
- **Fixed on the way.** `install.ps1 -DryRun` ran `reg export` and wrote a backup
  file; it now only reports it.

## 2026-09-12: UI direction, and multichannel features added to scope

Owner's decisions while reviewing the main-screen mockups
(`docs/design/mockups`, gitignored; published as a design canvas):

- **Framework leaning: Qt 6 Quick**, pending a one-screen prototype that matches
  the mockups. Look: MUSE-like, not dense pro-audio.
- **Themes:** System, Dark, Light and Custom (user base colours), blue default
  accent. Band colours are an option: one accent or a colour per band.
- **Layout:** one layout with a collapsible sidebar (labelled with outputs when
  open, icon rail with an outputs popover when collapsed). The Channels panel
  collapses too.
- **Bands:** the strip scrolls sideways; band order is Manual or By frequency.
- **Balance** is shown as a number from -1.0 to +1.0 in 0.1 steps, as Peace does
  (confirmed in Peace's help file, `releasenotes.htm`). Its gain mapping is not
  decided; the proposal is to attenuate the opposite side only, reaching silence
  at the ends, never boosting.
- **Mono is removed, UI and core.** `EqState::mono`, the processor's downmix
  and the param block field are gone; `kParamVersion` is 3 and `reserved` grew
  to keep the layout aligned. The compat backend worktree still emits `Copy:`
  for mono and must drop it before merging.
- **No draggable room map** for speaker placement. Speaker setup is a table
  (level, distance or delay, polarity, test, mute, solo), as JRiver's Room
  Correction and Roon's speaker setup present it. No explainer microcopy in
  the UI.
- **Added to scope for multichannel outputs** (the owner asked for what 2.1, 5.1
  and 7.1 users need, without clutter). Shown only when the active output has
  more than two channels:
  - speaker groups (named channel sets a band can target);
  - per-speaker level, delay set by distance or directly, polarity, mute, solo
    and a test tone;
  - bass management: a crossover sending bass from small speakers to the sub,
    and an LFE low-pass;
  - routing: stereo upmix, swap front and rear, swap left and right, and a global
    delay for lip sync.

  This supersedes the plan's deferral of channel delay and swap (4.8). Crossfeed
  stays out (1.1).

Engine work this implies, none of it built yet: per-channel delay lines
(preallocated, crossfaded when changed), per-channel polarity, a channel mixing
matrix that implements swap, upmix and the bass-management sum in one stage,
Linkwitz-Riley 24 dB/oct crossover filters, named groups over the existing
channel masks, and a param-block layout change to carry all of it.

## 2026-09-12: UI framework is Qt 6 Quick

Owner's decision, replacing plan section 2 item 4 (Electron). The UI is QML with
C++ in one process, linking `isotone_core` and `isotone_transport` directly, so
the plan's `/bridge` N-API addon and its IPC are dropped, and the EQ-by-ear tone
becomes native audio. The build brief is `docs/ui-spec.md`. No UI code yet; Qt
is not installed on this machine.

## 2026-09-12: Multichannel speaker features in the engine

`SpeakerSetup` in `EqState` carries per-speaker delay, lip sync, polarity,
speaker mute, swap left/right, swap front/rear, stereo upmix (off, all,
no centre) and bass management (crossover, small-speaker mask, LFE low-pass).
Speaker groups need no engine work: a band already targets a channel mask. Solo
is UI over speaker mute. The per-speaker test tone is not built; it belongs with
the EQ-by-ear tone generator (stage 5).

- **Stage order:** routing matrix (upmix, then the swaps) → bass management →
  preamp, bands, trim and post gain → delay. Polarity and speaker mute are a
  signed per-channel gain folded into the post gain, so they are smoothed like
  every other gain.
- **Speaker positions** come from the stream's speaker mask (channel n is the
  n-th set bit). IsoAPO passes `dwChannelMask`, or upstream's default mask for
  the channel count when it is 0. Missing speakers make their features no-ops;
  bass management needs an LFE channel.
- **Routing** is one 8×8 matrix built by `routing_matrix` in
  `core/speakers.cpp`. Upmix sends each front to the side and back of its own
  side at -3 dB; "all" also sends half of each front to the centre. The swaps
  permute outputs after upmix. The matrix is smoothed per control block and
  interpolated per sample, so a swap while playing crossfades; identity takes a
  fast path.
- **Bass management:** Linkwitz-Riley 24 dB/oct (two Butterworth biquads) at the
  crossover. A small speaker keeps the high-pass; the sum of the low-passes goes
  to the LFE, whose own content gets an LR4 low-pass at the LFE frequency. LR4
  low and high sum flat, which the test checks. Turning it on or off crossfades,
  and filter state is cleared while it is off.
- **Delay:** a ring buffer per channel sized for 1 s at the stream rate,
  preallocated in `initialize`. Delay is whole samples,
  `floor(ms * rate / 1000 + 0.5)`, the rounding Equalizer APO's `DelayFilter`
  uses, so both backends agree. Speaker delay plus lip sync is capped at 1 s.
  A change crossfades from the old tap to the new one over 10 ms.
- **Param block v4:** `ParamSpeakers` (80 bytes) sits between the channel gains
  and the bands, and the header grew to 48 bytes to carry `speaker_mask`, which
  the host publishes with the format. `isotone-shm write` resolves channel names
  with it instead of assuming the default mask.
- The field is `small_speakers`, not `small`: `rpcndr.h` defines `small` as a
  macro.
- **Tests** (`core/tests/test_speakers.cpp`, 15 cases): positions, delay and lip
  sync on an impulse, delay change without a click, polarity, mute, all swaps and
  a swap while playing, both upmixes, crossover levels against the analytic LR4
  response and a flat sum, LFE low-pass, a layout without LFE, bass management on
  then off without a click, hostile values, the param block round trip, and the
  speaker text format. Each click and step test was checked by breaking the code
  it guards. Two did not fail at first: the polarity switch landed on a zero
  crossing, and enabling bass management from rest is click-free by nature. Both
  were moved to where the bug shows.
- **Measured offline in the APO:** the self test writes a delay of 1.3 ms and an
  inverted channel through the region and finds both in the output to 1e-4, and
  its 7.1.4 stream publishes its speaker mask. `check_shm_transport.py` does the
  same from another process: 0.25 ms on channel 1 reads -90.00° at 1 kHz,
  inverting it +90.00°, muting it -200 dB with channel 0 unchanged.
- **Cost:** 8 channels, 12 bands, one thread, test-signal generation included:
  76× real time with speaker features off, 66× with delay, upmix, swap, polarity
  and bass management all on.

## 2026-09-12: Compat backend merged

`windows/compat` (built in an agent worktree) is reviewed and merged, and builds
with MSVC from the root `CMakeLists.txt`. `isotone-compat` locates Equalizer
APO, attaches or detaches one `Include: Isotone.txt` block at the end of
`config.txt` (backing it up first), writes per-device blocks to `Isotone.txt`
atomically with coalescing for live edits, reads them back, and captures WASAPI
loopback for measurement.

- **The guard.** Tools reach the live config directory only through
  `--real-install`. The first version compared path text and let
  `C:\PROGRA~1\EqualizerAPO\config` through onto the owner's file during a test
  run (restored, hash-checked against `sim/reference`). It now compares volume
  serial and file ID, so short names, junctions, case and slashes are all caught;
  the test exercises each.
- **Mono** is gone from the backend as from the core.
- **Speaker features in upstream's commands.** Each device block writes, in the
  processor's stage order: a routing section of `Copy:` rows from the same
  `routing_matrix`; bass management as a virtual channel `ISOTONEBASS` collecting
  the small speakers' bass, LR4 `LPQ`/`HPQ` pairs (`Filter: ON LPQ Fc f Hz Q
  0.707106781187` twice), and `Copy: LFE=1*LFE+1*ISOTONEBASS`; then the curve; then
  an output section with `Copy:` for polarity and mute and `Delay: x ms` per group
  of channels with the same delay. The `SpeakerSetup` itself is a `# Isotone:
  speakers` comment so the file reads back exactly.
- **Upstream semantics, from source** at `53d885f7`: `CopyFilter` reads every
  input before writing, channels it does not assign keep their samples, an unknown
  target name becomes a virtual channel zeroed each block and never output, a lone
  token is a factor only if it is "0" or contains '.', and `DelayFilter` rounds
  `ms * rate / 1000 + 0.5`.
- **Equivalence test.** `compat_tests` includes a small interpreter of those
  commands with upstream's semantics. It runs the text Isotone writes for a 5.1
  device with bands, a trim and every speaker feature on, and compares with
  `Processor` on the same input: worst difference below 1e-4, with the signal
  shown to have changed by more than 0.2 so a no-op cannot pass. Four deliberate
  emission bugs (one crossover biquad instead of two, no LFE bass sum, polarity
  dropped, lip sync ignored) each failed it.
- `format_speaker_setup` and `parse_speaker_setup` live in core, so
  `isotone-compat apply --speakers` and `isotone-shm write --speakers` take the
  same text.
- `isotone-measure measure` now also reports each channel's phase relative to
  channel 0, which is how delay and polarity will be checked on CABLE Output.
  Not yet run against a live endpoint.
- **Not verified live:** nothing in this entry has run against the installed
  Equalizer APO. The interpreter is a model of upstream, read from its source.

## 2026-09-12: devicetool install and speaker features measured in audiodg

The owner, elevated, removed the `install.ps1` install from CABLE Input,
restarted the audio service, staged and registered the current `IsoAPO.dll`,
ran `isotone-devicetool install {798436d2-...} --replace-equalizerapo`, and
restarted again. `status` afterwards: IsoAPO in MFX with an install record
holding the original slots, Equalizer APO's pre-mix class still in SFX, staged
DLL hash-identical to the build. That install printed `"operations":[]`
because only the dry-run sink filled the list; the writes happened. Fixed
below.

With a silent stream held open, `isotone-shm status` read version 4, 48 kHz,
2 channels, speaker mask `0x3`, heartbeat advancing. A flat config was then
written with four speaker settings, each measured with `isotone-measure` from
CABLE Input to CABLE Output. Channel 1's phase relative to channel 0, minus the
baseline:

| `--speakers` | 250 Hz | 1 kHz | expected (12 samples) |
|---|---|---|---|
| (default) | 0.00° | 0.00° | baseline |
| `delay_ms=0,0.25` | -22.500° | -90.000° | -22.5°, -90° |
| `delay_ms=0,0.25 inverted=0x2` | +157.500° | +90.000° | +157.5°, +90° |
| `muted=0x2` | channel 1 digital silence, channel 0 +0.000 dB | same | |

Channel 1's level moved 0.000 dB under delay and inversion. This was the first
use of `isotone-measure`'s phase output. Upmix, swaps and bass management were
not measured live: both cable endpoints are 2-channel.

## 2026-09-12: devicetool lists what a real run wrote

A real `install`, `uninstall` or `repair` now lists the registry writes it made.
`RegistryHelper` (vendored, marked `Isotone modification`) reports each write to
an installed `RegistryLog` after the write returns without throwing, through a
guard declared at the top of each of the ten write functions. A failed run
prints the writes that succeeded before the failure, so the output is what
changed. Checked with a scratch program against a throwaway HKCU key: create,
set, a denied HKLM write, delete value, listed as exactly the three that
succeeded, and nothing after the log's scope ended; the key was removed. Dry-run
output is unchanged (13 operations for the CABLE In 16ch install, roundtrip
restores). Not yet seen on a real elevated install.

## 2026-09-12: Compat backend measured against Equalizer APO

Ran against the owner's Equalizer APO 1.4.2.0 with Peace closed. `config.txt`
matched the `sim/reference` snapshot before; `isotone-compat attach
--real-install` appended the Include block; each state was written with `apply
--real-install`; `detach --real-install` removed it and the file matched the
snapshot again. The run detached in a `finally`, so a failure could not leave the
block behind.

`Isotone.txt` was scoped to CABLE Output's capture endpoint, where Equalizer APO
processes (stage 1a), so IsoAPO on CABLE Input played no part: a silent stream
held its region open with a flat config written through `isotone-shm`. Measured
CABLE Input to CABLE Output at 250 Hz, 1 kHz and 4 kHz, relative to a baseline
taken before attaching, with `PK 1000 Hz -6 dB Q 1` in every state:

| State | Result |
|---|---|
| band + `delay_ms=0,0.25` | -0.423 / -6.000 / -0.405 dB on both channels, matching the RBJ response to 0.001 dB; ch1 phase -22.50°, -90.00°, 0.00° (12 samples) |
| + `inverted=0x2` | ch1 phase +157.50°, +90.00°, -180.00° |
| band + `muted=0x2` | ch0 has the band, ch1 digital silence |
| `--bypass` with the same settings | +0.000 dB and +0.00° at every frequency |
| after detach | +0.000 dB and +0.00° at every frequency |

At 4 kHz, 12 samples is a whole cycle, so that frequency checks inversion and
not delay. This is the first time the speaker commands (`Copy:`, `Delay:`)
have run in the real Equalizer APO rather than the model of it. Routing and
bass management commands (`Copy:` sums, the virtual channel, `LPQ`/`HPQ`) remain
checked only against the model, since both cable endpoints are 2-channel.

Left in the config directory for the owner to delete, since the harness will not
let a session delete there: `Isotone.txt` (no longer included) and
`config.txt.isotone-backup`.

## 2026-09-12: Routing and bass management measured at 7.1, both backends

VB-Cable 3.3.1.7 needed nothing new: CABLE Input, CABLE In 16ch and CABLE Output
all accept 2 to 16 channels (int16 and int24, not float) in exclusive mode,
checked with `IAudioClient::IsFormatSupported`. The owner set CABLE Input and
CABLE Output to 8 channels, 24-bit, 48 kHz, mask `0x63F`, with a scratch tool
over `IPolicyConfig::SetDeviceFormat` (the call behind the Sound control panel's
default format), after its `GetDeviceFormat` read matched the registry's
`PKEY_AudioEngine_DeviceFormat` byte for byte. The Claude Code permission
classifier would not let the session make that change. Both were 2 channels,
24-bit, mask `0x3` before.

`isotone-measure measure` gained `--channel-gains`, a level per render channel,
so a swap is visible. Each channel played the tone at 1, 0.5, 0.8, 0.6, 0.9,
0.3, 0.7, 0.4; the capture read each at its own level to 0.01 dB, so the cable
carries 8 channels 1:1. Measured at 40, 80, 160 Hz and 1 kHz. Expected values
came from the core `Processor` run on the same input (scratch `expect.exe`):
each channel's output divided by its input, which a baseline-relative
measurement reads because `peace.txt` is `Channel: all` on both cable ends.
Compared per channel on magnitude (0.02 dB) and on phase relative to channel 0
(0.2°); channels expected below -40 dB only had to measure below -35 dB.

| Case | IsoAPO (audiodg, CABLE Input) | Equalizer APO (Isotone.txt, CABLE Output) |
|---|---|---|
| swap left/right | 0.0002 dB, 0.001° | 0.0002 dB, 0.001° |
| swap front/rear | 0.0001 dB, 0.002° | 0.0001 dB, 0.002° |
| upmix all | 0.0001 dB, 0.002° | 0.0002 dB, 0.001° |
| upmix no centre | 0.0002 dB, 0.002° | 0.0001 dB, 0.001° |
| bass management, all small, 80/120 Hz | 0.0017 dB, 0.008° | 0.0001 dB, 0.000° |
| bass management, backs small, 100/150 Hz | 0.0013 dB, 0.015° | 0.0000 dB, 0.000° |
| all of it, plus a 1 ms centre delay and an inverted back left | 0.0004 dB, 0.006° | 0.0001 dB, 0.002° |

Worst error over all channels and frequencies. Equalizer APO bypass of the last
case measured as the baseline. Negative control on the saved data: every
measurement was compared with every case's expectation, and each passed only
against its own. `config.txt` matched the snapshot before attaching and after
detaching.

With this every speaker feature has been measured live in both backends. The
virtual `ISOTONEBASS` channel, `LPQ`/`HPQ` pairs and `Copy:` sums behave in the
real Equalizer APO as the model of it said.

## 2026-09-12: Balance, spectrum, test tone, replacing Equalizer APO

Owner's decisions (details in `docs/ui-spec.md`):

- **Balance** turns down the opposite side only, linearly (gain 1 − |b|), muting
  it at ±1.0; the favoured side never changes.
- **Pre spectrum is dropped.** Deriving it as post / |H| was already noisy under
  deep cuts and is wrong once speaker routing, bass management or delay mix
  channels. The top bar's spectrum control is On / Off; the design screens were
  regenerated with it.
- **Per-speaker test tone:** pink noise, one speaker at a time, −30 dBFS RMS, the
  level home-theatre test discs use against a 75 dB C target (AVIA-style discs
  use −20 dBFS against 85 dB C). Played by the UI through the engine.
- **Installing IsoAPO where Equalizer APO is** removes Equalizer APO from that
  endpoint, when the user chooses it (Devices and the stage 6 setup wizard).

`isotone-devicetool install --replace-equalizerapo` now does that: after
upstream's install it deletes Equalizer APO's classes from the effect slots the
install did not write. Upstream's install records every slot's original value
before writing, and its uninstall writes each back, so uninstall restores
Equalizer APO. Dry run on CABLE In 16ch (Equalizer APO in SFX and MFX): IsoAPO
into MFX, SFX deleted. `roundtrip --replace-equalizerapo` (install with the
removal, simulated driver update, repair, uninstall) restores both classes and
fails if Equalizer APO is left after install. Without the flag nothing else
changes. `repair` does not remove Equalizer APO again. Not yet run for real:
CABLE Input's install predates this and still has Equalizer APO's pre-mix class
in SFX.

Still unhandled: Equalizer APO's own record under
`HKLM\SOFTWARE\EqualizerAPO\Child APOs\{guid}` stays. If the user later runs
Equalizer APO's Device Selector or uninstaller, it would write its recorded
originals back over IsoAPO. Not tested.

**`status` reports `,19` and `,20`.** The Windows SDK names them
`PKEY_CompositeFX_Offload_StreamEffectClsid` and `_ModeEffectClsid`, the effect
lists for hardware-offloaded streams. On this machine both Realtek endpoints
(unplugged) hold Realtek's own classes there (`RltkAPOU64.dll`), next to
Realtek classes in the composite lists `,13`–`,15`, while Equalizer APO sits in
`,5` and `,7`. Whether audiodg runs the single-CLSID slots at all when composite
lists are present is not known; it matters for both backends on Realtek
hardware and needs a plugged-in Realtek endpoint to measure.

**What Equalizer APO does with the APO it replaces** (read in upstream
`EqualizerAPO/EqualizerAPO.cpp` at the vendored commit): install records the
replaced class as `PreMixChild`/`PostMixChild`. At `Initialize` the APO
`CoCreateInstance`s that class and passes it the same initialization data;
format negotiation is delegated to it (`IsInputFormatSupported`), and
`LockForProcess`/`UnlockForProcess` are forwarded. In `APOProcess` the child
runs first, writing the output buffer, and Equalizer APO's filters then process
that buffer in place, with the channel count taken from the child's output
format. If the child fails at any step it is released and Equalizer APO runs
alone. The user can turn the child off per device ("use original APO").
IsoAPO does none of this yet: devicetool records the child, IsoAPO ignores it.

## 2026-09-13: Child APO, removing Equalizer APO, bass management, band order

Owner's decisions:

- **IsoAPO hosts the APO it replaces, as Equalizer APO does** (previous entry).
- **Choosing IsoAPO uninstalls Equalizer APO**, so the two never conflict. Taken
  to mean the whole Equalizer APO install, not only its slots on one endpoint:
  that also removes its `Child APOs` records, which would otherwise let Equalizer
  APO's Device Selector write itself back over IsoAPO. Where Equalizer APO was
  wrapping a vendor APO, IsoAPO has to take that vendor class over as its own
  child, or the vendor processing is lost.
- **Realtek composite and offload lists are out of scope.**
- **Bass management follows AV receivers:** no slope control, Linkwitz-Riley
  24 dB/oct as the engine already does, crossover 40–250 Hz (default 80),
  LFE low-pass 80–250 Hz (default 120), 10 Hz steps. Denon's range and defaults
  per its support pages and owner forums; the fixed 24 dB/oct is the common
  receiver behaviour, not checked against a manual.
- **Band order:** the "Order" label and icon are gone; the Manual / By frequency
  control stands alone.
- **Test tone:** pink noise, as recorded above.

Screens regenerated for band order and the bass management card.

## 2026-09-13: IsoAPO hosts the APO it replaces

Ported from upstream `EqualizerAPO.cpp`:

- **Initialize** reads the child CLSID from `HKLM\SOFTWARE\IsoAPO\Child APOs\
  {endpoint}` (`PreMixChild` for the pre-mix class, `PostMixChild` for
  post-mix), creates it in-process, and passes it the same initialization data.
- **Negotiation and lifecycle:** `IsInputFormatSupported`, `LockForProcess`,
  `UnlockForProcess` and `GetLatency` go to the child.
- **Processing:** `APOProcess` runs the child first, on silent buffers too, and
  the processor on the child's output buffer, with the channel count and speaker
  mask of the output format.
- **Failure:** a child that fails creation, `Initialize`, format negotiation or
  `LockForProcess` is released and IsoAPO runs alone.
- **Two deviations from upstream.** Upstream ignores a child's `LockForProcess`
  failure and keeps calling it; IsoAPO drops it. IsoAPO also refuses a record
  naming IsoAPO itself, which would otherwise recurse inside audiodg.
- **Record location:** the self-test build of the DLL reads the record from
  `HKCU\Software\IsoAPO-selftest` instead of HKLM, so it runs unelevated. That is
  its only other difference from the shipping DLL.

**Self-test, with a test child** registered in the test process through
`CoRegisterClassObject`. The child scales audio by 0.5 and counts calls. Checked:

- **Creation and init data:** the child is created, gets the same endpoint GUID
  and class, and reports its latency through IsoAPO.
- **Negotiation and lifecycle:** it sees format negotiation (twice: the SDK base
  class's `LockForProcess` asks again), lock, unlock and every process call.
- **Output:** 1 kHz measures −18.021 dB, the child's −6.02 dB followed by the
  −12 dB seed band. The same holds with separate input and output buffers.
- **Release:** releasing IsoAPO releases the child.
- **Running alone:** IsoAPO runs alone at −12 dB when the child fails
  `Initialize`, when the recorded CLSID is unregistered, when it names IsoAPO,
  and when the value is `!VALUE` or empty.

Six deliberate bugs each failed it: child not run; input copied over the child's
output; lock not forwarded; latency not forwarded; a failed child kept; no
self-guard.

**devicetool hands over what Equalizer APO was hosting** when
`--replace-equalizerapo` removes it:

- **Slots:** a slot Equalizer APO had taken from another APO (per Equalizer
  APO's own install record) gets that APO back instead of being emptied.
- **Child:** the APO Equalizer APO's post-mix instance hosted becomes IsoAPO's
  `PostMixChild`.

No endpoint on this machine has such a record (the four that name a child
belong to devices that no longer exist), so `roundtrip` gained
`--simulate-equalizerapo-child <clsid>`. It writes that record into the dry run
only. On CABLE In 16ch with Microsoft's WM GFX APO as the simulated child:

- **After install:** SFX holds the WM GFX APO, MFX holds IsoAPO, and IsoAPO's
  `PostMixChild` is the WM GFX APO.
- **After uninstall:** both Equalizer APO classes are back exactly.

Two deliberate bugs (slot not restored, child not taken over) each failed it.
Without the flag, and without `--replace-equalizerapo`, output is unchanged.

**Pending test, Realtek (owner, when hardware is at hand).** Equalizer APO
works on this machine's Realtek outputs, by the owner's memory. There its classes
sit in SFX (`,5`) and EFX (`,7`), upstream's recommended mode for the endpoint is
`SFX_EFX`, and Realtek's own APOs are in the composite lists `,13`–`,15` and
`,19`–`,20`. devicetool's default `--mode mfx` would instead write IsoAPO to
`,6`, a slot Equalizer APO does not use there; `--mode efx` writes `,7`, where
Equalizer APO is. The test: with something plugged into the Realtek jack,
install with upstream's mode, play audio, and check `isotone-shm status` shows a
moving heartbeat and the ring carries processed audio.

**Not handled:**

- **Pre-mix child:** a pre-mix child Equalizer APO hosted in LFX is lost,
  because upstream's SFX/MFX install deletes LFX.
- **Not seen live:** none of this has run in audiodg yet.

## 2026-09-13: Code review and fixes

Four read-only reviewers covered the core, IsoAPO and the transport, the
Equalizer APO backend, and devicetool with the tools: about 60 findings, each
re-read against the code before acting. The owner asked for everything fixable
to be fixed, with two decisions: the shared region stays writable by every
local user (anyone with a login is trusted), and mute is true silence.

Every fix below has a test that fails when that fix alone is reverted, checked
by reverting each one and rebuilding, unless it is listed under "verified by
reading" at the end.

**Core** (`core/tests/test_hardening.cpp`, 15 cases, plus a 5.1 subcase in
`test_speakers.cpp`):
- Band gain is clamped to ±60 dB and preamp and trims to −120..+60 dB, and a
  non-finite sample from any filter clears that channel's filter state and
  outputs silence for the sample. A huge finite value from shared memory used
  to leave the filters NaN until the stream restarted.
- A band moved to other channels crossfades out where it was and in where it
  goes; the mask switched instantly before.
- A shape change arriving mid-crossfade waits for the fade to finish instead
  of discarding the outgoing filter.
- Smoothers and fades advance by the frames actually processed, so a host's
  buffer size no longer changes how fast parameters move.
- Preamp and crossfade weight ramp per sample, as bypass and post gain already
  did.
- A band with zero or negative width is off in the audio, as it already was in
  the drawn curve; a width-mode change snaps the width instead of sweeping
  between units.
- A dB-slope shelf steeper than 12 dB/oct stays stable at high gain (the term
  under the square root is held at the value for Q 10).
- The ring reader takes the capacity from the caller, not the shared header; a
  writer that lost its claim to another process stops writing.
- Numbers are read and written with a period whatever the C locale
  (`std::from_chars`/`to_chars`); frequencies are written so Equalizer APO's
  Room EQ Wizard rule cannot read `80.125` as 80125 Hz (a trailing zero); widths
  the importer does not accept for a filter type are exported as the Q that
  designs the same filter; mute is exported as `Copy: X=0` for every channel and
  read back as mute.
- Swap front/rear uses the side speakers on a layout with no backs, such as
  Windows' default 5.1 (0x60F); it did nothing there before.
- The random-parameter stress test's fixed peak bound of 1000 was replaced by a
  runaway bound and a check that the output returns to unity once parameters
  are flat. The wait-for-the-fade change let high-Q bands hold their shape and
  one peak reached 1781; the old code's constant restarted fades had kept it
  lower. RMS over the ten seconds was equal or lower, and nothing grew.

**IsoAPO** (self-test, 12 new checks):
- Silent buffers go through the child and the processor, zeroed first, so a
  delay tail plays out and nothing stale waits in the delay line; the output is
  flagged silent only when it is.
- Alone, a channel-count change in format negotiation is answered with a
  suggestion in the output's layout instead of accepted and then refused at
  lock; a child that declines one probed format is kept.
- A failed lock unlocks a child that had locked; `LockForProcess` catches
  exceptions; `Initialize` while locked returns `APOERR_ALREADY_INITIALIZED`
  (the SDK has no "already locked" code).
- Equalizer APO's classes are refused as a child; flush-to-zero is set on every
  call, after the child; class factories count for `DllCanUnloadNow`.
- The region is seeded before its magic is written, so no other process can
  write it during seeding; an open that finds a zero magic gives up after 50 ms
  (it waited 200 sleeps).

**Equalizer APO backend** (`compat_tests`, 7 new cases, and CLI runs):
- Attach and detach check and change `config.txt` through one handle, retrying a
  sharing violation (Equalizer APO's own loader retries forever), and refuse a
  file that is a hard link or reparse point. Atomic writes create their `.tmp`
  fresh, so a planted link is not written through.
- The default sandbox is checked before it is created; the guard no longer
  fails open when `ConfigPath` is unreadable; `loopback` refuses an output path
  inside the live install (CLI run: refused, no file created).
- An Include of `Isotone.txt` by absolute path counts as attached.
- Crossover frequencies use the Room EQ Wizard-safe format (the model-based
  test compares with Equalizer APO semantics); a mask of 0 gets the default
  layout; mute is `Copy: X=0`.
- Failed writes count toward the write coalescer's rate limit.
- `apply` reports lines it does not model and `Device:` lines it ignored, and
  takes a full device ID (CLI runs).

**devicetool** (dry-run roundtrips on every present render endpoint, 18
simulated handoffs, a throwaway HKCU key program):
- Replacing Equalizer APO restores a slot only with an APO Equalizer APO was
  actually hosting there, and makes IsoAPO host an APO only when IsoAPO took
  that APO's slot. Before, an Equalizer APO in SFX/EFX with IsoAPO in MFX put the
  EFX vendor APO back in EFX and also hosted it: twice. The simulation now
  checks each vendor APO runs exactly once (or nowhere, when it was not hosted).
- Without `--mode`, install and repair use the mode of Equalizer APO's post-mix
  slot where it is on the endpoint (MFX on the cables, EFX on the Realtek
  outputs), otherwise upstream's recommendation.
- Uninstall restores the "disable enhancements" flag and removes processing-mode
  lists install added (recorded in the install record), replaces Equalizer APO
  classes with what they replaced when Equalizer APO is no longer registered,
  and succeeds when a driver removed FxProperties.
- A real install that fails is rolled back; `--replace-equalizerapo` on an
  already-installed endpoint does the replacement (dry run on CABLE Input:
  deletes SFX); repair refuses an endpoint Equalizer APO took back and uses the
  backup directory; a dry run whose registration checks fail reports ok false.
- The dry run refuses a key creation the ACL would deny Administrators, so it
  shows upstream's take-ownership fallback; `createKey` does not log a key that
  already existed; arguments are read as UTF-16 (`wmain`).
- `install.ps1` is removed: devicetool replaces it, and its uninstall
  unregistered the DLL for every endpoint.

**Tools:** `isotone-shm` rejects a malformed endpoint (exit 2) and a capture too
large for a WAV header; `isotone-measure` escapes JSON strings, handles 24-bit
3-byte and refuses other unsupported mix formats instead of playing an
uninitialised buffer, and requires `--capture` or `--loopback`.

**Verified by reading only** (no test reaches them): rollback of a failed real
install, repair's backend refusal and backup directory, the dry-run ok false
path, the guard's registry fallback, the `LockForProcess` exception catch, the
seeding order, per-call flush-to-zero, the shm WAV size limit, the measure
format refusal, and `readValue` on a zero-length value (the old out-of-bounds
read did not crash, so a functional test passes either way).

**Not changed:** the region's DACL (owner's decision); channel names past the
layout's count, which match upstream `ChannelHelper::getChannelNames`.

Not yet run live: the rebuilt IsoAPO DLL is not staged, so the audio path
changes (silent buffers, child hosting) have only run in the self-test.

## 2026-09-13: The reviewed build measured live, reinstalled, two more fixes

The owner staged the reviewed IsoAPO DLL (`d02eddc`) and allowed the Equalizer
APO backend runs to change the live `config.txt` as long as each is reverted.
Every run detached in a `finally` and checked `config.txt` against the snapshot
before and after; it matched each time.

**Before reinstalling** (IsoAPO in MFX, Equalizer APO's pre-mix class still in
SFX):

| Check | Result |
|---|---|
| 7.1 routing and bass management, 7 cases, IsoAPO | worst 0.0017 dB, 0.015° |
| same 7 cases through Isotone.txt, plus bypass | worst 0.0002 dB, 0.002° |
| negative control, both backends | every capture matched only its own case |
| curve through the region, 14 frequencies × 8 channels vs scipy | 0.0001 dB rms |
| delay, polarity, speaker mute, both backends | exact; one Equalizer APO 0.17° miss did not repeat in 6 runs at 2 and 8 channels |
| 300 ms lip-sync tail through silent buffers | full tone length, then exact zeros |
| band moved L to R during a tone | worst step 0.05 % of peak |
| `80.1250` written for 80.125 Hz | -12.000 dB at 80.125 Hz in Equalizer APO |
| devicetool `test` on both cables; `roundtrip` on every present output | pass |
| simulated handoffs, Microsoft WM and Realtek classes, 4 outputs × hosted/unhosted/unregistered | 24 of 24; every vendor APO placed once, or nowhere when unhosted |

**Reinstall.** The owner ran, elevated, `isotone-devicetool uninstall` then
`install --replace-equalizerapo` on CABLE Input. Uninstall wrote Equalizer APO's
classes back to `,5` and `,6` and deleted IsoAPO's record; install put IsoAPO in
`,6`, removed Equalizer APO from `,5`, wrote the record and a `.reg` backup, and
passed its registration checks. `status` now reports `native` (it was
`conflict`). The owner also deleted the leftover `Isotone.txt` and
`config.txt.isotone-backup`.

**The 6.2 dB input offset is explained.** Since stage 3 the tone reached IsoAPO
6.2 dB low. With Equalizer APO's pre-mix class gone from SFX, the ring reads
-14.999 dB for a -15 dB design: that class was running `peace.txt` (-5 dB preamp,
a 100 Hz high-pass, shelves) on the render side ahead of IsoAPO. It answers the
open question in "State of the owner's machine".

**After reinstalling**, two IsoAPO bass management cases failed at 40 Hz with the
same loss on all 8 channels and exact phase: the Windows engine's limiter. With
the high-pass gone, the summed LFE reached 1.21 and 1.89 of full scale; the
losses were 1.78 and 5.63 dB against 1.68 and 5.52 dB of overshoot, the same
0.1 dB margin each time. At 8 dB lower level all cases pass (worst 0.0010 dB).
Three times afterwards "everything" lost 0.30 or 0.65 dB uniformly at one
frequency, with IsoAPO's output under full scale; it did not recur in 22 further
runs, in any run that also recorded the ring, and never in the Equalizer APO
backend. Where the ring was recorded, IsoAPO's output matched the processor to
0.000 dB. Unexplained.

**Fixes:**
- **Mute and speaker mute end on exact zero.** They approached zero exponentially
  and reached it only when samples underflowed, about 1.5 s in audiodg (the ring
  read 1e-38 to 4e-30). They now snap within 1e-6 of their target, as the
  routing and bass gains already did. New test in `test_speakers.cpp`; it fails
  without the snap (24000 nonzero samples in the second half-second).
- **The Equalizer APO backend writes the frequency the processor designs.**
  Upstream does not clamp: a band above Nyquist makes its biquad unstable and
  Equalizer APO outputs silence (measured: -170 dB at every frequency for a band
  read as 80125 Hz), where IsoAPO designs it at 0.95 of Nyquist.
  `ApoFormatOptions::sample_rate` and `DeviceConfig::sample_rate` clamp with
  `clamp_fc`; 0 leaves the value as entered, which exports keep. `isotone-compat
  apply --rate HZ`. Live: with `--rate 48000` the file says `Fc 22800 Hz` and the
  capture matches that filter to 0.001 dB at 1, 10, 16 and 20 kHz. Tests in
  `test_hardening.cpp` and `test_compat.cpp`; both fail with the clamp removed.
  A shelf's corner frequency, which the processor clamps a second time, is not
  clamped in the text; that differs only for shelves within an octave of Nyquist.

**Open, for the owner:**
- **Bass management headroom.** Summing every small speaker into the sub can
  exceed full scale on ordinary material; on IsoAPO the engine limiter then
  turns every channel down together. Auto preamp does not account for it.
- **Equalizer APO's own install record.** `--replace-equalizerapo` leaves
  `HKLM\SOFTWARE\EqualizerAPO\Child APOs\{guid}`, and devicetool's uninstall uses
  it. The 2026-09-13 decision to uninstall Equalizer APO as a whole is not
  implemented; until it is, Equalizer APO's own uninstaller could write its
  records over IsoAPO.

Not yet live: mute's snap (the staged DLL predates it).

## 2026-09-13: Headroom for speaker routing; IsoAPO takes Equalizer APO's record

Mute's snap measured live after the owner staged the DLL: a speaker-muted channel
has 0 nonzero samples in a second (17,844 before). Owner's decisions on the two
open items above: both fixed as proposed.

**Auto preamp covers routing and bass management.** `composite_peak_db` now
takes the speaker mask and models the speaker stages as the processor runs them:
routing matrix, then each small speaker's Linkwitz-Riley high-pass and the LFE
channel's sum of every small speaker's low-pass plus its own low-passed content.
Per output and frequency it adds the magnitude of every path in, the most any
full-scale inputs can reach (paths from different inputs can be brought into
phase). Muted outputs need no headroom and are skipped. `butterworth2` moved
from the processor to `biquad.h` so both use the same sections. The UI passes the
device's speaker mask. Upmix with every speaker small peaks at +21.0 dB at 40 Hz
on the LFE, so auto preamp would be -21 dB there.

Test (`test_speakers.cpp`): at 40, 100 and 1000 Hz the processor's loudest output,
with the LFE input's phase searched in 10° steps, is never above the bound and
within 0.004 dB of it. Four checks fail with the speaker terms removed.
Identical in-phase inputs are not the worst case: at 100 Hz they fall 0.78 dB
short, because the LFE's own 120 Hz low-pass and the 80 Hz crossover differ in
phase.

**Correction to the open item above.** Equalizer APO's uninstaller could not
write over IsoAPO: it runs `DeviceSelector /u`, which uninstalls only endpoints
where `DeviceAPOInfo::isInstalled` is true, meaning its classes are in the
effect slots (read in upstream `Setup.nsi`, `DeviceSelector/main.cpp`,
`DeviceAPOInfo.cpp` at the pinned commit). After a replacement they are not.
Device Selector would only do so if the user ticked the device again. The
leftover record was still wrong in two ways: IsoAPO's uninstall restored
Equalizer APO rather than the device's own APOs, and the record outlived
Equalizer APO.

**Replacing Equalizer APO takes its record over.** For each slot where IsoAPO's
record says it replaced an Equalizer APO class, IsoAPO's record now holds what
Equalizer APO's record says was there before (a vendor CLSID, or `!VALUE`), and
Equalizer APO's record for the endpoint is deleted. Uninstalling IsoAPO restores
the device as it was before either EQ, as Equalizer APO's own uninstall would.
`install --replace-equalizerapo` on an endpoint an earlier replacement left a
record for does only the takeover, which migrates CABLE Input. `status` reports
Equalizer APO's uninstaller (`equalizerapo.uninstaller`) for the UI to run once
the user has moved every device they want to IsoAPO; running it is left to the
UI and the user, not devicetool.

`roundtrip` now expects, when replacing, the pre-Equalizer APO slots after
uninstall, checks the record is gone, and runs its second cycle (driver update,
repair) from the state the first uninstall left. 32 dry runs pass: 4 present
outputs × plain, replacing, and 6 simulations (Microsoft WM and Realtek classes,
hosted, unhosted, Equalizer APO unregistered). With the takeover disabled the
replacing runs fail and the plain ones do not. Dry run on CABLE Input: its record
takes `!VALUE` for SFX and MFX and Equalizer APO's record is deleted.

## 2026-09-13: Pre-UI review

Six read-only reviewers (core DSP, core model and transport, IsoAPO, compat
backend, devicetool and tools, test suite) before stage 4. Every finding was
checked with a test that failed first, fixed, and mutation-checked: the fix
reverted, the test must fail. 90 mutations, all caught; the three that first
survived exposed weak tests, which were strengthened. Owner's decision: bypass is
EQ only.

**Core DSP**
- **Bands keep their slot by `Band::id`**, not list position. Deleting,
  inserting or reordering a band used to crossfade every band after it. Ids must
  be unique within a state.
- **Bypass turns off the bands and the preamp only.** Mute, trims, balance,
  polarity, speaker mute, routing, bass management and delay stay. Same in
  `magnitude_db`, `flat_gain_db`, `composite_peak_db` and the compat file.
- A change queued behind a crossfade starts after the fade's last sample; it
  skipped the last step.
- A NaN on one input no longer resets every routed output (0 × NaN in the
  matrix); outputs past float range are cleared, not played as inf.
- Bandwidth is clamped to an equivalent Q of 1e-4, so a wide band near Nyquist
  cannot design a2 = −1.
- The param block writer takes the seqlock with a compare-and-swap. Two writers
  used to both hold it: 126 torn blocks in 13,565 reads, and the engine then
  never re-read.

**Config format**
- **Export writes what the processor plays:** a bandwidth shelf as LSC/HSC with
  the Q for the device's rate (LS + Q was 3.6 dB off); a dB slope past the
  processor's limit at that limit (upstream takes the root of a negative number);
  bands with no width as OFF; nothing non-finite; gains and levels clamped.
- **Import reads filter lines with upstream's own patterns:** case-sensitive
  tokens, `Hz` and `dB` required, a shelf's dB slope over its Q, channel words
  split on spaces only, `#` a comment only at the start of a line, U+00A0 in Fc
  removed. Lines upstream ignores are ignored with a warning.
- Sections under `Stage:` for capture only are skipped. `Device:` lines other
  than `all` and `If:`/`Else:` branches are imported, with a warning each.
- A layout with no speaker mask names channels by the default mask.
- `to_param_block` returns false when bands past 64 were dropped.
- A ring claim from another process waits for the owner's write index to stand
  still for 50 claims. Two live hosts took it from each other on every call.

**IsoAPO**
- **Saved state per endpoint:** `%ProgramData%\IsoAPO\devices\{guid}.bin`, a
  `ParamBlock` as bytes (`windows/transport/persisted_state.h`). A file that
  exists is the device's state, flat included. No file: flat. An invalid file:
  flat. Read before the region is created, so sibling instances never wait on
  disk. `config.txt` is no longer read. The self-test build keeps the -12 dB
  default. `isotone-shm persist` and `forget` write and delete it.
- A region Initialize could not create is created at LockForProcess.
- An IsoAPO initialized inside its own child's creation refuses. A wrapper APO
  whose record names IsoAPO would otherwise recurse until audiodg's stack
  overflowed.
- A child that fails its lock is dropped only when IsoAPO can process the format
  alone; otherwise the lock fails and the child is kept for the next attempt.
- The output flags are set before the child runs; the max frame count falls back
  to the output's; `Reset` clears the processor and reaches the child; a
  discovery-only instance does no work; a second lock is refused before the
  child is touched.

**Compat backend**
- **Routing and output sections address channels by number inside
  `If: outputChannelCount == N`.** Equalizer APO applies Isotone.txt to whatever
  format the device has when it loads. A Copy source naming a channel the
  layout lacks is added as a constant: a block written for 7.1 put DC of 1.0 on
  a stereo device, and 3.0 on the LFE of a 2.1 device. Tested on every pair of 8
  layouts through the upstream model.
- **Bands are guarded by `If: sampleRate >= X`**, the lowest rate at which every
  written frequency is below the processor's clamp. At a lower rate upstream
  designs the band unstable.
- **Mute is `Preamp: -1000 dB`.** Upstream stores the gain as a float, so it is
  exactly 0 on any layout; the Copy form left channels a grown layout added
  playing.
- **Bypass comments out the preamp and bands, and ends at `# Isotone: end`.**
  Files written before, with everything commented to the end, still read.
- A second block for a device is removed on update and reported on read; a full
  device ID names the endpoint by its GUID; a routing section without its end
  marker no longer swallows the curve.
- An include of Isotone.txt under a narrower `Device:` or inside `If:` is not
  attached, and attach refuses rather than include it twice.
- Each write re-reads Isotone.txt and keeps another writer's blocks; a pending
  live edit is written when the writer goes away.
- The install guard refuses everything when a root exists but reports no file ID.
- `isotone-compat apply` requires `--channels` and `--rate`.

**devicetool and tools**
- Replacing Equalizer APO takes its originals for the slots its install deleted.
- Uninstall on a detached endpoint leaves the driver's mode lists alone; the
  install mode is recorded and repair reuses it; a `--mode` that would delete a
  slot where Equalizer APO hosts an APO is refused; uninstall works on a device
  that is no longer present.
- **Rollback** covers FxProperties' creation, both install records and their
  parent keys, and runs on the replacement path, uninstall and repair, not only
  install. `roundtrip` checks it: 27 dry runs on 6 present outputs pass, and 4
  rollback mutations fail them. Not covered: upstream's ownership and ACL change
  on an endpoint key with no FxProperties.
- Dry runs route DWORD and type reads through the hook, nest, keep a deleted and
  recreated key empty, and print their operations on failure.
- `roundtrip --simulate-equalizerapo` refuses an endpoint without Equalizer APO,
  where the simulation meant nothing.
- `isotone-measure` keeps 50 ms queued instead of 200 ms, retakes a window with a
  capture discontinuity or render underrun (up to 3 times), and reports glitches
  and each channel's residual after the fitted sine. On the cable: 0 glitches,
  residual -106 dB. This may explain the 0.3 to 0.65 dB drops seen earlier;
  not yet confirmed.
- Loopback and ring captures report dropped frames and discontinuities, and are
  `complete` only without them. JSON never carries nan.
- `gen_reference.py`: the Peace cases were mislabelled (a 0.9 dB slope, not
  upstream's no-width default). The real no-width cases and LS/HS with a Q and
  the corner shift are added; the core matches scipy on all 26.

**Not changed, noted:** the backups directory is writable by standard users (the
trust decision covers it); the `unregistered` roundtrip simulation passes by
construction since the record takeover; `equalizerapo_slots_removed` does not
count slots upstream's install deleted.

**Tests:** `core_tests` 177 cases, `compat_tests` 35, APO self test and the shm
transport check pass on MSVC; `core_tests` passes on GCC 16.1 with no warnings.

**Measured live** after the owner staged the reviewed DLL (hash-identical to
the build of `4df7343`):
- **Saved state in audiodg:** with a -6 dB band saved for CABLE Input, the region
  IsoAPO created held that band and the ring read -6.000 dB at 1 kHz; with the
  file deleted, 0 bands and 0.000 dB. LocalService reads the file a user wrote.
- **IsoAPO at 7.1:** every speaker case within 0.001 dB and 0.002° of the core;
  silent-buffer tail, exact-zero speaker mute and the band move all pass.
- **Compat on CABLE Output's Equalizer APO, 7.1, 48 kHz:** the speaker cases
  within 0.0002 dB; bypass keeps the speaker setup and drops the band; the band
  reads -6.000 dB on every channel when not bypassed; a block written for 2
  channels skips its routing on the 8-channel device; bands written for 96 kHz
  carry `If: sampleRate >= 84211` and do nothing at 48 kHz, the same band written
  for 48 kHz plays; mute is exact zeros (the tool's -200 dB floor) on every
  channel. `config.txt` matched the snapshot before and after, and Isotone.txt
  was put back byte for byte.
- **The unexplained level drops found.** One run read 0.212 dB low at 160 Hz on
  all 8 channels, with exact phase. That window had no capture discontinuity
  and no render underrun, but a residual after the fitted sine of -7 dB, against
  -99 dB in every clean window: a splice WASAPI does not flag. `isotone-measure`
  now also retakes a window whose residual is above -40 dB re the fitted sine on
  any channel carrying the tone. The rerun passed, with no glitch to retake.
- **Not measured:** whether writing `Isotone.txt.tmp` makes Equalizer APO reload
  twice. Its trace log needs an HKLM setting. (Measured in the next entry: it
  does.)

## 2026-09-13: Final backend review before stage 4

Seven read-only reviewers: core DSP, config format and transport, IsoAPO,
compat backend, devicetool, measurement tools and CI, and the seam the UI will
use. The lead re-read every high and medium finding against the code before
acting (two claims were partly wrong and are corrected below). Owner: fix
everything that needs no decision, decide the rest, and "do whatever you need
to get the best possible tests" for this session, live runs included. Every fix
below has a test that fails without it and was mutation-checked (fix reverted,
test fails, fix restored), unless listed as verified by reading.

### Core
- **A band's channel change no longer restarts it where it already played.**
  Widening a 40 Hz Q 10 cut from L to L+R lifted L by 9 dB for 125 ms: the
  crossfade cleared filter state on every channel. Channels in both masks keep
  their state and play the new filter alone.
- **The parser cannot throw.** A Filter line with about 600 spaces threw
  `regex_error(error_complexity)` on MSVC 14.51 (upstream's older `<regex>`
  resets its count at each position and does not); on GCC 50,000 spaces took
  77 s. Whitespace runs are collapsed before matching (upstream's patterns only
  use `\s+`/`\s*`, so no result changes) and a line whose match still throws is
  skipped with a warning, as upstream's loader does. A Filter line longer than
  1024 characters after that is skipped before matching: on the CI runner's
  GCC 13.3, libstdc++ matched a million-digit Fc recursively, one frame per
  character, and crashed with SIGSEGV instead of throwing (the first push of
  this review failed `core (ubuntu-latest)` on it; Windows builds had passed).
- **Import reports everything it does not apply.** GraphicEQ, Include, Delay,
  Copy and unknown lines were in `unsupported` only, so an AutoEq GraphicEQ file
  imported as flat with nothing reported. `Filter N: OFF` with a whole filter is
  a disabled band, so export and import keep disabled bands.
- **Band width is clamped** (Q 1e-4 to 1000, shelves Q 10, equivalent limits
  for bandwidth and slope). A low-pass at Q 1e6 played +120 dB past the gain
  limit; at Q 1e17 it was unstable.
- A new band with a non-finite value plays what the curve draws instead of the
  previous band in its slot. The curve applies the processor's clamps.
  `composite_peak_db` designs each band once (6.0 to 3.0 ms for 64 bands, 512
  points, 8 channels). The parser bounds a channel count at 65535.
- `Copy:` is read as mute exactly where upstream reads every source as zero.
  The reviewer said `L=+0` is DC; it is mute (upstream's split on `+` drops the
  empty term). `L=-0`, `L=00` and a trailing tab are DC.
- Weak tests replaced: the seqlock contention test checks 1000 reads taken
  while the writer runs (the old one passed a broken seqlock 2 of 10 times); the
  stage order test observes routing; the locale test asserts its probe and
  restores the locale on every path.

### IsoAPO and transport
- **A child APO that refuses a format is dropped and IsoAPO runs alone**, as
  upstream `EqualizerAPO.cpp:253-277` does. This reverses the earlier rule that a
  child declining one probe is kept, which upstream does not have. Two
  deviations: a child locked is unlocked before release, and a child is kept
  while IsoAPO is locked.
- **One endpoint identifier normaliser** (`canonical_endpoint_guid`): the region
  name and the saved-state path accept a braced GUID, a bare GUID or the full
  device ID. A bare GUID or `IMMDevice::GetId` string named a region that never
  existed.
- `write_persisted_state` stamps the header, so a block without
  `init_param_block` no longer writes a file IsoAPO rejects.
- A buffer longer than the negotiated maximum is processed in pieces; it passed
  through unprocessed. The speaker mask comes from the output format, as
  upstream's. A second unlock does not reach the child.
- The header's format is published only by the instance that owns the ring, so
  a 16 kHz communications stream cannot relabel a 48 kHz media spectrum.
- Ring tokens carry a per-process nonce instead of the process id, so a claim
  left by a crashed audiodg whose id is reused is taken over.
- The ring pages are touched at lock: a full lap took 64 page faults on the audio
  thread before, 0 after. `VirtualLock` was not used: the default minimum
  working set (50 pages) is smaller than the ring.
- `isotone-shm` takes `--channels`/`--mask` when no engine is running (it
  assumed 7.1) and reads UTF-16 arguments.
- Correction: `isoapo.cpp` and the stage 1b notes said upstream accepts
  `APOInitSystemEffects2`; upstream accepts only `APOInitSystemEffects`
  (`EqualizerAPO.cpp:106`). The self-test build differs from the shipped DLL in
  four ways, not one: namespace, child record hive, saved-state directory, and
  the -12 dB cold-start band.

### Compat backend
- **Concurrent writers keep each other's blocks.** Every writer used the same
  `Isotone.txt.tmp` and nothing locked read, merge and replace: two writers
  failed 598 of 600 rounds and silently lost a block in 2. The pre-UI review's
  claim that each write keeps another writer's blocks was not true. Writers now
  share `Isotone.txt.lock` (share mode 0; Equalizer APO's installer grants Users
  full access to the directory) and use per-process temp names.
- A write is skipped only when the disk already holds it, not when this writer
  last sent the same text.
- `Stage:` and an unclosed `If:` at the end of `config.txt` are read as
  upstream reads them; attach appends `EndIf:` and `Stage: post-mix capture`
  where needed, and refuses an include that only some instances reach.
- The preamp is outside the `If: sampleRate >= X` band guard: at a lower rate
  the -21 dB auto preamp vanished while upmix and the LFE sum still played.
- A permanent access denial fails at once instead of retrying 200 ms per write.
  `DeviceConfig` without a channel count or rate is refused. `isotone-compat`
  reads UTF-16 arguments. The first packet's discontinuity flag in loopback
  capture is not a glitch. `LoopbackCapture::start` has a timeout and reads are
  safe during `stop` (the read race has no failing test; verified by reading).
- The upstream model in `compat_tests` is ported from upstream's own filter
  parsing, BiQuad design and channel naming, independent of `core/`; a 5% error
  in core's peaking design and swapped SL/SR names now fail compat tests.

### devicetool
- **Replaced, then re-ticked in Equalizer APO's Device Selector**: install and
  repair each sent the user to the other while both EQs ran, and the only exit
  lost the vendor APO. `status` names the state (`replaced_by_equalizerapo`,
  `alongside_equalizerapo`) with `remedies`, and
  `install --replace-equalizerapo` takes the endpoint back from Equalizer APO's
  record. Uninstall from `alongside_equalizerapo` keeps Equalizer APO working.
- A vendor APO Equalizer APO hosted through upstream's GFX fallback is hosted.
- The install checks require LOCAL SERVICE read and execute on the DLL.
- **A journal** (`HKLM\SOFTWARE\IsoAPO\Pending\{guid}`) is written before the
  first change and cleared last; a killed run reads as `interrupted` and the
  next command undoes it. Kill checks at every write of install (22), uninstall
  (9), repair (23) and take-back (9).
- The dry run's ACL check tests `KEY_CREATE_SUB_KEY` only; it granted creation
  to any read ACE. A record without an install mode takes the mode from the
  record, the slot, or Equalizer APO's record, and repair otherwise refuses
  without `--mode` (it would have moved CABLE Input from MFX to EFX). Uninstall
  of a detached endpoint leaves a driver's slots alone. `rolled_back` is true
  only when every rollback step succeeded.
- **`--output <file>`** for elevated runs (created new, no reparse points).
  Exit codes 0 ok, 1 refused, 2 bad arguments, 3 not elevated, 4 busy.
  Inapplicable flags and junk around a GUID are refused. Mutating runs are
  serialised by a mutex in a private namespace bounded by the Administrators SID
  (a `Global\` name could be squatted by any user).
- Multi-string reads go through the dry-run hook; upstream handle leaks and
  `delete` on `new[]` fixed (marked in VENDORED.md; verified by reading, only
  elevated runs reach them).

### Measurement tools and CI
- **A dead stream no longer reads as perfect silence.** A failed WASAPI call
  returned quietly: -200 dB on every channel, 0 glitches, exit 0. Every stream
  error is counted, each window records frames rendered and captured, and a
  short or errored window is retaken and then fails the run (exit 3). The
  earlier "exact zeros" mute results were taken with the old tool, so they do
  not rule out a dead stream; the live runs below repeat the silence checks
  with frame counts.
- Render and capture formats are recorded and a channel-count mismatch is
  refused. Phase is null when its reference carries no tone; `--phase-ref`
  chooses the reference. An ambiguous endpoint fragment is refused. UTF-8 JSON.
  The analysis moved to `measure.cpp` with `measure_tests` (fake WASAPI clients).
- `gen_reference.py --check` fails on a case missing from either side.
- **CI builds with warnings as errors** (`ISOTONE_WARNINGS_AS_ERRORS`); vendored
  code and SDK headers are exempt. A fresh MSVC configure was Debug (CMake sets
  the type inside `project()`), and a Debug `IsoAPO.dll` imports runtime DLLs no
  redistributable installs; the default is now set before `project()`.
- Packaging note (stage 6): `IsoAPO.dll` imports MSVCP140 and VCRUNTIME140 built
  with toolset 14.51, and Microsoft requires a redistributable at least as new
  as the newest build tools used. Equalizer APO's bundled 14.40 runtime does not
  satisfy that; ship a new enough redistributable or link the CRT statically.

### New: `windows/devices` (owner's decision 9)
`isotone_devices`: render endpoints with their full ID, GUID, names, state and
default roles; the current format from `PKEY_AudioEngine_DeviceFormat`; the
engine per endpoint, classified exactly as `devicetool status` does
(`isoapo.state`, `backend`); an `IMMNotificationClient` wrapper whose `close`
guarantees no callback after it returns; and `probe_engine`, which tells
running, idle, not installed and stalled apart from the region's heartbeat and
active audio sessions. `devices_tests` compares every render endpoint with
devicetool's JSON and skips visibly on a machine with no audio service.

### Owner's decisions
1. **The curve is each output's own path**: `magnitude_db`/`phase_deg` take the
   stream's layout and draw speaker mute, the small speaker's high-pass and the
   LFE low-pass; cross-feeds are not drawn. Matched against the processor within
   0.001 dB and 0.01°.
2. **Auto preamp only cuts**: `auto_preamp_db` = min(0, -`composite_peak_db`).
3. **Per-channel values follow the speaker across layout changes.** Param block
   v5 records the layout the state was written for; `remap_channels` moves band
   channels, trims, delays, polarity, speaker mute and small speakers by speaker
   role the way Equalizer APO resolves channel names (SL and RL stand in for each
   other on a layout with one pair; values landing on one channel combine; a band
   left with no channel is disabled). IsoAPO remaps at lock and on every block;
   the curve functions remap; the compat backend writes delay, polarity and
   speaker mute by name (a `Copy:` whose target and source both name a missing
   channel writes a virtual channel, so no DC), with routing still guarded by
   channel count. A v4 saved file reads as flat; no saved files existed. From
   the first release a reader must migrate older versions.
4. **Compat devices hear an edit when it is committed.** Measured on the cable
   pair (CABLE Output's Equalizer APO): every change Equalizer APO notices in its
   config directory (a write, a file created or deleted) rebuilds every Equalizer
   APO device's filters from rest with a 10 ms crossfade.

   | Case | Measured |
   |---|---|
   | no delay, 997 Hz | no dip over 0.05 dB |
   | lip sync 100 ms | 90 to 110 ms of exact zeros per write, all channels |
   | speaker delay 50 ms on one channel | that channel only, 40 ms |
   | 40 Hz Q 10 -12 dB band | +10.5 dB, over 1 dB for 130 ms per write |
   | 100 Hz Q 10 / Q 2 | +8.9 dB for 50 ms / +3.2 dB for 10 ms |
   | 30 writes/s with delay or a bass band | silent, or never settles, for the whole drag |
   | temp file inside the config directory | two reloads per write |
   | temp moved in from outside, or in-place | one reload |

   The owner's `peace.txt` (every device) also restarts its 100 Hz high-pass on
   each reload (+5 dB for 7 ms). Owner chose "write less often"; the numbers
   rule that out, so: `CompatWriter::apply` writes nothing, `persist` (on
   release) writes once, and the time-based coalescer is gone. Temp files go to
   `%LOCALAPPDATA%\Isotone\compat-tmp` when it is on the config directory's
   volume, with the replaced file's DACL (auto-inherit kept) or the DACL a new
   file in the directory would get. Live: one reload per write, content applied
   (-5.992 dB for a -6 dB band), config directory restored byte for byte and
   SDDL for SDDL.
5. **A muted speaker contributes nothing anywhere**, including the bass that bass
   management sends from it to the LFE (processor, `composite_peak_db` and the
   compat routing lines).
6. **Solo is never saved**, and soloing a small speaker keeps the LFE (UI rule).
7. **Test tones** play with bypass on and swaps and upmix off (UI rule).
8. **`IAudioSystemEffects2/3` deferred.** Rule: only if other software does it.
   No APO host found implements or forwards them: upstream Equalizer APO 1.4.2
   (SourceForge HEAD, full history), ViPER4Windows, UniteFx, the Equalizer APO
   forks, Microsoft's samples (none host a child). Realtek's and Windows' CAPX
   APOs on this machine expose all three interfaces but sit in the composite
   lists `,13`-`,20`, which Windows chains itself; Equalizer APO's records here
   host no vendor APO.
9. **`windows/devices`**, above.
10. Standard users can set endpoint effect slots (`BUILTIN\Users` has SetValue
    on MMDevices): fine, by the earlier trust decision.

**Not changed, noted:** compat routing on the same channel count with a
different mask can address different speakers (IsoAPO resolves routing by role);
a band the remap disables keeps its old mask; a render stream that stalls without an error is not detected by
`isotone-measure`; `compat_tests` creates `%LOCALAPPDATA%\Isotone\compat-tmp`.

**Measured live, compat backend and measurement tool** (CABLE Input to CABLE
Output's Equalizer APO, 7.1, 48 kHz, reviewed binaries; `peace.txt` scoped away
from CABLE Output for the run; expected values from `isotone::Processor` run on
the same state and analysed by `isotone-measure`'s own analysis code):

| Case | Result |
|---|---|
| flat block, 31 frequencies | within 0.0004 dB; frames captured and 0 stream errors on every window |
| block for 96 kHz on the 48 kHz device | preamp applies (-9 dB), band above 24 kHz does not; 0.00005 dB |
| state for 5.1 written to the 7.1 device | SL band and trim on SL, SR inverted, C exact zeros; 0.00005 dB, 0.000° |
| muted small FL with bass management, 40 and 60 Hz | FL and LFE exact zeros with frames captured; unmuted, LFE within 0.00004 dB |
| speaker mute on channel 0 | phase reference moves to channel 1; channel 0's phase is null |
| writes every 100 ms during a window | retaken 3 times, `failed_windows` names it, exit 3; clean afterwards |
| attach after `Stage: pre-mix`, `If: 1 == 1`, `If: 1 == 0` | a plain include is hidden where upstream says; attach's block applies (-6 dB) in all three; detach restores byte for byte |
| `CompatWriter::apply` 20 times, then `persist` | 0 directory events and 0 dips for the applies, one dip at the persist |
| negative control | each of 12 captures matched only its own state (or a physically identical one) |

The config directory (72 entries) matched the snapshot by hash and SDDL after
every batch.

**Found in the live run, fixed afterwards** (each with a test that failed
first and a mutation check):
- `isotone-compat apply --text-channels N [--text-mask M]` gives the input's
  layout separately from the device's.
- Attach adds no blank line after a file that ends in a newline; when the last
  line has no line break, the block starts with one and its comment says so, so
  detach restores byte for byte. A block written by the earlier build still
  detaches but leaves its blank line.
- Attach closes each open `If:` and resets each `Stage:` change under the
  `Device:` pattern it was opened under, so no device sees an `EndIf:` without
  an `If:` (the upstream model counts them). Ambiguous cases keep the If counted:
  at worst one extra `EndIf:` that upstream logs and ignores, never a hidden
  include. A `Stage:` inside an `If:` is reset on every device its pattern
  matches.
- `isotone-measure` lists every discarded attempt per frequency with its reasons,
  glitch parts, stream errors, frames and worst residual (`discarded_attempts`).
- **A layout not given is stereo.** `ChannelLayout{}` is unspecified:
  `parse_apo_config` reads it as stereo, `format_apo_config` writes for the given
  layout (remapping the state), else the state's own, else stereo, and
  `isotone-compat show` reads for stereo without `--channels`. It was 7.1.
- A Filter line longer than 1024 characters is skipped before matching (above).

**Also found, not Isotone:** a single stale frame in cable captures, from
VB-Cable; see the IsoAPO live results below.

**Tests:** `core_tests` 202 cases (MSVC 19.51 and GCC 16.1, warnings as errors),
`compat_tests`, `measure_tests`, `devices_tests`, the APO self test, the
transport check and the reference check all pass.

**Measured live, IsoAPO** after the owner staged this review's DLL (SHA-256
EE9AECEE…8567): CABLE Input to CABLE Output, 7.1, 48 kHz, `peace.txt` scoped
away from CABLE Output; expected values from `isotone::Processor` on the same
state, remapped from its parse layout; the ring capture as a second view of
IsoAPO's own output.

| Case | Result |
|---|---|
| cold start | region v5, 48000 / 8 / 0x63F, running, heartbeat moving, flat within 0.0001 dB; `probe_engine` running while streaming, idle 1.6 s after it stops |
| saved state read by audiodg | a persisted -6 dB band plays -6.0000 dB; after `forget`, flat |
| 5.1 state saved, played on 7.1 (remap at lock) | every value on its speaker role: 0.00008 dB, 0.0003°; C exact zeros; ring against core 1e-6 dB |
| another 5.1 state written while playing | 0.00012 dB, 0.0012°; ring against core exact |
| muted small FL with bass management | FL and LFE exact zeros in ring and capture; unmuted, LFE within 0.00008 dB |
| 7 routing and bass management cases, delay, polarity, trims | worst 0.0003 dB, 0.003°; ring against core exact; each capture matched only its own case |
| phase reference and a window during writes every 100 ms | reference moves, channel 0's phase null; exit 3 with `failed_windows`, clean afterwards |
| 300 ms lip sync tail, global mute | tail plays out, then exact zeros; mute exact zeros on 8 channels with 37,000 frames per window |
| -12 dB 40 Hz Q 10 band on L widened to L+R while playing | L's peak constant within 0.0000 dB (ring), 0.0005 dB (capture); before the fix +9 dB |
| a second stream and a 16 kHz communications stream beside the first | header format and ring writer unchanged, ring written |

The capture side of the cable adds about 18 LSB rms of noise that the ring does
not carry, so cells below about -60 dBFS (LFE at 1 kHz under bass management)
missed 0.001 dB on the capture by up to 3.6 LSB; the ring matched the core
exactly at each of them. A true communications-mode instance cannot be forced:
CABLE Input's mode lists hold only the default mode.

**A stale frame in cable captures, from VB-Cable** (investigated afterwards,
about 400 live trials). VB-Cable keeps audio rendered into CABLE Input while no
capture is open on CABLE Output, and when a capture and a render stream next run
together it plays back one frame of it, once: 20 to 35 ms after the capture
starts when the render stream was already running, 37 to 54 ms after the render
starts when the capture was first (120 to 131 ms with exclusive capture).

| Condition | Stale frame |
|---|---|
| a render stream running at capture start, silent-flagged or zero-valued | 189 of 190 |
| no render stream at capture start | 0 of 84 |
| exclusive render and exclusive capture (no APO on either side, no region) | 20 of 20 |
| 150 ms or more of silence rendered before the earlier audio was replaced | 0 of 50 |

The silent flag, IsoAPO and Equalizer APO play no part: it happens with no APO
in the path, IsoAPO's ring is zero at that moment, and on the capture side
Equalizer APO only filters the frame. The earlier run's 1 in 4 came from its own
sequencing (a holder stream outliving the capture). `isotone-measure` settles
before every window, so the frame never reaches one at its 0.3 s default;
`--settle` below 0.1 s is now refused (test, mutation-checked). A cable test
expecting exact zeros discards 60 ms after the later of the two starts (140 ms
with exclusive capture), or renders 150 ms of silence before opening the
capture.

The Equalizer APO config dir matched its snapshot after every batch;
`%ProgramData%\IsoAPO\devices` is empty; no region is held.

## 2026-09-14: Stage 4 prototype, owner's decisions

The owner reviewed an interactive prototype of every screen and state (published
as an artifact from `docs/design`-derived tokens; the 17 boards stay the look).
`ui-spec.md` is updated where a screen changed.

**Screens**
- The first-run output table is also Settings, Outputs, for when outputs change.
- EQ by ear: the third mark shows its value like the others; a separate Add band
  button creates the band. The boards had it created on the third mark.
- The band popover is centred on its band, from the column or the handle.
- No scale labels under the balance slider.
- One typeface, Instrument Sans, everywhere: config lines, skipped import lines,
  hex values and error reasons. A failure's reason is a plain sentence, with no
  "reason:" label.
- The curve does not include the preamp. The spectrum has its own dBFS scale and
  must not clip at the top of the graph.
- Preamp: a slider and a typed value. Auto is a mode that follows every edit,
  not a one-off button; moving or typing the preamp turns it off. The slider's
  −24 to +6 dB range is the prototype's choice.
- Protected audio "Disabled" in amber stays.
- The sidebar and tray list only working outputs; orange, red and grey ones are
  in Devices. Devices has no per-output auto-switch toggle (General has it).
- With the EQ off, the handles stay where they are, faded.
- EQ by ear has no speaker picker on surround outputs.
- Replacing Equalizer APO: the two options keep their fact lines; IsoAPO is
  preselected and marked Recommended.
- A preset assigned to several outputs changes on all of them when edited.
- Closing to the tray with unsaved changes asks to save. Don't save, there or
  when switching presets, puts the output back to its saved preset.
- The minimum window size is mocked at 1120 × 760 (sidebar and panel collapsed).

**Devices and install**
- The Speakers view's layout picker changes the Windows speaker setup, offering
  only layouts the output supports (the recommendation: an Isotone-only layout
  would disagree with the channels Windows sends).
- After an install, repair or uninstall the UI restarts the audio service and
  tests the outputs, as Equalizer APO's Device Selector does: read in upstream
  `DeviceSelector/DeviceTestThread.cpp:49` (restart, then a test per device,
  retrying other install modes with another restart) and `DeviceSelector.cpp:228`
  (a Windows restart is offered only when the service restart failed).
- Windows approval is asked once, when a change is first made (Settings, Outputs;
  first run; or a Devices action), and covers every change until the app exits.
  Per-output actions stay in Devices.
- Audio enhancements turned off on an output (FxProperties
  `{1da5d803-...},5`) are turned back on, as Equalizer APO's install does
  (`DeviceAPOInfo.cpp:607`, "force-enable enhancements") and its Device Selector
  offers for an installed device ("Audio enhancements will be enabled").
  devicetool's install does it already; `repair` skips an endpoint IsoAPO is in
  (`plan_repair`), so an installed output with enhancements off has no command.

**Backend work these need:** built the same day (next entry); the live tests
need the owner.

## 2026-09-14: Backend for the prototype's decisions

Built in two parallel worktrees, merged, then reviewed and extended by the lead.
None of it was run for real: every registry, service and format change here
was a dry run or a check.

- **`restart-audio [--dry-run]`** (elevated, machine lock) calls upstream's
  `ServiceHelper::restartService(L"AudioSrv")`, vendored unmodified with
  `PrecisionTimer.h`; its quirks are listed in VENDORED.md (one 30 s timer for
  stop and start, a service found starting is not stopped). The dry run reads
  the service and the running services that depend on it.
- **`enable-enhancements <endpoint> [--dry-run]`** (elevated, machine lock)
  deletes FxProperties `{1da5d803-...},5` when present, as upstream's install
  does. Not journaled (one delete), but it undoes an interrupted command's
  journal first. The install record is left alone: it holds the flag as it was
  before IsoAPO, which uninstall restores. `status` offers it as a remedy when
  IsoAPO is in a slot and the flag is non-zero (not while interrupted), and
  `roundtrip` checks it with the flag set, clear and absent.
- **`serve --pipe <name> --parent <pid>`** and **`DevicetoolSession`**: the app
  creates the pipe (first instance, one instance, no remote clients; the user,
  SYSTEM and Administrators), launches serve with ShellExecuteEx "runas", and
  checks the process that connected is the one it launched. Serve opens the
  pipe with identification-only security, refuses a pipe whose server is not
  `--parent`, and runs each request (fields separated by U+001F) as a child of
  its own exe, read through an anonymous pipe, so every check, journal, lock
  and exit code is the command's own. A temp file in the user's folder was
  rejected: an unelevated process could swap it between the child writing it
  and serve reading it. Refused in a request: serve, roundtrip, `--output`.
  Administrators is in the DACL for serve running as another account (an
  administrator's password typed at a standard user's prompt); an unelevated
  token holds that group deny-only.
- **Speaker layout** (`windows/devices/speaker_layout.h`, the library's one
  write): the four layouts (masks checked against ksmedia.h and the core),
  `supported_speaker_layouts` (exclusive-mode `IsFormatSupported` at the
  device's rate and depth, after checking the current format passes, so a
  check that says nothing is an error, not an empty list),
  `check_speaker_layout` and `set_speaker_layout` over the undocumented
  `IPolicyConfig` (IID `{f8679f50-...}`, vtable from six public sources, listed
  in the header). The set refuses unless `GetDeviceFormat` matches
  `PKEY_AudioEngine_DeviceFormat` first, and compares the device and mix
  formats read back field by field. `PKEY_AudioEndpoint_PhysicalSpeakers` is not
  set: it is absent on every active render endpoint here, and upstream reads it
  only for a format without a mask. devicetool's **`layouts <endpoint>`** and
  **`set-layout <endpoint> --layout stereo|2.1|5.1|7.1 [--dry-run]`** wrap it;
  the dry run is the check.

Read-only results on this machine: CABLE Input and CABLE In 16ch support all
four layouts, Steam Streaming stereo, 5.1 and 7.1, the two monitors and the
Anker stereo only; on all 7 active endpoints `IPolicyConfig`'s reads matched the
property and `IAudioClient::GetMixFormat`, and the format builder reproduced
each endpoint's stored device and mix format field for field.

**Tests:** `devicetool_tests` (new, 14 cases: the session against direct runs
byte for byte, argument quoting, refusals, a wrong `--parent`, serve exiting
when the app does, enable-enhancements dry runs on every render endpoint,
layouts and every layout's set-layout dry run agreeing on every endpoint),
`devices_tests` (format builder, supported layouts read-only). The test helpers
refuse any changing command without `--dry-run`, since CI runners are elevated.
Mutation-checked: 10 mutations of serve, enable-enhancements, restart-audio
and the remedy, 11 of the format builder, 4 of the layout commands (two of
which passed a first version of the test, which was then tightened). ctest 7
of 7, the APO self test, transport and reference checks pass.

**Run live by the owner the same evening** (results in `%USERPROFILE%\isotone-live`):
`layouts` on CABLE Input listed all four layouts; `set-layout 7.1`, from a
window the owner opened unelevated, returned S_OK with the device format, mix
format and property all 8 channels, mask `0x63F`, and `status` agreed; `set-layout
stereo` put back 2 channels, 24-bit, 48 kHz, mask `0x3`, float32 mix, and `status`
agreed; `restart-audio` from an administrator window restarted AudioSrv in 0.34 s,
running before and after. So `SetDeviceFormat` needs no elevation (as far as the
owner's window was unelevated, which the tool does not record).

**Still not verified:** the approval prompt and its decline; a real
`enable-enhancements` (CABLE Input's flag is absent, so it would change nothing
unless enhancements are first turned off in Sound settings); whether open streams
are invalidated by `set-layout`. **Found, not investigated:**
`roundtrip` on the Headset endpoint `{136126fa-...}` fails with "the prepared
install hosts the APO in its slot", with the committed build as well.

## 2026-09-14: Leftovers of the final backend review

A spot check of the review (a fresh build, every test, and mutation checks of
eight of its fixes) found no wrong fix, and these gaps. Each fix below has a test
that fails without it, mutation-checked (fix reverted or broken, test fails, fix
restored).

- **Narrowing a band's channels had no test.** Only widening was tested. The code
  was right; `narrowing a band's channels leaves it untouched where it stays`
  guards it. Keeping filter state only when the new mask is a superset of the old
  fails it with a 0.646 difference on the left channel.
- **A Gain or width too large for a double imported as 0.** `from_chars` calls it
  out of range and `number_or_zero` read 0: a 400-digit gain was a 0 dB band, with
  no warning. Upstream's `wcstod` reads infinity, which designs no usable filter
  (`BiQuadFilterFactory.cpp:121`, `:138`). The line is now skipped with a warning.
  A value too small still reads as 0, as `wcstod` reads it, and a too-large width
  that a later width replaces is not the one used.
- **Export reordered bands.** `format_apo_config` wrote one group per channel
  mask, so bands on different channels came back in another order and the manual
  order was lost. It now writes the bands in order with a `Channel:` line wherever
  the channels change. Cascaded filters sound the same either way.
- **Two transport fixes were only in the APO self test**: every form of an
  endpoint GUID naming the same region, and the saved file's header being stamped.
  `transport_tests` (new, in ctest) checks both without IsoAPO or the registry;
  naming the region from the raw string, or removing the stamp, fails it.
- **The parser's `catch (regex_error)` is still unreached.** 47 hostile Filter
  lines under the length limit (30 in the review's check, 17 more on 2026-09-14:
  long digit, no-break space and `e` runs without their unit, repeated Fc, Gain,
  Q and BW fields, long type tokens) made MSVC's regex throw none. The catch stays,
  as upstream's loader has one; it has no test.

**Tests:** `core_tests` 205 cases on MSVC 19.51 and GCC 16.1 (warnings as errors),
ctest 6 of 6 with `transport_tests`, the APO self test, the transport check and
the reference check pass.

## 2026-09-14: Stage 4 begins: the Equalizer view, connected

Qt 6.11.2 (MSVC 2022 kit; 6.12 LTS ships 2026-09-30 and is a rebuild away), built
with `-DISOTONE_BUILD_UI=ON`, off by default so CI is unchanged. Layout under
`ui/`: `backend/` without Qt (spectrum analysis, the state an output gets,
`DeviceLink`), `src/` (the band model `EqSession`, `Outputs`, `ResponseGraph`),
`qml/`, `fonts/` (Instrument Sans 400/500/600, OFL, from the project's GitHub).
Qt's headers raise C4702 under `/WX`; that warning is off for the UI targets.

**The Main board** renders at 1440 x 900 matching `Main.png` (same curve, bells,
handles, readout at 3.2 kHz -1.7 dB from the core), with the owner's changes. The
graph draws with QPainter from `magnitude_db`/`band_magnitude_db` (no preamp);
measured 3.4 ms mean, 4.2 ms worst per paint with the live spectrum at 60 paints
a second, so the scene-graph renderer is not needed yet.

**Connected to outputs.** `Outputs` lists working outputs (`windows/devices`),
refreshes on device notifications, and probes IsoAPO outputs off the UI thread.
Edits go through `DeviceLink`: native writes the region under the seqlock on every
edit; Equalizer APO applies during a drag and persists once on release, on a
worker thread. The balance becomes the channel trims and speaker mute. Selecting
an output loads what it plays (the region, the saved state, or its Isotone.txt
block). The spectrum reads the ring (native) or loopback (Equalizer APO), mixes to
mono, FFT 8192 Hann, smoothed in power (20 ms attack, 300 ms release), on its own
0 to -90 dBFS scale, and disappears 500 ms after audio stops.

Found while building it: IsoAPO's region exists only while a stream plays, so an
edit made while idle reached nothing and the next stream started from the saved
state. `DeviceLink` now reports a region it finds again and the session writes
its state to it at once.

**Band strip scrolling** (owner's report: neither a horizontal wheel nor a drag
scrolled): a QML test reproduced three failures (horizontal wheel, dragging the
columns, dragging the thumb) before the fix. `WheelHandler` takes only the
vertical axis by default, the Flickable was not interactive, and the thumb had no
drag. Fixed with a wheel area over the columns, an interactive Flickable whose
gain sliders keep their drags (`preventStealing`), and a draggable track.

**Live, on CABLE Input** (IsoAPO) to CABLE Output: the app started with
`--output {cable} --add-band 1000,-6` while the cable was idle; once a stream
started the band played, matching the peaking response on both channels to
0.0001 dB at 250 Hz, 1 kHz and 4 kHz (-0.2216, -6.0000, -0.2119 dB against
baseline); after the app exited, the next stream was flat again (no saved state).
A 1 kHz tone at -12 dBFS showed as a peak near -14 dBFS on the spectrum (the band
at 2 kHz takes 1.3 dB at 1 kHz).

**Tests:** `ui_tests` (doctest, 7 cases: FFT, a sine's level, release and channel
mixing, balance both ways, `DeviceLink` against a Local\ region and a sandbox
Equalizer APO directory, loading back); `ui_qml_tests` (Qt Quick Test, 9 cases:
the band strip's scrolling and that a slider drag sets gain instead). Both run in
the UI build only.

**Every filter type, live** (same day, owner's request): a scratch tool wrote each
state through the UI's write path (`state_for_output`, `DeviceLink::commit`) to
IsoAPO on CABLE Input while a silent stream held the region open, read it back,
and `isotone-measure` measured CABLE Input to CABLE Output at the 31 third-octave
points against the core's `magnitude_db` and `phase_deg` (what the graph draws).
One band on the right channel only, so channel 0 stayed flat and channel 1's
phase relative to it is the filter's: peak (Q and bandwidth), low and high shelf
(Q, and dB slope with the corner shift), low pass, high pass, band pass, notch,
all pass. Then on both channels: a peak, the 12-band Main board preset with
preamp, preamp with balance -0.5, a disabled band, bypass. Worst magnitude error
0.0013 dB (band pass at 20 kHz), worst phase error 0.002 degrees, over every point
not below -50 dB (phase: -40 dB). One window, low pass at 20 kHz (-88 dB, not
compared), failed on a capture glitch after retakes. Flat again afterwards within
0.0003 dB; the live Equalizer APO files were byte-identical before and after. The
app then loaded each state from the region and drew it; the screenshots show
every type's curve as expected.

Found in those screenshots and fixed: a bandwidth or dB slope was labelled as a Q
("Q 1.50", "Q 12.00"), and a scroll on such a band set Q mode with that number,
changing the sound at once. `setQ` is now `setWidth`, which keeps the band's unit
(Q in the view's 0.1 to 50, the others held where the processor holds them), and
the column shows "Q 1.41", "1.50 oct" or "12.0 dB/oct". `ui_model_tests` (new,
doctest on `EqSession`) failed before the fix; breaking the mode, the label or the
limit each fails it again. Also new in `ui_tests`: every type, on both channels
and the right one, through `DeviceLink` to a Local\ region and to a sandbox
Isotone.txt, loads back with the same magnitude and phase (1e-3 and 1e-6); dropping
the width mode from the param block, or writing a bandwidth as Q in the text, fails
it.

Not fixed, for the owner: types without gain (low pass, high pass, band pass,
notch, all pass) still show a gain slider and "+0.0 dB" that change nothing; the
graph draws channel 0's composite only, so a band on the right channel alone
leaves a flat curve with its handle at 0 dB until the L / R view exists.

The QML tests failed on the desktop platform later that evening with the band
strip 0 px wide: its Row was never laid out, because the window was not being
drawn (the same binary passed offscreen; the display state was not checked).
They now run offscreen unless `QT_QPA_PLATFORM` is set.

**Writing to Equalizer APO outputs:** editing one writes `Isotone.txt` in Equalizer
APO's config directory, which reloads every Equalizer APO device; Equalizer APO
applies the block only once `config.txt` includes it (attach, not built in the UI
yet).

---

## 2026-09-15: Typed values, gain-less types, Auto by default, band popover, L / R view

Committed first: the Equalizer view as it stood (375094a, owner's request).

**Types without gain** (owner: grey them out). Low pass, high pass, band pass, notch
and all pass show their gain slider and value greyed at 0 dB and take no gain from
the slider, the value, the handle or Reset gain. The band keeps its stored gain, so
it comes back when the type changes back to one with gain.

**Typed values** (owner: type to enter did not exist). Gain, frequency and width in
each column, the preamp and the balance: click, type, Enter. Escape keeps the
value; leaving the field applies a value that reads, and text that does not stays
open and selected on Enter. Values read as the field shows them or shorter
(`typed_value.h`: "−3.0 dB", "-3", "1.2k", "1.20 kHz", "Q 1.41", "1.50 oct",
"12.0 dB/oct"; U+2212 is a minus, a comma a decimal point); a unit that is not the
field's, an exponent, hex, inf or nan is refused. Enter on the unchanged text
changes nothing, so a shown "1.23 kHz" does not round the band to 1230 Hz. Delete
is now a shortcut, not a key handler, so Delete in a field edits the text.

**Auto preamp by default** (owner). The region and Isotone.txt do not carry the
mode, so every output loaded with Auto off. A loaded state is now in Auto unless
its preamp differs from Auto's value by more than 0.01 dB (Isotone.txt rounds
it), in which case it was set by hand and stays; loading never changes what the
output plays. A new or flat state is in Auto.

**Band popover** (BandMenu board), from a column's type name (centred on the
column, above it) or a right click on a handle (centred on the handle, below it
when there is no room above): the eight type tiles drawn by `FilterGlyph` from the
core with the generator's tile parameters, Channels (stereo), Enabled, Duplicate,
Reset gain (greyed for a type without gain), Delete. A press outside or Escape
closes it. Changing type keeps the band's id, frequency, gain and width; a shelf's
dB slope becomes, off a shelf, the Q of the same shape, 1/sqrt((A + 1/A)(12/slope
- 1) + 2) with the gain and slope the processor holds, since the processor reads a
slope as a Q on any other type. Duplicate gives a fresh id and lands next to the
band, selected; nothing at 64 bands.

**L / R view.** L and R draw that channel's composite and bells, with bands not on
it faded; L+R draws the left channel, and the right as a second, fainter line
where the two differ. A handle sits on its band's channel: in L+R, a band on the
right alone sits on the right line. This fixes the right-only band drawn flat with
its handle at 0 dB. The fainter second line is the implementer's choice; the
boards show only one curve. Hidden on outputs that are not stereo (their group
picker is not built).

**Tests:** `ui_tests` 11 cases (3 new for typed values), `ui_model_tests` 7
(gain-less types, Auto on load, type change, channels, duplicate and reset, the
view; now on QGuiApplication, offscreen), `ui_qml_tests` 28 test functions (new:
typed values 9, band popover 11). Every new behaviour was broken on
purpose once: 29 mutations, each failed a test (two needed tests added first: an
exponent in a typed value, and a slope converted at a gain other than 0 dB).
Screenshots of the popover from a column and a handle, and of L+R and R with a
right-only band, taken with the app's new `--click x,y[,right]` option on CABLE
Input.

---

## 2026-09-15: The rest of stage 4, in four work packages

The owner asked for all of stage 4 except EQ by ear. The lead built a shared
foundation (9a7fc6e), then four agents built the packages in parallel worktrees
from the same brief, and the lead merged and tested them.

**Foundation.**
- `AppSettings` (settings.ini) and `AppPaths`: the data directory
  (`%APPDATA%\Isotone`) and the Equalizer APO directory can be moved with
  `--data-dir`/`--compat-dir` or `ISOTONE_DATA_DIR`/`ISOTONE_COMPAT_DIR`, so checks
  and tests never touch the owner's settings, presets or Equalizer APO install;
  the test runners set a scratch data directory.
- Themes from the prototype's app.css tokens: System (Windows' app mode), Dark,
  Light, Custom (Dark with five colours overridden); six accents per theme; band
  colours per band or accent.
- The shell: sidebar open (248 px) or the 72 px rail with an outputs popover and
  the output name under the preset name; the Channels panel collapses to its
  52 px strip; views switch through `UiState.view`; the graph takes the height
  the band strip leaves (422 px at 900, 282 at the 760 minimum).
- Shared controls from app.css (`Button`, `TextBox`, `DialogFrame`, `Popover`,
  `Spinner`, `StatusDot`), `UiState` for dialogs and toasts, and a `Presets` stub
  so the packages could call each other's API before it existed.
- The app is a `QApplication` (the tray icon is Qt Widgets).
- EQ by ear is hidden from the navigation until stage 5.
- L+R with channels that differ is now drawn as the approved prototype draws it,
  the right channel dashed and the ends marked L and R, replacing the fainter
  line chosen the day before.

**The packages.** Each package's full record (what it built, every decision where
the spec and prototype were silent, its tests and mutation table, what is not
verified) is in `docs/notes/stage4-<presets|devices|settings|surround>.md`. The
decisions that change behaviour the owner will notice:

- **Presets** (`presetstore`, `Presets`, `PresetsMenu`, import, export, undo).
  - One JSON file per preset in `%APPDATA%\Isotone\presets`, assignments in
    `outputs.json`.
  - An output with no preset is "Untitled"; no preset is created behind the user's
    back.
  - `modified` compares what the output plays with its preset, so it survives a
    restart and an undo back clears it. Balance, mute and speakers never mark it.
  - A preset assigned to several outputs reaches the others when it is saved, not
    on every live edit (the safer reading of "changes on all of them when edited";
    one line to change).
  - Loading, saving and assigning write the saved state.
  - Undo is per output, a drag is one step, 500 steps; "Band N deleted" has Undo.
- **Devices** (`Devices`, `Devicetool`, the Replace, Attach and Uninstall dialogs,
  Settings Outputs, first run).
  - Status comes from `windows/devices` and `isotone-devicetool status` run
    unelevated; the mapping is in the notes.
  - Every change runs `restart-audio` then `test`; a failed restart offers a
    restart of Windows.
  - Attach needs no elevation: Users have full control of Equalizer APO's config
    directory on this machine.
  - First run shows only while no output works and it was never finished.
  - `--fake-devicetool <script>` drives every state for tests and screenshots.
    Nothing real was installed, repaired or restarted.
- **Settings** (General, Appearance, Shortcuts, About, tray, window).
  - Launch at sign-in writes the HKCU Run value (tests use their own key).
  - Graph gain and frequency ranges, spectrum resolution, release, peak hold and
    tilt.
  - A colour picker for the custom accent and colours.
  - Rebinding with conflicts; the selected band's keys apply while held and commit
    once on release (the compat contract).
  - Global hotkeys through `RegisterHotKey`.
  - Close hides to the tray or quits; one instance per user and data directory.
  - About reads `DisableProtectedAudioDG` as upstream does.
- **Surround** (Speakers panel and view, targets, Showing picker, test tones).
  - Speaker chips add or remove speakers from a band's target; group chips set it.
  - The graph draws the channel in view with the most bands, others dashed.
  - Distances are derived from the delays and one stored farthest distance.
  - Bass management is on while any speaker is small.
  - Solo and test tones end with the view and are never saved.
  - A speaker change saves the saved state's speaker part and keeps its saved
    bands.
  - Measured live at 7.1 on CABLE Input: level -6.0000 dB, 2 ms delay exact in
    phase, test tone -29.994 dBFS RMS on one channel only. The cable is back to
    stereo.

**Merging** (the lead). The surround and presets packages had both split
`useOutput` into `useTarget`, and both changed `commit()`; resolved by keeping
both, with `commit()` recording the undo step and then writing `engineState()`.
The tray test needed `Presets`' real constructor. After the merges: `ui_tests` 38
cases, `ui_model_tests` 75, `ui_qml_tests` 200, and the non-UI build with warnings
as errors and its 7 ctest suites pass.

**Changed by the lead after merging.**
- Closing the window and the tray's Quit both ask about unsaved changes through
  the presets package's `PresetActions.confirmUnsaved`.
- Global hotkeys are off until turned on per action. The boards show Global on for
  EQ, Mute, Next and Previous preset, but the settings package verified that a
  global Ctrl+Left/Right takes word navigation from every other app while Isotone
  runs. This is for the owner to confirm.
- An output's layout name ("2.1", "5.1" with back or side speakers, "7.1" or wide)
  is one function, used by the outputs list and the Speakers view; the list called
  2.1 "5.1".
- Devices keeps Engine and Status on one line and cuts a long output name short;
  the presets popover opens where the prototype puts it; the toast's action is in
  the accent.

**Left for the owner.**
- `C:\ProgramData\IsoAPO\devices\{8f4d2a10-0000-4000-8000-00000000d157}.bin` was
  written by one of the surround package's mutation runs: a saved state for a GUID
  no endpoint has. It does nothing; delete it when convenient. Tests since write
  saved states only to scratch or self-test directories.
- IsoAPO.dll has no version resource, so About shows only its output count.
- Empty table cells show U+2014, as the prototype and Devices.png draw them.

---

## 2026-09-15: Review of the merged UI, and the fixes

Three reviewers read the merged UI by area and proved each finding with a failing
test or a precise trace; three fixers fixed them in parallel worktrees, each fix
with a test that failed before it and a mutation check (75 mutations; 73 made a
test fail, and the two that did not are explained in the devices notes). Details per bug are in the notes' "Fixes after
review" sections.

**Session, presets, speakers**
- Solo and test tones stayed on an output when another was picked, and came back
  as real speaker mutes. They are now cleared on the old output before retargeting.
- Saving or loading a preset dropped solo and tones from a native output: the file
  gets the saved state, the region what plays.
- A layout change was an undo step whose undo put the old layout's masks on the
  new layout. It is no longer a step and clears the history.
- Auto preamp was computed with the EQ off, so turning it back on could clip. It is
  computed as the EQ on plays.
- Undo of a speaker change now saves the speaker setup; a speaker change writes
  the file before the region.
- Import and presets are held to 64 bands on IsoAPO outputs; import lists the rest
  as skipped (`ApoParseResult::band_lines`, new in the core, tested).
- Undo of a distance restores the farthest distance too.
- One saved-state directory (`DeviceLink::saved_state_path`), so no test can write
  ProgramData.
- A wheel spin on a handle commits once when it stops, not per notch.

**Devices**
- Repair and Undo ran devicetool's machine-wide `repair`. `isotone-devicetool
  repair` now takes an endpoint (tested with dry runs on every render endpoint
  here), and the UI repairs only the output selected.
- Equalizer APO outputs whose config.txt does not include Isotone.txt were listed
  as working, though edits did nothing. They are "Not attached" in Devices, with
  Attach, and left out of the sidebar until attached. On this machine that is all
  three Equalizer APO outputs.
- "Off" on an Equalizer APO output is stored and sticks.
- Settings Outputs keeps each row's result after the devices re-read; Retry after a
  failed test retries the test; a busy restart shows as busy, not a Windows
  restart; a declined or failed approval shows its reason; quitting stops between
  steps; Attach with Remove include removes Peace only after the attach succeeds;
  an empty Equalizer APO path is refused.
- `--fake-devicetool` refuses to run without a sandbox compat directory, and fake
  mode also follows the environment variable.

**Window, keys, dialogs**
- Delete, the arrows, Ctrl+Z and the other window keys acted on the band behind an
  open dialog, popover or first run. They are off while one holds focus (global
  hotkeys and the tray still act).
- A press inside a dialog took focus out of it; a second close stacked a second
  unsaved dialog; a tray preset pick asked in the hidden window. Fixed, with the
  key, press and close logic in `WindowKeys`, `PressWatch` and `WindowClose` so it
  is tested as Main uses it.
- Ctrl+S on an untitled output opens Save as.
- The tray shows a shortcut only for actions whose Global is on.
- Custom theme: a light custom background left dark popovers and unreadable text.
  The unset tokens now come from the matching light or dark set, and those between
  background and text are mixed from the custom colours; text on a custom accent
  picks the readable colour.

**After the fixes** (lead): `ui_tests` 40, `ui_model_tests` 98, `ui_qml_tests` 237
(run twice), the MSVC build with warnings as errors and its 7 ctest suites, the
APO self test, the transport and reference-data checks, and the GCC build and tests
all pass. The Devices table keeps each cell on one line.

---

## 2026-09-15: The owner's first use of the app, and the fixes

The owner installed IsoAPO on a real output and used the app. Everything reported
was fixed the same day, each with a test that failed first and a mutation check.

**The spectrum looked bad** (the owner's main point, comparing it with
EasyEffects). It was drawn with one point per pixel, each the loudest FFT bin in
that pixel's span, so every harmonic of the music became a spike and the high end
was a comb. Now the graph asks for one point per 5 px (64 to 320), each the mean
power of the bins in its span (`SpectrumAnalyzer::Bands::Mean`; `Loudest` stays for
anything that wants a tone's own level), smooths them with a short Gaussian across
neighbours and draws a Catmull-Rom curve through them. Judged on a real track from
the owner's music played into CABLE Input with a scratch Media Foundation player,
in both themes. Peak hold was not involved: it is off by default.

**Tilt was reported as doing nothing.** It works: the same track at 4.5 dB/oct
pulls the bass to the floor and lifts the mids. With nothing playing there is no
spectrum to tilt, which is the likely reading. Left as it is.

**The rest, as the owner listed them.**
- The sidebar was broken after collapsing and expanding again: the navigation kept
  the rail's anchor, so the items stayed indented. Anchors gone, and the rail's
  column has its own width.
- Handles are 10 px, 12 selected (the boards had 12 and 14), and a double-click on
  one resets that band's gain, as a double-click on its slider does.
- The undo toast stays 3 s and fades in and out over 250 ms; it moved to
  `Toast.qml` so it can be tested.
- The horizontal wheel over the band strip takes `pixelDelta` where the event has
  it, and every delta eases `contentX` towards its target (260 ms, out cubic), so a
  free-spinning side wheel glides instead of stepping.
- Devices Refresh shows a spinner while it reads (held at least 400 ms) and
  "Refreshed" with a check for 1.5 s.
- A right click anywhere in a band's column opens the band popover, as clicking the
  type name does.
- Add band is centred in its box; the owner's reading of the box (between the two
  rules, so including the Row's spacing) was followed over the board's.
- Renaming a preset hides the current-preset check, so only the save check is there.

**A flaky test found on the way, and its cause.** Two to four cases of
`tst_settingsoutputs.qml` and `tst_devices.qml` failed in the full suite,
differently each run. Not leaked state: a button a phase change has just shown is
placed on the next polish, which offscreen regularly takes longer than the tests'
30 ms wait, and `mouseClick` maps the item where it is at the call, so the click
landed on the old layout (logged: an Apply button read at x 980 w 72, settling to
x 926 w 74; 2 to 5 of 22 clicks per run were stale). The four devices test files
now wait for the layout polish of the item and its parents before clicking, and
Settings Outputs resets the compat config, the Off keys and the scripted outputs in
its `init`. Removing only the polish wait brings the failures back. Qt Quick Test
gives each file its own engine, so the QML singletons do not leak; settings.ini and
the shared compat directory do, which is what the reset covers.

**An Equalizer APO write is retried.** The CI runner failed compat's two-writer
stress test once with a sharing violation (it passed on a rerun of the same
commit, and 10 runs plus 4 at once passed here), which is the same limit the
surround package had noted: a write that loses the race for Isotone.txt was
reported and the edit dropped until the next commit, so a drag's release could
silently do nothing while Equalizer APO, an antivirus or another editor held the
file. The UI's compat worker now repeats a failed persist every 150 ms for up to
10 seconds, a newer edit replaces the one being retried (so a stale state is never
written, and every write reloads every Equalizer APO device), and a write that
stays blocked gives up and keeps its error. `DeviceLink::compat_writes()` counts
the writes, which is what the tests observe.

**After all of it**: `ui_tests` 43, `ui_model_tests` 102, `ui_qml_tests` 263
five times in a row with no failures, the MSVC build with warnings as errors and its
7 ctest suites, the APO self test, the transport and reference-data checks, and the
GCC build and tests.

## 2026-09-15: The spectrum's fade, its points, and a peak fall the owner can tune

The owner's second pass over the spectrum, after living with the first fix.

**It drops instead of fading out.** Stopping the music faded the curve briefly
and then dropped it. Two causes, both in the same place: a player that stops
closes its stream, so no frames arrive at all (not silent frames, none), and the
analyzer kept reading its unchanged history while the display hid the curve
outright once the 500 ms timeout tripped. The fade the owner saw was the tail of
the music leaving the FFT window; the drop was the timeout. A read with no frames
for 150 ms now feeds the analyzer the silence the source is no longer sending
(`SpectrumAnalyzer::push_silence`) and keeps updating, so the curve falls at the
release the settings ask for, and it is drawn until its loudest bin is under
-100 dB (`loudest_db()`), by which point it is off the bottom of the plot.
Checked on a real track into CABLE Input with `--screenshot-after`: full at the
stop, a few dB down at +1 s, near the floor at +3 s, gone by +6 s.

**The points are fixed at 320.** They were one per 5 px, clamped to 64-320, so
the curve's detail changed with the window's width. The owner asked for the
maximum at every width. It draws slightly busier in a wide window than the old
214 points did, which is the owner's call and was said.

**Peak fall is a setting** (Settings, General, Spectrum). It was a constant
6 dB/s; the owner wanted to tune what the peak-hold line does, so it is a value
in dB/s, greyed while the line is off. Release was already a row and is what
governs the fade above.

**A checking flag**: `--screenshot-after <seconds>`, so a screenshot run can
catch what the window shows some seconds in, which is the only way to see a fade.

Mutation checks: silence fed as a no-op (the level stands at -6.02 dB for ever,
caught), and the peak fall back to the constant (caught). `ui_tests` 44,
`ui_model_tests` 102, `ui_qml_tests` 264, no failures.

## 2026-09-15: The spectrum's settings cut back, and the settings' own type

The owner's third pass. What was added the same day for the spectrum was mostly
the wrong thing, and this is what replaced it.

**Gone**: the resolution setting (pinned at 16384, the highest it offered, so the
bins are 2.9 Hz at 48 kHz), the peak-hold line and the peak fall added earlier
today, and the tilt. `SpectrumAnalyzer` no longer keeps a peak vector or a tilt.

**The two settings that are left are sliders**, in Settings, General, Spectrum:
- **Decay**, 50 to 500 ms, default 150 (the owner's), the meter release: how long
  a level takes to fall. It was a click-to-type box and the owner could not
  change it.
- **Smoothing**, 0 to 100%, a Gaussian across the drawn points whose sigma is the
  amount times 8 points. 0 leaves the points alone (spiky), 35% is the default
  (about a quarter octave), 100% is about an octave. It was a fixed 5-tap kernel.

**The spectrum's scale**: it was 0 dBFS at the top of the plot down to -90 at the
bottom, so music sat in the bottom third and a 12 dB cut moved it a third as far
as it moved the EQ curve. It now spans 60 dB, and its top follows the loudest
band being drawn (`EqSession::followTopDb`: 3 dB of headroom, up in 200 ms and
down over 2 s, held between 0 and -45 dBFS), so a loud passage is never cut off at
the top and the curve fills the plot whatever the music's level.

**The preamp is taken out of the levels the graph shows**
(`EqSession::removePreamp`). This is why the owner said an EQ filter did nothing
to the spectrum: with Auto preamp a +12 dB bell sets the preamp to -12, so what
the output plays has the band lifted 12 and everything, that band included,
dropped 12. The band ends up where it started and the rest of the curve sinks,
which reads as the EQ doing nothing. Cuts did show, because Auto leaves the
preamp at 0 for them, which is why this took two passes to see. Checked on a real
track into CABLE Input: a +12 dB bell at 4 kHz now lifts 3 to 6 kHz by its own
shape while the rest of the curve stays where it was.

**The numbers in the graph's handles** were low and half a pixel to the left. A
digit sits on the baseline and reaches the cap height, so centring the text's box
(which carries the whole descent) puts it low, and `anchors.centerIn` rounds the
position to a whole pixel. The number is now placed on the whole pixel nearest
centred on its own ink (`TextMetrics.tightBoundingRect`), and drawn by Qt rather
than the native rasterizer.

**The spectrum is clipped to the plot.** Where it ran along the bottom it spilled
a few pixels past it: the points are clamped to the plot, but a Catmull-Rom
segment between two points that both sit on an edge overshoots. The fill and the
edge are drawn inside a clip of the plot rect. The test paints the graph into an
image with a spectrum that dives to the floor and back, and counts coloured
pixels outside the plot: 14 below it before the clip, none after.

**One type for the settings' controls**: `Segmented` was 12 px regular, except on
Appearance where it was 13, while every value chip is 13 DemiBold; and it was
given a fixed height with its row pinned 3 px from the top, so the raised option
sat low. It is 13 DemiBold everywhere now and centres its row.

Mutation checks: the smoothing amount ignored (the smoothing test fails), and the
handle number back on `anchors.centerIn` with the old half-pixel nudge (the
centring test fails). `--screenshot-after <seconds>` is a new checking flag.
`ui_tests` 42, `ui_model_tests` 103, `ui_qml_tests` 263, no failures.

## 2026-09-15: Importing a curve, not filters

The owner imported `TC8FD05-04 EQ.txt`, exported from Peace, and got an empty EQ.
The file is one 1302-character line in Audacity's `FilterCurve` form: 50 frequency
and value pairs, a magnitude curve rather than filters. `parse_apo_config` read it
as a filter line, found it longer than upstream's 1024-character limit, and
skipped it, so nothing imported. Equalizer APO plays these (and its own
`GraphicEQ:` line, which is what AutoEQ writes) as a convolution filter. Isotone's
engine is biquads, so a curve has to be fitted.

**`core/curve_import.h`**: `parse_curve` reads either line, from a file of its own
or inside a larger config; `fit_curve` returns the filters whose composite follows
it. Each band is the one that takes most of the remaining error out, chosen from a
sixth-octave grid of frequencies and seven widths, added one at a time until the
curve is followed within 0.22 dB rms or twelve bands are used; their gains are then
solved together (normal equations over a 256-point log grid) and refined three
times against the real composite, since a peaking filter's shape widens a little
with gain.

The first attempt put a band on every third-octave centre and solved all 31 gains,
which fitted well and gave the owner thirty filters for a curve he had made with
thirteen sliders, six of them at 0 dB ("theres like 30 fucking filters, wtf man",
with a screenshot of the Peace window). A file of a handful of filters has to come
back as a handful. Choosing them one at a time gives ten for that file at 0.21 dB
rms, and they line up with what he set: a high-pass near 100 Hz, a lift at 127,
cuts at 254 and 1437, a high shelf at 5.9 kHz, and the top two.

**The shapes first.** Peaking bands alone fitted the owner's curve to 0.72 dB rms
but were 3.9 dB out at 22 Hz: the file asks for -40 dB at 10 Hz and -26 at 20, a
rolloff no peaking band holds. The owner said why: "it uses a high shelf along
with a high pass filter". So the fit now looks for those first. A high-pass is
searched over fc 12 to 300 Hz and Q 0.5 to 1.4, a high shelf over fc 1 to 12 kHz
with its gain solved in closed form, each taken only when it beats leaving the
region to the bands by a clear margin; what they leave is the bands' target.

Measured: a curve sampled from four peaking filters comes back as five bands
within 0.17 dB rms (0.50 worst), and the shapes are not picked up where they are
not wanted. A curve
made of a high-pass at 45 Hz, a high shelf and a peak comes back as three filters
at 0.13 dB rms, with the high-pass found within a quarter of its frequency. The
owner's own curve fits to **0.21 dB rms over its whole range, 10 Hz to 18.9 kHz**,
worst 0.77 dB.

The import preview fits when the file has no filters of its own, and the dialog
gains a "Curve fit" stat: the points the file held and the worst error, so the
approximation is visible rather than implied. A file of filters is untouched.

`core_tests` 212 on MSVC and GCC, `ui_tests` 42, `ui_model_tests` 105,
`ui_qml_tests` 265. The QML test imports the owner's file through the dialog and
checks the drawn curve at 25 Hz, 137 Hz, 253 Hz and 10.2 kHz.

## 2026-09-15: The preset is not the device's

Three of the owner's, after importing his curve.

**The output under the preset name is gone.** With the sidebar collapsed the top
bar showed the output's name and a chevron under the preset's, a second way into
the outputs popover; the prototype's Collapsed board has it, and the owner does
not want it ("Headphones (Anker USB Audio), that's what I'm talking about,
remove"). The rail's speaker icon still opens the outputs, so nothing is stranded.
`TopBar` no longer has an `outputsRequested` signal.

**An imported file with no preamp gets Auto.** A file carries no Auto mode, so
import read one: Auto only when the file's preamp was what Auto would set. A file
with no Preamp line at all (every curve, and plenty of configs) has none chosen,
which is what Auto is for, and it came in at 0 dB with the EQ clipping. Now no
preamp in the file means Auto; a file that carries one keeps it, by hand, as
before. New presets were already Auto by default (Settings, General).

**A preset is for every output until it is narrowed.** It was tied to the output
it was saved on: there was no "all", and no way to change it after. A preset now
carries the output it is for, a `forOutput` GUID in its file, empty for every
output (a file written before this reads as every output):

- **Save as** asks: For, with All outputs first and then every output, starting on
  All outputs. Its list opens under the box and the card grows for it, so the name
  above is not covered.
- **The presets popover** shows "Only <output>" under a preset that is narrowed,
  and nothing under one for every output. Its row has a device icon that opens the
  choices in the row itself, so the list's own scrolling cannot clip them.
- **Import** offers the same choice, starting on All outputs: the file is read for
  the current output's layout, created, and loaded there, and no other output is
  written. Picking an output keeps what import did before.
- **Every preset is listed on every output**, whichever one it is for. The first
  cut hid the ones for other outputs, and saving a preset for the cable while on
  the headphones made it vanish the moment it was saved ("wtf i created a new one
  for the cable device and when i saved and went to a different preset, it just
  disappeared"). What a preset is for is a label and what an output picks up, not
  a filter.

`AssignedRole` is now what the preset is for rather than where it is loaded.

`ui_model_tests` 106, `ui_qml_tests` 266. The new QML test needed the same wait
for a layout polish as the devices tests: the row grows to hold the choices, and a
click sent before that lands where they were.

## 2026-09-16: Loose ends after the owner's evening

**A flake seen once, unreproduced.** One `ui_model_tests` run failed 2 of its
1644 assertions, in the run straight after the app was force-killed and the tree
rebuilt; its output was gone before it could be read. Twelve runs since have
passed: five plain, three with the app running, and four more each straight after
killing it. The most likely cause is the devicetool exe having just been written
when a devices test spawned it, since those tests compare an in-process read with
what `isotone-devicetool status` says. Left as it is, on the record, rather than
guarded against something that has not been seen twice.

**A preset narrowed to one output does not load on another.** Testing the
default-output change, the owner found that an output comes back to the preset it
last had, and since a preset narrowed to another output could still be loaded
anywhere, an output could come back to one that is not for it. The first answer
was to drop such an assignment when switching to an output, which the owner then
hit from the other side: a cable-only preset clicked while the headphones were
current took them over, and left them untitled with the filters still playing on
the way back. The rule is now the simple one he chose: **a preset for one output
loads on that output and nowhere else.**

- Its row on another output is faded and does not load; the row still takes the
  click, so a dead click does not fall through and close the popover.
- The row's icons still work there, so it can be widened to every output from
  where it is seen.
- Saving for another output makes the preset and leaves this output as it was.
- Switching to an output still drops an assignment to a preset that is for another
  one, which is what repairs the state this left behind. GUIDs are compared
  without case, since the one a preset carries need not be spelled as the session
  spells it.

Mutation-checked both ways: without the load guard the new test fails three
assertions, and with the switch guard disabled the older one fails.

**The stray saved state is gone**: the `{8f4d2a10-...d157}.bin` under
`%ProgramData%/IsoAPO/devices`, left by a test under a GUID that is no endpoint
on this machine. The one real file there, `{407cef09-...}.bin`, was
not touched.

## 2026-09-16: The fade ends at the bottom of the plot

The fade after the music stops still ended in a flash. Two causes worked
together. The curve was hidden once the loudest single FFT bin fell under a fixed
-75 dBFS, which is neither the curve the graph draws (mean-power bands, lower
than the loudest bin) nor anywhere in particular on the plot, whose bottom is the
scale's top minus 60 dB. And the top kept following the falling level, so the
plot's bottom sank along with the curve and the curve hung part way down until
the cutoff took it. The owner put it exactly: time it from the top and let it fall
all the way down.

`EqSession::nextSpectrumScale` is one tick of the scale. While audio arrives the
top follows the loudest band being drawn, as before. Once it stops the top holds
still, so the curve falls through a plot that is not moving, and it is drawn until
the loudest drawn band is under the plot's bottom. The range lives on `EqSession`
(`kSpectrumRangeDb`), and `ResponseGraph` asserts at compile time that it draws the
same one. The silence fed to a stopped stream now runs for as long as the curve is
drawn, rather than to the old fixed level.

Checked on a real track into CABLE Input at the default decay (150 ms): full at the
stop, then down through the middle at 0.9 s, the last peaks at the bottom edge at
1.9 s, and gone by 2.6 s, off the bottom. Mutation-checked: letting the top follow
while idle fails 72 assertions, and the old fixed cutoff fails 2. `ui_tests` 42,
`ui_model_tests` 110, `ui_qml_tests` 267.

## 2026-09-16: Stage 5 begins: the owner's decisions, and the tone

**Owner's decisions**, on the questions raised at the start of stage 5:

- **EQ by ear makes an entirely fresh preset.** It does not edit the preset the
  output has. This settles which bands an A/B refit may replace. The view has its
  own New control that starts it; until then the tone plays through whatever the
  output has (asked with the alternatives of starting it on first Play or on
  opening the view).
- **Stereo and 2.1 only.** Anything with more channels is not a use case for it
  and is ignored. L and R are the front pair.
- **Marks in any order work.** The prototype's handling of them is not a rule: the
  prototype was for checking the basic starting point of the UI, nothing more.
- **The spectrum does what it does** while the tone plays.

Plan 8.1's Web Audio tone and its warning when the edited output is not the
default are gone with Electron: the tone renders straight to the output being
edited, so it always passes through that output's engine.

**The tone.** `SineTone` (`ui/backend/sine_tone.h`) is the generator, and
`TestTone` now plays any source on a channel mask; the Speakers pink noise is the
same overload as before, on one channel.

- A full-scale sine is 0 dBFS, so the default -30 dBFS is a peak of 0.0316. The
  pink noise's -30 dBFS is RMS, 3 dB louder than a sine of the same number.
- The phase runs through every change. A new frequency glides in octaves (one
  pole, 20 ms), the level ramps (one pole, 10 ms), and on/off is the level fading
  to or from silence, snapped to exact zero under -140 dBFS so the tail does not
  decay into denormals.
- Auto sweep: a rate in octaves per second, carrying the glide with it so a sweep
  does not lag, turning at 20 Hz and 20 kHz by what it went past. Setting a
  frequency while it sweeps moves it there and it sweeps on.
- The render is identical however the stream slices it, which the setters rely
  on: they are atomics read once per render.

Tests (`ui/tests/test_sine_tone.cpp`, seven cases): level and frequency at 44.1,
48 and 96 kHz; the 20 Hz to 20 kHz range; over 1000 random frequency, level and
on/off changes in random blocks, no sample step past the sine's own slope plus
one ramp step; the glide; the fades and exact silence; slicing; the sweep's rate
and both turns, rendered in 10 ms blocks as the stream does. Mutation-checked,
each caught: no glide (1 assertion), level applied at once (3), phase restarted
each render (2), no snap to silence (1), a sweep that lags (3), the direction
reset on every render (2), no turn at the ends (2).

**Live, on CABLE Input with IsoAPO, captured from its ring** (IsoAPO's output), a
scratch harness driving `SineTone` through `TestTone`, the region written flat
first:

| Check | Result |
|---|---|
| 1 kHz, -30 dBFS, mask L, R, Both | -30.000 dBFS on the channels chosen, exact zeros on the other |
| PK 1 kHz −12 dB Q 1 | 1 kHz −42.000, 300 Hz −31.441; RBJ maths −42.000 and −31.441 |
| Same, bypassed (control) | −30.000 |
| Tone changes (glides 100 to 400 Hz, level, on/off, sweeps up and down) through a flat EQ | largest sample step 0.9617 of the bound, which is the 400 Hz sine itself; zeros only in the two off periods; L and R identical; sweep ended at 134.35 Hz as computed |
| Band gain dragged 0 to −12 dB and back (0.3, 1 and 3 s, about 45 writes a second in process) and typed jumps, under a 1 kHz tone | detector `x[n+1] − 2cos(ω)x[n] + x[n−1]`, zero for any 1 kHz sine: 2.5e-3 of the amplitude at worst while the gain moves, 4.2e-7 steady. Controls on a synthetic sine: an instant −12 dB step 0.098 to 0.75 by phase, a 1 ms linear ramp 4.2e-3. The level follows each drag and a jump settles at −42.0 |
| Pink noise on R (the Speakers overload) | matches `PinkNoise`'s loop sample for sample over 6 s (7.5e-9); L exact zeros |

The drag result is IsoAPO's. On an Equalizer APO output an edit is heard when it
is committed, and each write rebuilds its filters from rest (2026-09-13), so a
drag there cannot be click-free; that stays as it is.

Nothing persisted: every write went to the region, which went with the stream.
The saved state files were not touched. That listing corrects the machine state
below: `%ProgramData%\IsoAPO\devices` holds a file for CABLE Input
(`{798436d2-...}.bin`, written 2026-09-15 23:18, from the owner's use of the
app on the cable) as well as the headphones'.

`ui_tests` 49, `ui_model_tests` 110, `ui_qml_tests` 267. `isotone.exe` was not
relinked: the owner's copy was running from `build-ui`.

## 2026-09-16: EQ by ear, sweep

The sweep half of stage 5, built to the `EqByEar` board with its geometry from
the generator (`gen_screens.py`, `eq_by_ear`, `log_slider`, `band_list`).

**`EqByEar`** (`ui/src/eqbyear.h`, a QML singleton over `EqSession`) holds the
tone, the frequency, the marks and the band they make. The view is
`EarView.qml` with `SweepSlider.qml`, `EarBandList.qml` and `VolumeDialog.qml`;
`GraphCard` draws the cursor, the marks and the marked span when `ear` is set.

Where the spec and the owner were silent:

- **Only on stereo and 2.1 outputs.** The navigation item shows when the output
  is 2 channels, or 3 with the 2.1 mask `0xB` (3.0 is not 2.1); an output that
  stops being one sends the view back to the Equalizer. L and R are channels 0
  and 1, the front pair. A band on Both is every channel on stereo and the front
  pair on 2.1, not the sub.
- **The band from marks**: Peak at Top, Q = Top / |End − Start| to two places,
  held to 0.1 to 50 (Start and End at one frequency is 50), −3 dB for Peak and +3
  for Dip. Marks may be in any order. A mark records the frequency to the hertz;
  Add band clears them. `EqSession::addBand` takes the Q and a channel mask so the
  band is one undo step, and stamps the layout on the state when the mask is not
  every channel.
- **New** is a button at the left of the controls, the presets' own New flow
  (unsaved changes are asked about first).
- **The volume dialog** shows before the first play of each app session, with the
  tone level and the output. Entering the view makes no sound, so it is not shown
  then.
- **Level**: a slider from −60 to 0 dBFS and a typed value, held to −90 to 0 (over
  full scale is clipping). **Sweep rate**: typed, 0.05 to 10 octaves a second.
  `TypedUnit` gained `Dbfs` and `OctavesPerSecond`, so the shown text can be
  edited in place.
- **Keys** while the view shows: Left and Right a 48th of an octave, with Shift a
  sixth, Page Up and Page Down an octave, Space play and pause. A field being typed
  in keeps its keys; a dialog or popover with the focus holds them all. Delete
  removes the selected band here too.
- **The tone stops** (fades out) when the view goes, the window hides, or the
  output or its layout changes; the marks clear with the output. A paused tone
  keeps its stream for 2 s, so playing again does not reopen it. A channel change
  while playing fades out, reopens the stream on the new channels after 120 ms,
  and fades in. A stream that fails stops the tone with a "Tone stopped" toast.
- **The cursor** on the graph drags to sweep; the hover readout is off in this view.

Tests: `test_eqbyear.cpp` (6 cases: the band from marks, which outputs, nudges and
range, marks, Add band on stereo and 2.1 and one undo step, another output),
`tst_earview.qml` (11: marks and Add band, scrolling to the new band, the volume
dialog once, keys, a field keeping its keys, the log slider, channel, level and
rate, leaving the view, the cursor), typed units. Mutation-checked, 16 of 16
caught: Q from a signed span, Dip's sign, 2.1's Both as every channel, 2.1 by
channel count alone, the tone and marks kept on another output, marks not
rounded, marks not cleared by Add band, the level not held under 0 dBFS, the
layout not stamped with a mask, the Q not passed, the dialog asked every time,
the tone kept when the view goes, the list not scrolled to a new band, Page Up
not bound, a linear slider, a cursor that does not follow. Two survived a first
run and changed the tests: the layout stamp needed a state that names no layout,
and scrolling on a count change was redundant (adding a band selects it) and went.

**Live, the app on CABLE Input** (a second build in `build-ui-check`, scratch
data and config directories), captured from IsoAPO's ring; the cable's saved
state was backed up first and is byte-identical after:

| Check | Result |
|---|---|
| Marks set from the slider, Add band | band 9: 3.41 kHz, −3.0 dB, Q 1.89, selected, scrolled into view; screenshots match the board's positions (marks at 566, list at 640, rows at 733 to 853) |
| Play through the dialog, 3556.56 Hz from the slider | 3556.559 Hz at −41.833 dBFS; −30 + Auto preamp −7.621 + the nine bands' −4.216 = −41.837 |
| Auto sweep at 1 oct/s | 1.0001 oct/s fitted over 2.5 s, the display and cursor following |

## 2026-09-16: The graph lagged maximized with the spectrum running

The owner: maximized, with the spectrum running, the whole app lags and the
spectrum is not smooth. On his 2560 × 1440 display, with pink noise into CABLE
Input, Qt's render loop timing (`qt.scenegraph.time.renderloop`) put all of it in
the sync phase, which blocks the GUI thread: 6 ms a frame at 1440 × 900, 15 ms
(17 at p90) maximized, where frames came every 19 ms instead of 16. Timing inside
`ResponseGraph::paint` (temporary, not kept) split the 15.6 ms at 2228 × 873: grid
and labels 4.4, spectrum 1.1, band bells 5.3, composite fill and stroke 4.6, the
maths under 0.3. Every spectrum frame rasterized the whole graph, though only
the spectrum had changed, and the bells' share grows with the band count.

`ResponseGraph` now draws a `part`: all of it (the Appearance preview), or the
grid, the spectrum or the curves, and `GraphCard` stacks three. A spectrum frame
repaints the spectrum; an edit repaints the curves and the spectrum, not the grid;
the range, the size and the style repaint all three. The picture is the same:
screenshots of both views before and after differ in about 950 pixels, by at most
2 of 255, from blending layers instead of drawing on one surface.

After, the same run: sync 1 ms maximized and under 1 windowed, frames every 16 ms
in both. `tst_graphlayers.qml` counts each part's paints; repainting every part on
a spectrum frame fails it, as does repainting the grid on an edit.

`ui_tests` 50, `ui_model_tests` 116, `ui_qml_tests` 281.

## 2026-09-16: Adding to a peak by ear, and short windows

**Add band where a peak already is adds to it** (owner). The same place is an
enabled Peak band on the same channels (a band on every channel counts as the
output's every channel) within a sixth of an octave of Top; the nearest if more
than one. Its gain takes the marks' −3 dB (Peak) or +3 dB (Dip), held to ±24; its
frequency and Q stay; it is selected, and the change is one finished edit (one
undo step, written to the output). At the 64-band limit Add band stays enabled
over such a peak. Mutation-checked, 8 of 8: never merging, a third of an octave,
channels ignored (it merged the 2.1 right into the pair's band), disabled bands,
other types, the first rather than the nearest, the limit, and no finished edit.
The last survived first because `undo()` records a pending edit before undoing; the
test now counts `committed`.

**Short windows** (owner: in a short, wide window show only the graph or only the
bands, automatically, with a setting for which). Settings, General, Graph, **Short
window**: Graph (the default) or Bands, `graph/shortWindow`.

- The Equalizer view shows both while the graph gets at least 200 px (a window 678
  high and up, as before); under that, only the one chosen. The graph alone takes
  the height under the top bar less 24; the strip alone keeps its 402.
- The window's least height is what is kept at its least: 300 for Graph (a 200 px
  graph), 478 for Bands. It was 760. The least width stays 1120.
- The sidebar scrolls when the window is too short for its navigation and its
  foot, in both widths, instead of the foot running into the navigation. The
  Devices view (760) and EQ by ear (754) scroll inside a shorter window; Settings
  and Speakers already scrolled. The other views are unchanged.
- `--size <w>x<h>` sizes the window for checks.

Checked in screenshots at 1440 × 700, 478, 300 and 100 (held to 300), with each
setting, both sidebar widths, EQ by ear and Devices. `tst_shortwindow.qml` (the
threshold, each choice and its least height, the sidebar at 900, 478 and 300);
mutation-checked, 3 of 3: the strip always shown, the setting not read, the sidebar
not scrolling.

**A top bar error found on the way**: the status pill's position still read the
output name's width, taken out of the top bar on 2026-09-15, so with the sidebar
collapsed its binding threw a ReferenceError and kept its old value. It follows the
preset name now; `tst_devicespill.qml` fails on the warning with the old line.

`ui_tests` 50, `ui_model_tests` 117, `ui_qml_tests` 288.

## 2026-09-16: New beside the frequency, and always on top

- **EQ by ear's New** is a primary button right of the frequency and its nudges
  (owner), no longer at the left of the channel row.
- **Always on top** (owner: like PowerToys' Always On Top): Settings, General, a
  Window section with one toggle, `window/alwaysOnTop`, off by default. The window
  carries `Qt.WindowStaysOnTopHint` while it is on, and changing it applies at once.
  Checked on the running app from outside it (`GetWindowLongPtr`, `WS_EX_TOPMOST`):
  on at start when saved on; off, then on after the toggle; on, then off after it.
  The window's flags binding has no unit test; that live check is its evidence.

`ui_tests` 50, `ui_model_tests` 117, `ui_qml_tests` 289.

## 2026-09-16: EQ by ear's channel is the top bar's; the title bar with always on top

**EQ by ear** (owner):

- **No channel picker of its own**: it repeated the top bar's. The tone plays on
  the view's channel: L, R or both for L, R and L+R on stereo. On 2.1 the top bar
  has the Showing picker instead, so the tone is L or R when that front speaker is
  shown without the other, and both otherwise. A band from marks takes the same
  channels as before, from that. Mutation-checked, 4 of 4: the view not followed,
  not followed at start, 2.1 always both, L and R swapped.
- **Level and auto sweep** are on the play row, right of the frequency; the row
  above it is gone and everything under moved up 48 px (the view's least height is
  706).
- **New** is **New preset**, still primary, left of Clear and Add band.

**Always on top lost the title bar.** Evidence, from the running window's styles
(`GetWindowLongPtr`): off, `0x96CF0000` (caption, system menu, sizing frame,
minimize and maximize); on, `0x96040000`, the sizing frame alone, so no buttons
and nothing to drag. Qt takes the hints a window is given as all of its
decorations, and `Qt.WindowStaysOnTopHint` alone gave none. The flags now carry the
title bar's hints with it. After: `0x96CF0000` in every state (started on, started
off, toggled on, toggled off), with only `WS_EX_TOPMOST` changing.

`ui_tests` 50, `ui_model_tests` 118, `ui_qml_tests` 289.

## 2026-09-16: A press on the graph moves the tone; the app's icon

- **EQ by ear's graph**: a press anywhere on the plot moves the tone there and a
  drag carries on, as the slider does (owner). It replaces the 18 px strip around
  the cursor. Band handles are above it, so a press on one takes the band and
  leaves the tone; a double click still adds a band. Tested in `tst_earview.qml`;
  without the press handler, or the double click, it fails.
- **The icon** (owner: a placeholder until the real logo): `logo_mark_icon`, the
  tray's, is the application's window icon, now drawn up to 256 px, and
  `res/isotone.ico` (the same drawings, a PNG entry per size, 16 to 256) is the
  exe's own through `res/isotone.rc`. Read back from Windows: one icon in the exe
  (`ExtractIconEx`), and the running window's large and small icons
  (`WM_GETICON`), all three the logo mark. A pinned taskbar shortcut may show
  Windows' cached icon until it is pinned again.

`ui_tests` 50, `ui_model_tests` 118, `ui_qml_tests` 290.

## 2026-09-16: The frequency grid, as squig.link draws it

The owner could not tell what the minor lines were. They were 1, 2, 3, 4, 5, 6 and 8
in each decade: from 100 to 200 (and 1k to 2k, 10k to 20k) there was no line in a
gap of 0.30 decades, where 50 to 100 had three. Read in squig.link's
`graphtool.js` (fetched 2026-09-16, `xvals = [2,3,4,5,6,8,10,15]` over three
decades, `tickPattern = [3,0,0,1,0,0,2,0]`, `tickThickness = [.2,.4,.4,.9,1.5]`):
lines at 1, 1.5, 2, 3, 4, 5, 6 and 8, every gap between 0.08 and 0.18 decades,
with the 2s (20, 200, 2k, 20k) heaviest, the 5s and 10s next, the rest faint.

`ResponseGraph::gridLines` now gives those lines with a weight: Strong for the 2s,
Major for the 1s and 5s (the labelled ones, as before), Minor for the rest. Minor
lines draw in the minor grid colour, Major in the major, Strong halfway from the
major grid colour to the zero line's, 1 px each. The labels are unchanged. The
narrow-range fallback is as it was, with Major every fifth step. Tested: the lines
and weights at 20 Hz to 20 kHz, and no gap more than 2.3 times another; without
1.5, or with the 1s strongest, it fails. Checked in both themes.

`ui_tests` 50, `ui_model_tests` 118, `ui_qml_tests` 290.

## 2026-09-16: A clear separator at every label

The owner, on the squig.link weights: not what was meant. The labels are right;
**the clear separators go at every label**, all alike. The 2s' extra weight is
gone. The lines stay squig.link's (1, 1.5, 2, 3, 4, 5, 6, 8 in a decade), and
`GridLine` is back to major or not.

The paint draws every grid line that has no label faint (the minor grid colour),
then a separator at every label frequency, whatever range the labels come from (1s,
2s and 5s; every line on a range with few of those; the narrow steps), in
`separatorColour`, halfway from the major grid colour to the zero line's. The
labels are the same list, drawn as before. Tested by painting the graph into an
image at 20 Hz to 20 kHz, 300 to 700 and 1100 to 1900: each label's column is the
separator colour and each other line's the minor colour; drawing the old major and
minor colours instead fails 23 of 38 checks. Checked in both themes.

`ui_tests` 50, `ui_model_tests` 119, `ui_qml_tests` 290.

## 2026-09-16: Three weights of separator

The owner, after trying every label alike: **major separators at 100, 1k and 10k,
less major ones at every other label, minor ones at every other line.**

- The decades (a power of ten, `ResponseGraph::isDecade`) draw in the zero line's
  colour, whether or not they carry a label, so a narrow range with 1 kHz in it
  still has its major line.
- Every other label draws in `separatorColour` (halfway from the major grid colour
  to the zero line's), as the entry before.
- Every other line draws in the minor grid colour.

Tested by painting the graph at 20 Hz to 20 kHz (three decades), 300 to 700, 1100
to 1900 and 900 to 1100 (a decade in the narrow steps); drawing the decades as the
other labels fails 8 checks. Checked in both themes.

Then (owner): the less major lines a tad less obvious, closer to the minor ones.
`separatorColour` is now a quarter of the way from the major grid colour to the zero
line's, not halfway: #2F3237 in the dark theme, from #383C41 (minor #1A1D22, major
#26292E, zero #4B4F54). The test pins the quarter, and failed at halfway.

`ui_tests` 50, `ui_model_tests` 119, `ui_qml_tests` 290.

## 2026-09-16: A/B is for later; stage 4's open decisions

**A/B mode is set aside** (owner): he will not use it; it may come later if people
ask for it. Plan 8.4 and the `EqByEarAB` board stay as the design for then, with the
questions it left open (which bands a refit may replace in a fresh preset, whether
points are kept). Stage 5 is done without it.

**Stage 4's open decisions** (owner):

- **Global hotkeys are on by default.** `ShortcutRegistry::isGlobal` reads a Global
  never set as on, for EQ, Mute, Next and Previous preset, as the boards show; the
  tray shows their keys from the start. Said once before the change: a global
  Ctrl+Left and Ctrl+Right take word jumps from every other app while Isotone runs;
  each can be turned off or rebound in Settings, Shortcuts. The registry test
  failed on the old default (4 checks); the tray test now expects the keys by
  default.
- **Preset edits reach other outputs on save**, as they do: switching outputs loads
  that output's preset and asks to save first.
- **Output names that differ by a little stay as they are**: they are the devices'
  names, which the app cannot change.
- Launch at sign-in's check after a restart is his, later.

`ui_tests` 50, `ui_model_tests` 119, `ui_qml_tests` 290.

## 2026-09-17: Dragging a handle lagged maximized, and it was the bells

The owner, after the maximized spectrum lag was fixed: maximized, dragging the
handles on the graph makes everything lag. The layer split held, so this was
something else.

Timing inside `ResponseGraph::paint` at a 2168 px plot width, per paint:

```
  bands   bell maths   bell path   bell stroke   composite
      1         0.07        0.05          3.05        0.56
     10         0.73        0.42         27.38        0.80
     24         1.83        1.09        109.84        1.47
```

`strokePath` on the bells is 97% of it and grows with the band count, so at 24
bands one curves paint took about 110 ms: six dropped frames for every mouse
move. Not the edit path, which costs 0.175 ms for the pair of calls a drag makes,
and not the spectrum. Auto preamp adds about 1 ms a pair and is not the problem
either. `drawPolyline` in place of `strokePath` changed nothing, so it is the
antialiased rasterisation itself, around 1.4 microseconds a segment, and each
bell carried one segment per pixel of plot width.

**Each bell is now simplified before it is stroked**, by Ramer-Douglas-Peucker on
the vertical distance alone, which is the whole error where x is one sample per
pixel and monotonic. Most of a bell is flat, and those points cost stroke time
while carrying no shape.

**Sampling more coarsely instead would have been one line, and it is wrong.** It
blunts exactly the high-Q bells: against the unsimplified rendering of a set
including a Q 12 and a Q 30 band, halving the points moved pixels by up to 124 of
255. The simplification at 0.05 px moves 0.5% of the pixels by at most 8 of 255,
mean 1.9, which is a fraction of one antialiased edge. Loosening the tolerance
buys no speed, so it is left tight: 0.3 px costs the same and moves those pixels
by up to 36.

`ResponseGraph::simplifyKeep` is a static member beside `smoothForDisplay` so it
can be tested: a straight line keeps only its ends, a one-pixel spike keeps the
spike and its neighbours, a bell keeps under half its points, and every point
dropped stays within the tolerance of the line that replaced it.

End-to-end drag timings were too noisy on a loaded machine to tune against (ten
bands swung between 28 and 45 ms a step across identical configurations); the
in-paint figures above are the ones to trust. This is a large improvement rather
than a complete fix at high band counts. If it is still not smooth, the
structural answer is to keep the dragged band's bell with the composite and leave
the other bells on a layer that does not repaint during a drag, which makes the
cost one bell a frame whatever the band count.

## 2026-09-17: Linux, the targets and the environment

**Targets** (owner): every desktop and session, as EasyEffects does: Wayland and
X11; GNOME, KDE Plasma and Cinnamon. The owner uses Linux Mint (Cinnamon).

**Environment** (agreed, then superseded: see "Linux, the environment as built"
below, which records what was actually stood up and why):

- A Linux Mint Cinnamon VM on this machine, reached over SSH, for the builds,
  PipeWire, measurement through virtual sinks (the Linux counterpart of the VB-Cable
  rig) and screenshots of the UI in a real session. About 4 cores, 8 GB, 60 GB.
  Windows here is 11 Home, so no Hyper-V: VirtualBox (free, no account) first;
  VMware Workstation Pro is the alternative if Wayland desktops need better 3D.
  The owner installs VirtualBox and Mint, then `openssh-server`, and adds a public
  key generated on this machine.
- Later, VMs or snapshots for GNOME on Wayland and KDE Plasma on Wayland and X11,
  once there is a UI to check on them.
- Not WSL2 (not installed here): headless only, no desktop to check the tray,
  hotkeys or windows in, so a VM covers both needs.
- The owner's own Mint machine has no Claude and will not: it is for his final
  test of an installed package, not development.

**What the Linux side needs**, in order:

1. **Stage 1c spike**: the PipeWire topology (a virtual sink, its monitor captured,
   the core, playback to the hardware sink; or PipeWire's own filter hosting),
   decided by a measured -12 dB at 1 kHz, as the Windows spikes were.
2. **The daemon** (stage 3's Linux half): POSIX shared memory for the param block
   and the audio ring, saved state under `$XDG_CONFIG_HOME/isotone`, rate changes,
   the default sink through WirePlumber metadata, a user systemd unit.
3. **CI**: the daemon (and the UI) built on Ubuntu, with a headless PipeWire and
   null sinks for measurement (not yet tried on GitHub's runners).
4. **The UI's Windows layer** (14 files include Windows headers): outputs and their
   notifications from PipeWire, edits to the daemon, the tones into the virtual sink,
   the Devices view (daemon and sink present, no install), the layout picker,
   launch at sign-in as XDG autostart. Wayland limits: global hotkeys go through
   the GlobalShortcuts portal (KDE and recent GNOME), an app cannot keep itself on
   top, and GNOME needs the AppIndicator extension for the tray.
5. **Packaging**: a .deb first (Mint, Ubuntu), Flatpak after its own spike (a
   daemon owning a virtual sink from inside the sandbox).

Windows packaging (stage 6) does not depend on any of this.

## 2026-09-17: Linux, the environment as built

The Mint VM above was not built. A WSL2 Ubuntu 24.04 distro was, and all of items
1 to 3 run on it. The VM is still wanted for item 4, and only for it.

**The premise for choosing VirtualBox was wrong.** "Windows here is 11 Home, so no
Hyper-V" confuses the Hyper-V role and its management tooling, which Home does
lack, with the hypervisor itself, which is already running on this machine for
virtualization-based security:

    HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard
        EnableVirtualizationBasedSecurity   = 1
    HKLM\...\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity
        Enabled                             = 1
    service HvHost                            RUNNING

With VBS on, VirtualBox cannot use its own VT-x hypervisor and falls back to
Hyper-V's platform API, which is slower and historically less reliable. Getting
full-speed VirtualBox would mean turning memory integrity off on the owner's daily
machine, which is not a trade worth making for a build box. WSL2 rides the
hypervisor that is already there. It cost one elevated `wsl --install` and one
reboot.

**Why WSL2 is the better first environment.** Items 1 to 3 (the spike, the daemon,
CI) are headless and CI-verified, so matching CI matters more than matching the
owner's desktop. `ubuntu-latest` resolved to `ubuntu24/20260907.300` on the run of
ced78b6, and this distro is the same Ubuntu 24.04 with the same g++ 13.3.0. The
MinGW GCC 16.1 stand-in in `build-gcc` never matched it: libstdc++ 13's regex once
crashed on input GCC 16 handled, and that failure only ever appeared in CI. There
is also no SSH, no key exchange and no VM networking to keep alive between
sessions; it is driven as `wsl.exe -d Ubuntu-24.04 -e bash -c '...'`.

**The box.** Ubuntu 24.04.5, kernel 6.18.33.2-microsoft, systemd as PID 1 through
`/etc/wsl.conf` (`[boot] systemd=true`), user `jackw` (uid 1000) with passwordless
sudo, and `XDG_RUNTIME_DIR=/run/user/1000`, which PipeWire's user session needs.
g++ 13.3.0, cmake 3.28.3, ninja 1.11.1, libpipewire-0.3 1.0.5, wireplumber 0.4.17,
scipy 1.11.4. The source tree stays on the Windows side at `/mnt/c/...` so there is
one tree; builds go to `~/build-linux` in the Linux filesystem, because compiling
across the 9p mount is slow. Two traps: Git Bash rewrites Linux paths in the
command line, so `MSYS_NO_PATHCONV=1` is needed, and `pactl` lives in
`pulseaudio-utils`, which `pipewire` does not pull in.

**Verified on it.** `core` builds clean with `-DISOTONE_WARNINGS_AS_ERRORS=ON`;
ctest passes core_tests in 5.37 s; `tools/gen_reference.py --check` reproduces the
scipy data to 1.455e-11.

**Measurement needs no audio hardware,** which is the finding that made the VM
unnecessary for items 1 to 3. Two null sinks stand in for the virtual sink and the
hardware sink (`pactl load-module module-null-sink sink_name=isotone_virt`, whose
`isotone_virt.monitor` source appears with it). `pw-play --target isotone_virt`
against `pw-record --target isotone_virt.monitor` round-trips a 1 kHz sine at
amplitude 0.5 at -9.032 dBFS, against -9.03 predicted, peak 1000.13 Hz. That is the
rig stage 1c's -12 dB measurement will use. `pw-record` must be ended with SIGINT,
not SIGKILL, or it never writes the WAV header.

**What WSL2 does not cover.** WSLg provides a rootless Wayland and X11 surface
(`WAYLAND_DISPLAY=wayland-0`, `DISPLAY=:0`, sockets under `/mnt/wslg`), so the Qt UI
will run and can be screenshotted there. What it does not provide is a desktop
environment: no Cinnamon panel or tray, no xdg-desktop-portal GlobalShortcuts
backend, no window manager honouring always-on-top. Every item 4 check that the
Wayland limits above are about still needs the Mint VM, and GNOME and KDE VMs
after it.



## 2026-09-17: Stage 1c complete, the PipeWire topology measured

`linux/spike/` holds the spike: `pw_spike.cpp` (187 lines) hosting
`isotone_core`'s `Processor` in a PipeWire filter node, `measure.py` running the
acceptance measurement, and `10-isotone-spike.conf` declaring the rig's sinks.
Same filter as stage 1b on Windows, peaking 1 kHz -12 dB Q 1, so the two platforms
are measured against the same analytic response:

```
    freq     bypass   filtered   measured   analytic     error
    1000     -6.021    -18.021    -12.000    -12.000    -0.000
     100     -6.021     -6.181     -0.161     -0.161    +0.001
```

Those are the stage 1b figures to three decimals (-12.000 and -0.161). Stage 1's
acceptance criterion is now met on Linux as well as both Windows backends.

**The topology, as the plan's EasyEffects model predicted.** Applications play into
a null sink; its monitor feeds the core; the core's output goes to the hardware
sink:

```
pw-play -> isotone_virt (null sink)
           isotone_virt.monitor -> isotone-spike:in_{FL,FR}
           isotone-spike:out_{FL,FR} -> isotone_hw:playback_{FL,FR}
                                        isotone_hw.monitor -> pw-record
```

**`pw_filter`, not a pair of `pw_stream`s.** One node with two DSP input ports and
two DSP output ports is handed both sides in the same graph cycle, so there is no
ring buffer between capture and playback and no second clock to drift against.
`pw_filter_get_dsp_buffer()` hands back planar float32, one buffer per port, which
is exactly the shape `Processor::process(float* const*, uint32_t)` already takes;
no conversion layer is needed on this platform. The links are made from outside
with `pw-link` rather than in the spike, so the topology under test is visible in
the measurement script.

**Every link is explicit, and that is not incidental.** Three traps cost time here,
all of them the same shape: a tool reporting success while doing something else.

1. `pactl load-module module-null-sink` returns a module id and the sink appears in
   `pactl list short sinks`, but after the daemon has been restarted no PipeWire
   node is created for it. `pw-cli ls Node` showed only WirePlumber's `auto_null`
   while `pactl` listed four sinks. The rig therefore declares its nodes in a
   `pipewire.conf.d` drop-in, which also means they come back with the daemon and a
   run is reproducible.
2. `pw-record --target isotone_hw.monitor` is a Pulse-ism. No PipeWire node carries
   that name, so the target silently fails, the stream falls back to the default
   source, and the capture reads the tone straight off `isotone_virt` with the core
   nowhere in the path. It fails quietly: the first run measured a clean -6.021 dBFS
   in *both* modes, which looks like a working rig and a broken filter. The fix is
   `--target 0` ("do not link") plus an explicit `pw-link`, same as the rest of the
   graph.
3. `pw-record` must be ended with SIGINT. On SIGKILL it never writes the WAV header.

The bypass column is the control, and it earns its place: the run that had the
core out of the path produced identical bypass and filtered numbers, so the
measurement does discriminate between a working chain and a bypassed one.

**What the spike deliberately is not.** No shared-memory transport, no saved state,
no default-sink tracking, and `Processor::initialize()` is still called from the
real-time thread on the first block. That last one is the daemon's first job:
initialize allocates, so a rate or quantum change has to be handled on the main
loop and the processor handed over.

**Build.** The top-level list file adds `linux/spike` only when `pkg_check_modules`
finds `libpipewire-0.3`, so Windows builds, the MinGW stand-in and CI's current
Ubuntu job (which does not install the development package) are all unchanged.
Builds clean under g++ 13.3 with `-DISOTONE_WARNINGS_AS_ERRORS=ON`.


## 2026-09-17: Stage 3's Linux half, the daemon, measured

`linux/` now holds the transport, the daemon and the measurement rig. The stage 1
criterion is met through the daemon on all three paths a state can reach the
processor by:

```
    freq    path       flat   with band   measured   analytic     error
    1000    live     -6.021     -18.021    -12.000    -12.000    -0.000
    1000    cold     -6.021     -18.021    -12.000    -12.000    -0.000
     100    live     -6.021      -6.181     -0.161     -0.161    +0.001
     100    cold     -6.021      -6.181     -0.161     -0.161    +0.001
```

`flat` is the daemon with nothing written, and it measures the input amplitude
exactly, so the pass-through is unity. `live` is `isotone-state set` writing the
shared region while audio plays, the path every UI edit takes. `cold` is
`isotone-state save` writing the file and the daemon then starting fresh and
seeding its region from it, the path a sink takes when it comes up before any UI
runs. `linux/daemon/measure.py`.

**The transport** (`linux/transport/`, `isotone_transport_posix`) is a POSIX
shared-memory object plus the saved-state file. It is thin, 2 source files, because
the layout, the seqlock and the ring are core's and already portable. Two
differences from Windows, both simplifications: a sink is identified by its
PipeWire `node.name` rather than an endpoint GUID, and both ends run as the same
user, so the object is 0600 and needs no security descriptor. The Windows side
needs `kMappingSddl` only because its host is audiodg running as LocalService.

One difference is not a simplification. A POSIX shm object outlives the process
that made it, so unlike the Windows mapping it can be found stale. A daemon killed
between `shm_open` and the header write leaves a region no later run could use, so
`create_or_open` re-initialises an invalid region rather than refusing it;
refusing would wedge that sink until someone deleted the file by hand. A region
that is valid is adopted untouched. `transport_posix_tests` covers both, and all
four of its less obvious behaviours were mutation checked: removing the host-field
zeroing, accepting a relative `XDG_CONFIG_HOME`, dropping the hash suffix that
keeps truncated long names distinct, and removing the stale-region recovery each
make exactly the test that covers it fail.

**The daemon** (`linux/daemon/`) creates everything itself through PipeWire's
`adapter` and `link-factory` factories: the virtual sink applications play into,
the filter node carrying the core, and the four links between them and the
hardware sink. No configuration file drops a sink in and no session manager has to
understand a filter.

That is deliberate. WirePlumber only grew smart-filter placement in 0.5, and
Ubuntu 24.04, which Linux Mint 22 is built on, ships 0.4.17; `grep` over its
shipped configuration finds nothing about smart filters. Relying on that feature
would have cut off the owner's own distribution.

`Processor::initialize()` allocates, so it never runs on the audio thread. The
processor is sized for 8192 frames up front, and a graph shape it cannot serve
makes `on_process` pass the audio through untouched and hand the work to the main
loop through `pw_loop_invoke`, one request at a time so a stalled main loop cannot
queue thousands.

**`isotone-state`** is what `isotone-shm` is on Windows: `show`, `set` and `save`
against a sink's live region and its saved file. It links no PipeWire, so it can
inspect a sink with no daemon running.

**CI** gains a `linux-host` job. It installs `libpipewire-0.3-dev`, builds with
`-DISOTONE_WARNINGS_AS_ERRORS=ON`, runs ctest, and then runs both acceptance
measurements through a real PipeWire graph with `linux/ci-audio.sh`, which starts
its own PipeWire, WirePlumber and D-Bus session in a scratch runtime directory.
Both ends of the chain are null sinks, so no audio hardware is involved. The job
asserts the three Linux binaries exist before it measures: without the development
package the top-level list file skips `linux/` entirely and every other step would
still pass.

The whole sequence was run from a clean build on Ubuntu 24.04 with g++ 13.3, the
same compiler `ubuntu-latest` resolves to. It has not run on a GitHub runner yet.

**Three findings, all the same shape as stage 1c's: a tool reporting success
while doing something else.**

1. **An adapter node has no ports until a session manager configures it.** Started
   with PipeWire alone, the declared null sinks appear in `pw-cli ls Node` with
   correct names and `media.class`, and `pw-link` lists no ports for them at all,
   which looks exactly like PipeWire having ignored the configuration. The ports
   arrive when something sets `PortConfig mode=dsp`, so the headless rig has to run
   WirePlumber even though it needs none of its policy.
2. **`node.always-process` is not what creates those ports.** It was tried first on
   that theory and changed nothing. It is kept for a different and real reason:
   it stops a null sink suspending between measurements, which would drop its
   ports and make a run flaky.
3. **`pw-play --target <sink>` leans on session-manager policy.** Every link in the
   rig is now made explicitly, playback included, so the measurement depends on no
   policy at all. An unlinked stream is simply not scheduled, so nothing of the
   file is lost between starting it and linking it.

**What is not done, and is not pretended to be.** The daemon does not yet follow
the default sink through WirePlumber metadata; `--sink` is given to it. It creates
the audio ring in the region but never writes to it, so the UI's spectrum has
nothing to read yet. Channel count and speaker mask are fixed at stereo and 0. The
rate-change handover is implemented and reviewed but has not been exercised by an
actual rate change. The systemd user unit is written and parses, but has never been
installed, because that wants a package (stage 6's Linux half) rather than a
hand-copied file.


## 2026-09-17: The Linux daemon finished to the edge of stage 4

Everything the previous entry listed as not done is done, and each piece is
measured through real audio rather than inspected. `linux/daemon/measure.py`:

```
      case    freq       flat   with band   measured   analytic     error
      live    1000     -6.021     -18.021    -12.000    -12.000    -0.000
      cold    1000     -6.021     -18.021    -12.000    -12.000    -0.000
      ring    1000                -18.021               vs sink    +0.000
      live     100     -6.021      -6.181     -0.161     -0.161    +0.001
      cold     100     -6.021      -6.181     -0.161     -0.161    +0.001
     44100    1000     -6.021     -18.021    -12.000    -12.000    +0.000
       5.1    1000     -6.021     -18.021    -12.000    -12.000    -0.000
```

**The audio ring.** The daemon writes the post-EQ audio into the region's ring,
which is what the UI's spectrum drains. The writer takes interleaved frames and a
filter hands over planar ones, so the audio thread interleaves into a buffer
sized on the main loop with everything else, and the ring is claimed, given its
channel count and prefaulted there too, exactly as `IsoApo::LockForProcess` does
it. `isotone-state capture` reads it back, the counterpart of `isotone-shm
capture`. The `ring` row above is that readback against what reached the sink:
the same audio, so the same figure, and it agrees to +0.000 dB.

**Following the default sink.** Without `--sink`, the daemon binds WirePlumber's
`default` metadata and moves with `default.audio.sink`: the output links, the
shared region and the saved state all follow together, and the processor is
re-sized because the new sink may run at another rate.

It never targets its own virtual sink. That is not a corner case but the ordinary
state: making Isotone the default is how applications come to play into it, and
feeding it back to itself would be a loop, since its monitor is the core's input.
Told to do that, the daemon keeps feeding whatever real sink it already had.

The limit that leaves: a daemon started while Isotone is *already* the default
has nothing to feed until a real sink is made default once. `--sink` is the way
round it, and the honest fix is for the UI to set the target, which is stage 4's
job rather than a guess made here about which sink the owner meant.

**A rate change, exercised rather than assumed.** `pw-metadata -n settings 0
clock.force-rate 44100` mid-stream: the audio thread finds a shape it cannot
serve, passes that block through untouched, and asks the main loop to re-size.
The published rate follows 48000 to 44100 and back, the heartbeat keeps climbing
across both changes (144, 251, 309), and the measurement above still reads
-12.000 at the forced rate.

**Channels past stereo.** A layout table maps PipeWire's channel names to the
speaker bits the core addresses bands, routing and bass management by, and drives
three things at once: the `audio.position` the virtual sink is created with, the
DSP port names, and the `speaker_mask` given to `remap_channels` and published in
the header. `--channels 1, 2, 4, 6 or 8`. The `5.1` row is a six-channel core
into a six-channel sink.

Ports are matched by speaker name, never by position in a sorted list. A 5.1 sink
sorts `playback_FC` before `playback_FL`, so taking the first two would quietly
send the left channel to the centre speaker; the same trap was in the measurement
rig and is fixed there too. A target that does not carry one of our positions
does not get that channel and the daemon says so ("isotone_hw carries 2 of our 6
channels"), which is how a 5.1 core feeding a stereo sink still plays its front
pair rather than nothing.

**One measurement bug worth recording,** because it looked like a daemon fault.
The ring first read 0.600 dB low. The capture runs longer than the tone and the
sink keeps running in between, so the ring holds silence at both ends, and
projecting onto the reference phasor across that pulls the level down. The window
has to find the signal first, which is what the sink's own capture already did.
Nothing was wrong with the ring.

**Still not done**, and all of it is stage 4 or later: the UI's Windows layer,
packaging, and a daemon that is told its target by the UI rather than by a flag
or the session manager.


## 2026-09-18: CI on a real runner, and what stage 4 actually costs

**The linux-host job is green on a GitHub runner**, which was the last thing
claimed but unverified. PipeWire 1.0.5 on `ubuntu24`, and the same figures as
this machine to three decimals:

```
=== stage 1c: the topology spike ===
    1000     -6.021    -18.021    -12.000    -12.000    -0.000
     100     -6.021     -6.181     -0.161     -0.161    +0.001
=== stage 3: the daemon ===
      live    1000     -6.021     -18.021    -12.000    -12.000    -0.000
      cold    1000     -6.021     -18.021    -12.000    -12.000    -0.000
      ring    1000                -18.021               vs sink    +0.000
     44100    1000     -6.021     -18.021    -12.000    -12.000    +0.000
       5.1    1000     -6.021     -18.021    -12.000    -12.000    -0.000
```

It took one fix. The job runs `bash linux/ci-audio.sh`, and the script re-execs
itself under a private session bus as `"$0"`; a checkout carries no executable
bit, so `dbus-run-session` could not exec it and stopped with "Permission
denied". It goes through `bash` now. Everything before that step had already
passed on the runner.

**A pre-existing flake, surfaced not fixed.** The same push showed
`core (windows-latest)` failing `compat_tests` with ERROR_SHARING_VIOLATION (32),
"cannot write Isotone.txt: the process cannot access the file because it is being
used by another process", in the two tests that run writers concurrently. It
passed on the next run with the same code, and the three runs before it were
green, so it is intermittent. Nothing in the Linux work touches `windows/compat`.

`CompatWriter::write` takes a lock file with a 1000 ms wait and then calls
`write_file_atomically`, whose retry is 200 ms by default. Two threads at 300
rounds each, plus a separate `isotone-compat` process, is enough to exceed one of
those on a slow contended runner. Whether the answer is a longer retry in the
product, a gentler test, or leaving it, is a decision about how Isotone should
behave when something else holds Equalizer APO's config, so it is the owner's.

**Wayland: an app cannot keep itself on top, and that is accepted** (owner,
2026-09-18). He can only test X11 on real hardware for now; Wayland desktops get
tested in VMs.

**Stage 4, surveyed by building rather than by guessing.** The UI was configured
against Ubuntu 24.04's Qt on the WSL box, with the `WIN32 AND MSVC` gate and the
version floor relaxed, purely to see what breaks. What it found, in the order it
found it:

1. **Qt 6.4.2 has every module the UI asks for.** `find_package(Qt6 COMPONENTS
   Gui Network Qml Quick QuickTest Test Widgets)` succeeds. The project pins 6.11
   on Windows, and the 7-version gap had looked like the first problem; it is not.
   That matters for packaging too, because Mint 22 is Ubuntu 24.04: a .deb may be
   able to use the distribution's Qt rather than bundling one. Whether the UI's
   *code* needs anything past 6.4 is still unknown, because the build does not get
   that far yet.
2. **One CMake incompatibility, trivial.** 6.4's `qt_add_qml_module` requires a
   `VERSION`; 6.11 made it optional. Adding `VERSION 1.0` is accepted by both.
3. **The real coupling is four Windows libraries, not the Windows headers.**
   `isotone_ui_backend` links `isotone_transport`, `isotone_compat` and
   `isotone_devices`; `isotone_ui` links `isotone_devicetool_session` and compiles
   in `$<TARGET_FILE:isotone-devicetool>` as a path. Configure fails on that
   generator expression long before a single source file is compiled. Those either
   gain Linux counterparts (the transport already has one) or are compiled out
   behind the platform seam.

So the order for stage 4 is: the platform seam in `ui/CMakeLists.txt` first, then
`DeviceLink`'s `std::wstring` identity, then the backends themselves. The 18
source files that include `windows.h` are the last and most mechanical part, not
the first.


## 2026-09-18: Stage 4 begins, the platform seam in the UI's build

The first piece of the UI builds on Linux: `isotone_ui_backend`, with GCC 13.3 and
`-DISOTONE_WARNINGS_AS_ERRORS=ON`.

**Which files are portable was settled by compiling them, not by reading their
includes.** Each `ui/backend/*.cpp` was compiled on its own against Linux. Five
build (`output_state`, `spectrum`, `typed_value`, `pink_noise`, `sine_tone`) and
six do not (`devicelink`, `speaker_setup`, `test_tone`, `config_attach`,
`diagnostics`, `startup_registration`). The six are now a platform list in
`ui/CMakeLists.txt`, and Linux takes `isotone_transport_posix` where Windows takes
`isotone_transport`, `isotone_compat` and `isotone_devices`.

`speaker_setup` is the near miss, and it says where the work is. Nothing in it
touches Win32 except `save_speaker_setup`, whose path is a `std::wstring` going
into `windows/transport`'s `persisted_state.h`. The wide-string identity leaks
into code that is otherwise portable. That seam, not `windows.h`, is what stage 4
costs.

The Qt layer, `isotone_ui`, stays Windows-only for now: its models reach the
device APIs, the devicetool and the compat writer directly, and the file returns
early on other platforms rather than pretending.

**The Qt floor is 6.4, not 6.11**, because Ubuntu 24.04 ships 6.4.2 and Mint 22 is
built on it. Holding out for 6.11 would mean a .deb that cannot use the
distribution's Qt. All eight components the UI asks for exist in 6.4.2. Whether
the Qt layer's *code* needs anything newer is still unknown; Windows continues to
build against 6.11.

**A trap that cost a red suite.** Lowering the floor, `qt_standard_project_setup`
was lowered with it, to `REQUIRES 6.4`. That is not a floor: it is the policy
level, and setting it below the Qt actually in use changes what
`qt_add_qml_module` does. On Windows with Qt 6.11 present, the module's types
stopped registering: all 290 QML tests failed with "DefaultOutputFollower is not a
type" and the app exited at startup, while `ui_tests` and `ui_model_tests` stayed
green, so nothing in C++ pointed at it. It is now
`qt_standard_project_setup(REQUIRES ${Qt6_VERSION})`: the newest policies the
installed Qt knows. Windows is back to 45943, 2002 and 290.

The other 6.4 difference found while probing: `qt_add_qml_module` requires a
`VERSION` there and 6.11 made it optional. Not needed yet, since the Qt layer does
not build on Linux, but it is one line when it does.


## 2026-09-18: The Linux host reviewed, and what the review found

A review of everything added this session. Fifteen findings, four of them serious,
and the ones checked by hand against the code were all real. Worth recording that
the measurements did not catch any of them: every figure was green throughout,
because measurement proves the path where nothing goes wrong and says nothing
about shutdown, retargeting, or a device going away.

**The ring was claimed once and never again.** `audio_ring.h` says plainly of
`claim`: "Call it again on later process calls while it fails", because a ring
another instance holds is only taken over after its write index has stood still.
`IsoApo::APOProcess` retries from its process loop; the daemon claimed once in
`do_initialize` and gave up. A daemon that is killed leaves a live-looking claim,
so the next run's spectrum was dead for its whole life. `host_publish_format` was
nested inside the successful claim as well, so the UI would not have learned the
rate or channel count either. The claim is now retried from the audio thread and
the format is published regardless.

Verified: kill a daemon mid-stream with `--keep-region`, start another, and read
the ring. -18.021 dBFS with the fix, "no audio in the ring" with it reverted.
`linux/daemon/measure.py` has it as the `stale` case.

**The region was unmapped from under the audio thread.** `close_region` did
`munmap` with no handshake, and `on_process` reads that memory. Both callers hit
it: `set_target` closed the region before clearing `ready`, and teardown closed it
four lines before destroying the filter, which is still scheduled. Switching the
default sink while audio played, or SIGTERM, was a read of freed memory on the
data thread. There is now a `quiesce()`: clear `ready`, then wait for the flag
`on_process` holds across its body, the two ordered against each other so a call
that starts late returns before touching the region and one already inside is
waited for. Teardown destroys the filter first.

**The service unit could not start.** `ExecStart=... --sink ${ISOTONE_SINK}` with
the variable set only by an optional drop-in: systemd expands an unset variable to
nothing, leaving `--sink` with no value, which exits 2, which `Restart=on-failure`
turns into a restart loop. It now takes no arguments and follows the default sink,
and the drop-in replaces the command instead.

**A fixed target was terminal.** `on_global_remove` cleared `target` when the fed
node went away, but only the metadata listener ever sets one and that is bound
only when following. With `--sink`, one unplug left the daemon silent for good.
The name is now kept when it was given explicitly.

The rest, in short: links are made per channel and retried, so a port that has not
reached the registry yet no longer leaves that channel silent for the run, and a
failure part way through no longer double-links a channel on the next event (which
would have been about 6 dB hot); link proxies are destroyed rather than dropped;
the metadata proxy is released so a WirePlumber restart does not end default
following; a refused `pw_loop_invoke` no longer wedges the daemon into permanent
passthrough; the shm create/open race that `systemctl --user restart` walks into
retries instead of failing the start; a failed start no longer leaves its region
behind; `write_persisted_state` no longer reports a save that happened as a
failure when the directory cannot be fsynced, nor success when the write was
short; and an informational pipeline in `ci-audio.sh` can no longer end the run
under `pipefail` before any measurement.

All measurements still pass, including the new `stale` case, from a clean build.


## 2026-09-18: The identity goes narrow

`OutputTarget::guid` is a `std::string`. It is the one thing every backend has to
agree on: an endpoint GUID as "{lower-case}" on Windows, a PipeWire `node.name` on
Linux. A canonical GUID is ASCII, so nothing is lost, and the Windows backend
widens at its own edge through `widen_id` and `narrow_id`.

The identity is narrow all the way through the model now, not converted at each
use: `Outputs::Output::guid`, `default_guid_`, `current_guid_`, `selectGuid`,
`Speakers::guid_`, `EqByEar::stream_guid_` and `ImportPreview`'s. Wide strings
survive only where they are genuinely Win32's: device and connection names, the
devicetool's arguments, `SpeakerStore`'s keys, and the calls into
`isotone::devices` and `isotone::win`.

**The header count was a bad estimate of the work.** 31 `std::wstring` in `ui/src`
headers suggested a wide refactor; the first build gave five errors, all in
`devicelink.cpp`, because the Qt layer carries the identity as a `QString`. It
then grew to 34 and 40 as each translation unit got its turn, and settled at
around 90 sites, nearly all one-line conversions at a boundary. The compiler found
every one.

Two of them were mine, from sweeping with a regular expression: `e.guid` in
`outputs.cpp` and `devicestatus.cpp` is the *devices* layer's endpoint, which is
legitimately wide, and a pattern that matched "anything ending in .guid" narrowed
it. Both were caught by the same build. A regular expression does not know which
layer a name belongs to, and the two layers here spell the field the same way.

`ui_tests` 45943, `ui_model_tests` 2002, `ui_qml_tests` 290: the same counts as
before the change, which is what says the behaviour did not move. Linux still
builds and its tests pass.

What this unblocks is the point: a Linux `DeviceLink` can now name a sink without
pretending it has a GUID. Still Windows-only in the header: the `DWORD` returns,
the `Backend` enum's `equalizer_apo`, and the region namespace and compat
directory the constructor takes. Those are the next seam, and they only matter
when the Linux backend is written.


## 2026-09-18: Where a Linux output's edits go

`ui/backend/devicelink_posix.cpp`: the Linux half of `DeviceLink`. It is a third
the length of the Windows one, because there is one engine rather than two. The
daemon owns the sink's region, every edit is a seqlock write into it, and the
daemon already writes the post-EQ audio into the region's ring, so there is no
Equalizer APO worker thread and no loopback capture to have a counterpart.

`DeviceLink`'s public API is platform-neutral now. `DWORD` became `LinkError`, a
`uint32_t` that is 0 for success, a Win32 code on Windows and an errno on Linux;
that was nearly free, because `apply` and `commit` had their results ignored at
every call site and only `last_compat_error` is ever shown. `saved_state_path`
returns a `std::filesystem::path`. `Backend` gains `pipewire`. The constructor and
the private members are what stay per platform, because the two sides share
almost nothing below the surface: `#if defined(_WIN32)` around them, and two
translation units.

The behaviour the tests hold it to, with the test playing the daemon rather than
running one: a commit reaches the region a daemon would read and leaves the
daemon's header alone, so the format it published survives an edit; no daemon
means no region and an edit says so; `save` writes the file and treats "no daemon
to tell" as success, because the file is what the sink will start from;
`load_current` prefers the region and falls back to the file; `read_audio` drains
the ring the daemon writes, at the rate the daemon published; and only a
`pipewire` output gets a region at all. Six cases, 32 assertions, and four
mutations each fail exactly the test that covers them.

CI's linux-host job now installs Qt and configures with `-DISOTONE_BUILD_UI=ON`,
so this runs there too, and asserts the backend was built for the same reason it
asserts the daemon was: without Qt the whole `ui` directory is skipped and every
other step still passes.

Two things worth keeping. `region_open()` was defined inline in the header against
`mapping_`, which does not exist on Linux; it is a declaration now, defined on
each side. And a comment line ending in a backslash continues onto the next one:
the line describing the `Global\` namespace swallowed the one under it, which MSVC
never minded and GCC treats as an error under `-Werror`.

Windows unchanged: 45943, 2002, 290.


## 2026-09-18: The outputs a Linux machine has

`ui/backend/pipewire_outputs.{h,cpp}`: what `isotone::devices` does with
`IMMDeviceEnumerator` on Windows, done with PipeWire's registry. No Qt, so it can
be tested without one, and it is what `Outputs` will sit on when the Qt layer is
ported.

It reports each `Audio/Sink` by `node.name` (the identity an `OutputTarget`
carries and the region is named for, stable across restarts unlike the object id)
and `node.description` (what a person is shown, falling back to the name), marks
Isotone's own virtual sink so nothing offers it as something to feed, and follows
`default.audio.sink` from the same WirePlumber metadata the daemon follows.

A `pw_thread_loop` of its own, because the UI thread must not block on PipeWire
and PipeWire's loop wants to own its thread. Everything a caller reads is a
snapshot taken under a lock. The change callback runs on the PipeWire thread, so
a UI hands it to its own loop rather than touching widgets from it.

`wait_ready()` waits for a `pw_core_sync` to be answered, not merely for the
registry to start reporting: there is no other way to know that everything
already there has arrived. A caller listing outputs once therefore does not poll.

**The test found a real bug in it.** Stopping and starting again left `ready`
true from the first run, so `wait_ready()` returned at once and the caller read an
empty list. `start()` clears it now. That is the case that would have shown up as
"the outputs list is empty after the audio server restarts", which is exactly the
kind of thing that is hard to find later.

Nine cases, 51 assertions, each skipped rather than failed where there is no
PipeWire or the rig's sinks are not declared, so the suite still passes on a
machine without a sound server.

Windows unchanged: 45943, 2002, 290. Linux: three test binaries green from a clean
build, and both measurements still pass.

**What still needs a desktop, and so a VM:** the tray, the GlobalShortcuts portal,
and window behaviour (an app cannot keep itself on top under Wayland, which the
owner has accepted). Everything before that is compiler work: the Devices view
with no install concept, the tones into the virtual sink, XDG autostart in place
of the Run key, and then the Qt layer itself.


## 2026-09-18: Launch at sign-in on Linux

`ui/backend/autostart_xdg.{h,cpp}`: a `.desktop` file under
`$XDG_CONFIG_HOME/autostart`, which GNOME, KDE and Cinnamon all read. The
counterpart of the value under HKCU's Run key.

The file is the whole of the state: present means on, absent means off. There is
no equivalent of Windows' StartupApproved, so a desktop that lets a person switch
an entry off writes `Hidden=true` or `X-GNOME-Autostart-enabled=false` into the
file rather than deleting it, and both are read back as off. Neither `OnlyShowIn`
nor `NotShowIn` is written, so the entry is not pinned to one desktop.

`Exec` is refused rather than written when the command holds a newline: a desktop
entry value ends at the newline, so one in the path would not make a longer
command, it would write a second key. The file is written whole and renamed, so a
desktop reading the directory at the wrong moment never sees half an entry.

Fifteen cases across the Linux backend now, 75 assertions. Four mutations of this
file each fail the test that covers them: accepting a newline in the command,
ignoring `Hidden=true`, accepting a relative `XDG_CONFIG_HOME`, and pinning the
entry to one desktop.

One note on the mutation checks themselves. The newline mutation first reported
"still passes", which would have meant the test was worthless. It was the `sed`
that failed to match, not the test: applied by line number instead, it fails two
assertions. A mutation that does not change the binary proves nothing, so a
mutation check has to show the mutation actually landed.

## 2026-09-19: The Qt layer on Linux

The whole UI builds and runs on Linux now, against the distribution's Qt 6.4.2
(Ubuntu 24.04, Linux Mint 22), and Windows builds from the same sources against
6.11. Commits 3c8e71c to 7daf8ab.

**How it is split.** The QML is shared, with `Qt.platform.os` branching where a
screen is Windows' alone. The C++ is shared where it was portable, and the rest
is either `#if defined(_WIN32)` inside the file (where most of it is shared:
`outputs.cpp`, `about.cpp`, `startup.cpp`, `speakers.cpp`, `main.cpp`) or a
second translation unit where the two sides share nothing below the header
(`devicesmodel_posix.cpp`, `devicetool_posix.{h,cpp}`,
`globalhotkeys_posix.cpp`, `test_tone_posix.cpp`), as `devicelink_posix.cpp`
already did.

What each piece is on Linux:

| Windows | Linux |
|---|---|
| render endpoints with IsoAPO or Equalizer APO | every PipeWire sink but Isotone's own (`pipewire_outputs`) |
| the engine probe for the status dot | the daemon's heartbeat in the fed sink's region, compared between 3 s probes |
| Devices: devicetool, install, repair | Devices: each sink fed, standby or daemon stopped; Start runs `systemctl --user start isotone-daemon` |
| tones by WASAPI on the endpoint | a PipeWire stream into Isotone's sink, only while the daemon feeds the output asked for |
| RegisterHotKey | X11 key grabs on an X session, the GlobalShortcuts portal on Wayland |
| the Run key | an XDG autostart entry |
| `%APPDATA%\Isotone` | `$XDG_CONFIG_HOME/isotone/ui` |
| About: IsoAPO, Equalizer APO, protected audio | About: the daemon and the PipeWire library |
| Settings, Outputs; first run | none: nothing is chosen per output and nothing is installed |

**What Qt 6.4 lacks, and how each is bridged where it is used:**

- `QQmlEngine::singletonInstance(uri, name)` is 6.5. 6.4 looks C++ singletons up
  by type id; a singleton defined in QML (UiState) has none there, so it is read
  through Main.qml's own imports (`qmlsingleton.h`).
- `qrc:/qt/qml` became a default import path in 6.5. Without it 6.4 never reads
  the module's qmldir, the singletons are plain types, and every `Theme.x` is
  undefined: the whole UI renders without colours. `main.cpp` and the QML test
  runner add it.
- The resource prefix: 6.4 puts the module at `/`, so `RESOURCE_PREFIX /qt/qml`
  is written out.
- `Qt.styleHints.colorScheme` is 6.5: System reads the palette's window colour
  instead. `Shape.CurveRenderer` is 6.6: the geometry renderer with 4x
  multisampling. `PathRectangle` is 6.8: the drop outline is a PathSvg.
  `QFont::setFeature` (tabular figures) is 6.7: 6.4 draws proportional figures.
- A QML list property is not iterable with `for...of` before 6.5 (tests only).
- A subclass of a QML singleton is a singleton in 6.4 too: PreviewSession, the
  Appearance page's preview, could not be created, so Settings, Appearance did
  not load at all. It shadows the marker.

One false lead worth recording: a startup hang looked like 6.4's ahead-of-time
compiled bindings looping on a type lookup, and bytecode-only compilation was
put in. It was the missing import path. With the path added, the compiled
bindings run, and the workaround came out again.

**The build machine.** The Mint VM builds the tree in about 16 s, but it is not
a reliable place to measure audio (see "The Mint VM, and why it is slow"
below). Levels are measured in the WSL box under its PipeWire, as before; the VM
is for the desktop.


## 2026-09-19: The Linux app measured through the daemon

`ui/tests/measure_linux.py` runs the app itself, offscreen, on the rig's sink
and measures what the daemon plays, so the Qt layer's own path is in the chain.
It runs in CI after the daemon's measurement:

```
      case    freq       flat   with band   measured   analytic     error
       app    1000     -6.021     -18.021    -12.000    -12.000    +0.000
       app     100     -6.021      -6.102     -0.081     -0.082    +0.001
     saved    1000     -6.021     -18.021    -12.000    -12.000    +0.000
      tone    1000    -30.001     -42.012    -12.011    -12.000    -0.011
```

`app` is Add band into the live region; `saved` is the file the app writes,
read by a daemon started afterwards; `tone` is EQ by ear's own stream at its
-30 dBFS, through the same band. Two check flags were added for it,
`--save-output` and `--ear-tone <hz>`.

Getting the tone case to pass found two bugs:

- `PipewireOutputs::wait_ready()` returned before the default sink was known.
  The metadata that holds it is bound during the registry sweep and its
  properties arrive after the sweep is answered, so the app saw the default
  "change" moments after starting, and Switch preset when the default output
  changes moved the session off the output it had been opened on. A second sync
  once the metadata is bound; the new test failed 5 of 5 before it.
- The tone's hand-iterated PipeWire loop never connected from inside the app,
  though the same code did from a bare test program. `pw_main_loop_run` with a
  timer that quits on stop does.


## 2026-09-19: Applications captured by the daemon

**A decision of mine, for the owner to confirm.** Until now the EQ applied only
to what played into Isotone's sink, so a user had to make that sink the default,
and then changing output meant going round the desktop's settings twice. The
daemon now does what EasyEffects does: it gives every application's playback
stream a `target.object` of Isotone's sink in the default metadata as the
stream appears, which is exactly what moving it in pavucontrol writes. The
default sink stays the hardware the desktop shows, the daemon follows it as it
already did, and so choosing another output in the desktop's sound settings
moves the EQ with it. That is how an output is chosen on Windows too.

The rules: a stream that names its own target is left alone (pw-play
`--target`, Isotone's tones, the measurement rig); one the user moves elsewhere
later is theirs, and the daemon forgets it; the targets it set are cleared on
exit, with a round trip so the server has them, and the streams go back to the
hardware. After a WirePlumber restart every current stream is captured again
(written for, not yet exercised).
`--leave-streams` turns it all off.

Measured, in `linux/daemon/measure.py`:

```
   capture    1000     -6.021     -18.021    -12.000    -12.000    -0.000
     leave    1000     -6.021      -6.021      0.000      0.000    +0.000
   release  stream into isotone: yes, back to isotone_hw after exit: yes
```

`capture` is an application playing with no target to the default sink, which
is the hardware; `leave` is the same with `--leave-streams`, which is what shows
the capture and nothing else put the EQ in the path. In the Cinnamon VM an
application's audio followed `pactl set-default-sink` from one sink to another
and the app's sidebar and spectrum followed it.

The one visible difference from Windows: a stream is moved a moment after it
appears, so its first few milliseconds can reach the hardware unequalized.
EasyEffects has the same.


## 2026-09-19: The UI's suites on Linux, and what they found

`ui_tests`, `ui_model_tests` and `ui_qml_tests` build and run on Linux, in ctest
and in CI's linux-host job. `ui/tests/test_rig.h` gives the model tests an
engine region and saved state of their own on either platform, where they had
Windows' Local\ namespace written into them. Linux: `ui_tests` 40 cases,
`ui_model_tests` 91 cases and 1433 assertions, `ui_qml_tests` 232 passed with
49 skipped (Devices, its pill, first run and Settings, Outputs, which test
devicetool and Equalizer APO). Two more ctest entries run the global hotkeys
against real servers: `ui_model_tests_x11` under Xvfb, and
`ui_model_tests_portal` against `ui/tests/mock_portal.py` on a private session
bus.

What running them there found, all fixed, each with a test that failed first:

- **The GlobalShortcuts portal's presses never reached the app.** QtDBus
  refused the typed slot for the Activated signal (connect returned false) and
  said nothing, so on a Wayland desktop a bound shortcut would have done
  nothing. The slot takes the whole message. With the typed slot put back, the
  portal test fails at the press.
- **A region never written read back crossover 0 Hz and LFE low-pass 0 Hz**, on
  both platforms: `init_param_block` left them zero, where a default state has
  80 and 120. A fresh output's Speakers view showed 0 Hz, and turning bass
  management on from there would have used it. They are the defaults now. This
  is in core, so IsoAPO gets it with its next build; audio is unchanged, since a
  never-written region only ever plays with bass management off.
- **The launch-at-sign-in entry wrote Exec unquoted**, so a path with a space or
  another reserved character (an AppImage in "My Apps") would never start. It
  follows the desktop entry specification's quoting now.
- **Presets sorted "Preset 10" before "Preset 9" under the C locale**, where Qt
  drops QCollator's numeric mode. C.UTF-8 is what a bare session, a container or
  a CI runner has. English collation is used there.
- **Releasing X11 grabs was a flush, not a round trip**, so another client could
  be refused the keys a moment after Isotone let them go.
- An unsequenced `id++` in a preset test's arguments, which MSVC and GCC order
  differently; and a dead helper MSVC never warned about.

Two limits, not fixed:

- F13 and up have no keycode in a standard X keymap, so a global shortcut on
  them cannot be grabbed on X11; the Shortcuts page shows it In use.
- A Wayland desktop without the GlobalShortcuts portal (Cinnamon on Wayland,
  older GNOME) marks every Global shortcut In use too, which is true in effect
  but not in wording.


## 2026-09-19: The desktop checks, on Cinnamon under X11

In the Mint VM, on the owner's desktop, by hand and with screenshots:

- The tray icon appears (Cinnamon's StatusNotifierWatcher), with its tooltip
  ("Isotone", then output and preset) and its menu: EQ and Mute with their keys,
  the Output and Preset submenus, Open Isotone, Quit.
- A global Ctrl+E pressed through the X server with another window focused
  turned EQ off, and the daemon's region read bypassed; pressed again, back.
- Closing the window leaves the app in the tray; a second launch exits 0 and
  raises and activates the running window, one process throughout.
- Always on top sets `_NET_WM_STATE_ABOVE` and the window stays over another
  that is activated; switched off in Settings it clears at once, the window
  still mapped.
- Launch at sign-in writes the entry and removes it; across a reboot the VM
  signed in and Isotone started with `--tray`, no window, in the tray.
- With the daemon stopped the top bar shows Daemon stopped and Start; Start in
  Devices ran the user unit, and the lists followed as sinks appeared.
- Import opened the desktop's own GTK file chooser; the file was read, the
  preset made and loaded, and the region and saved state held its bands.

**Not checked live: GNOME and KDE, and Wayland.** There is no VM for them yet
(docs/notes/linux-vm-setup.md lists them). The portal is checked against the
mock, which speaks the protocol as xdg-desktop-portal documents it, but not
against Plasma's or GNOME's own.


## 2026-09-19: Linux speaker layouts

**A decision of mine, for the owner to confirm.** On Windows an output's layout
is its endpoint's format, and the Speakers view offers the picker only for
outputs with more than two channels; a stereo endpoint is switched in Windows'
own sound settings. On Linux the layout is that of Isotone's own sink, one for
every output, and there are no system settings for it. So it is chosen in
Settings, General, Speakers, Layout (Linux only), with the same confirmation
dialog. Showing the Speakers view for stereo was tried first and dropped: the
view is not built for two channels (an empty speaker table, 0 Hz rows).

A choice writes `$XDG_CONFIG_HOME/isotone/daemon.conf` (`channels=N`,
`linux/transport/daemon_config.h`, read by both sides), which the daemon reads
when no `--channels` is given, and restarts the user service. The daemon gains
2.1 (FL FR LFE). The sink's streams are captured again when it comes back.

Measured: a daemon left to find its count in daemon.conf, at 2.1 into the rig's
5.1 sink, -12.000 at 1 kHz against -12.000. In the VM: Stereo to 5.1 to 7.1, the
daemon restarted each time, and the sidebar, Settings and the Speakers view
followed without restarting the app; 7.1's Speakers view played a speaker's
test tone into the sink.

Two things this found: the daemon published its layout only with the first
processed block, so a suspended sink read as 0 channels and the UI took it for
stereo (it is published when the region opens now); and Outputs' probe did
nothing while no output was fed, and did not notice the fed one coming back in
another layout.

What the layout does not do: a 7.1 core feeding a stereo sink plays its front
pair and drops the rest, with no downmix, as the daemon always has with a sink
that lacks a position.


## 2026-09-19: The Mint VM, and why it is slow

The owner asked whether the VM could be given more. What was measured:

- VirtualBox runs it through Hyper-V's platform API (the log: "AMD-V is not
  available", NEM), because memory integrity is on. That was expected.
- The kernel reports soft lockups: "CPU#1 stuck for 331s" once, then every
  minute or so "stuck for 46s", on idle CPUs as well. The guest's clock stalls
  and jumps; a timing loop read 783 million clock reads a second, which no
  Python loop does.
- The same `core_tests` took 55 s in the VM and 5.3 s in WSL at one moment, and
  6 s in the VM after a reboot: the slowness comes and goes with the stalls, not
  with a steady cost per instruction.
- Four CPUs instead of eight, and 3D acceleration off: the lockups continued
  (put back to the owner's eight and 3D on afterwards).
  The Hyper-V paravirtualization interface: the guest detected no hypervisor
  clock at all and fell back to the PM timer, which read wrong by a factor of
  eight. Reverted to KVM, the default.

So nothing inside VirtualBox's settings fixes it. What would: memory integrity
off (declined, 2026-09-18), or a Linux machine that is not a guest of this one.
For what the VM is used for, the desktop checks, it is good enough; audio
levels are measured in WSL, where they are exact.

Changed in the VM and put back: the sound card profile (turned off while
testing so the rig's null sinks were the only sinks), the layout (back to
stereo), launch at sign-in (off). Left in place: the build tools, Qt, the dev
packages, the daemon's user unit pointing at `~/build`, `~/desk.sh` (runs a
command in the logged-in session) and `~/rig.sh` (the two null sinks).


## 2026-09-19: Intermittents seen, not diagnosed

- `compat_tests` on the Windows runner failed with ERROR_SHARING_VIOLATION (32)
  in 3 of the last 4 pushes, in the two concurrent-writer tests. The same flake
  as "CI on a real runner" (2026-09-18), now much more often. Nothing here
  touches `windows/compat`. The decision it waits on is still the owner's.
- The stage 1c spike's 100 Hz case read 6.2 dB low once on a runner (534deb2);
  the next push passed. The daemon's `stale` case read 1.18 dB low once in seven
  local runs, the first of a sitting. Both look like dropped audio in the
  capture window rather than a wrong filter, since the ring read correctly while
  the sink read low. Not diagnosed.


## 2026-09-19: The intermittents, diagnosed and fixed

The owner asked for them fixed. Three separate causes, one of them a bug in
the daemon that the loaded runs turned up.

**The compat writers' lock starved a waiter.** A throwaway branch logged which
call failed on the Windows runner, 25 runs of the two concurrent-writer tests
on each of four runners: every failure (6 of 100 runs) was the lock timing out
after 1000 ms, never the replace or a read. The lock was an exclusive open of
`Isotone.txt.lock`, polled with `Sleep(1)`. A writer persisting back to back
holds it for a few milliseconds and leaves it free for microseconds, and a
waiter that wakes on the runner's 15.6 ms timer tick can miss every gap for a
second. Here `Sleep(1)` is 1.4 ms, which is why it never failed locally.

Now it is a byte-range lock (`LockFileEx`, exclusive, one byte) on the same
file, waited for with an overlapped wait: a waiter queues in the kernel and is
handed the lock as it is released. `DirectoryLock` moved to `compat_writer.h`
so it can be tested alone. Tests: a waiter is served within one 200 ms hold of
a holder that retakes the lock at once (the polling lock failed 4 of 5 runs,
waiting 406 to 1000 ms; the new one waits 203 to 204 ms, 10 of 10); a second
holder is refused and the lock is free after the first goes; a child process
holding it is killed and the lock is free. On the runner, the same 4 × 25 loop:
0 of 100 runs failed. An older build's exclusive open and this build's shared
open still exclude each other, but this build does not wait for an older
holder; the UI and isotone-compat ship together.

**The daemon could start and never link its graph.** Seen twice in the loaded
runs below: "the daemon never linked its graph". Logged: every link attempt
had found the filter's node id still `SPA_ID_INVALID`, after every port,
including the filter's own, had reached the registry. Linking ran only on
registry events, so nothing tried again once the filter was bound. It now
tries again when the filter reaches paused or streaming, which is when its id
is known. With 24 busy processes on 24 CPUs: 4 of 160 starts never linked
before, 0 of 300 after. `linux/daemon/measure.py` has a `starts` case, 100
starts with twice as many busy processes as CPUs; with the new handler made a
no-op it failed 4 runs of 4 (2 to 5 of 100 unlinked).

**The rig's captures glitch now and then.** A capture of the daemon at 2.1
read 0.18 dB low. Its tone had one discontinuity 0.3 s in: a permanent step of
16 samples (mod 48) at 1 kHz, no silence, in a case where nothing is written
during the capture. The spike, which has no daemon in its path, read 6.2 dB low
once on a runner, and the daemon's ring read correctly once while the sink read
low: the drop is in the rig's own streams under load, not in what is measured.
`level_dbfs` now checks that the tone is continuous (every 100 ms block within
0.01 rad and 0.05 dB of the others) and raises otherwise; a capture that
glitched is taken once more with a line saying so, and two in a row fail. That
does not hide a systematic fault in the path, which would glitch both times.
The captured WAV that read 0.18 dB low is reported as a phase step of 1.002 rad.
Clean captures read 0.000 rad.

## 2026-09-19: Linux is stereo only

The owner's decision, on "Linux speaker layouts" above: Linux is stereo, as
EasyEffects is (its sink is FL FR, hard-coded, `src/pw_node_manager.cpp:731`).
The layout picker in Settings, General is gone, and with it daemon.conf
(`linux/transport/daemon_config.*`), the daemon reading it, and its 2.1 layout;
the files are as they were before 7daf8ab. The Speakers view is not offered,
as on Windows for a stereo output. Kept from that commit: the region publishing
its layout when it opens, a fresh region reading as the default state, and
Outputs' probe noticing a fed output come back. The daemon still takes
`--channels` 1, 2, 4, 6 or 8, which the rig's 5.1 case uses; nothing in the app
sets it.

On capturing applications' streams, the owner asked whether EasyEffects does
the same. It does: "Process All Output Streams" (`processAllOutputs`, default
true, `src/contents/kcfg/easyeffects_db.kcfg:190`) writes `target.node` and
`target.object` in the default metadata for every playback stream
(`src/pw_manager.cpp:460-477`), with a switch and per-application and
per-output exclusion lists that Isotone does not have. Left as it is.

## 2026-09-19: Short output names, the global defaults, and a curve that leaves the plot

Three the owner asked for after the laptop's desktop checks.

**An output is shown by its short name.** His laptop's four outputs all read
"Alder Lake PCH-P High Defi..." in the sidebar: one card's node.descriptions
share everything up to where the sidebar cuts them off. PipeWire also carries
node.nick, "HDMI 3" or "Speaker + Headphones", which is what is shown now
(`sink_display_name`, node.nick, else the description, else node.name).

**The global hotkeys are Ctrl+Alt+Shift+E, M, PgDn and PgUp.** They were Ctrl+E,
Ctrl+M and Ctrl+Left/Right, which a global hotkey takes from every other
application: Ctrl+Left/Right is word-jumping in every text field. Save, undo,
redo and the band keys are not global and keep theirs. Not Ctrl+Alt+Shift+
arrows, which GNOME and Cinnamon use to move a window between workspaces.

That found one more thing. On the owner's laptop the new keys did nothing, by
hand or injected: `grp:alt_shift_toggle` was in his XKB options (two layouts,
both us), so Alt+Shift switches the keyboard layout and whichever of the two is
pressed second is taken out of the modifiers. Measured, with every modifier
subset grabbed: Ctrl then Alt then Shift arrives as Ctrl+Alt, Ctrl then Shift
then Alt as Ctrl+Shift. The X grab is granted either way and simply never
matches, which is the worst kind of failure. The X11 path now reads the options
from the root's `_XKB_RULES_NAMES` and, where they switch the layout on
Alt+Shift, marks such a binding failed in Settings and leaves the keys alone
(`altShiftSwitchesLayout`). Moonlight is not affected by any of this: a focused
window is sent the keys whatever the modifiers say. Windows has the same
Alt+Shift as a language switch, but only for a press of the two alone, so
Ctrl+Alt+Shift+E works there; measured on the owner's machine. He turned the
option off on the laptop, where the second layout was a duplicate, and the keys
then worked.

**A curve that leaves the plot is cut at the edge.** A high-pass under the
bottom of the graph was clamped two pixels past it and drawn, so it read as a
flat line along the bottom (owner, 2026-09-19). The curves are clipped to the
plot now, as the spectrum already was, and clamped a plot's height outside so
the slope at the edge is still the curve's.

## 2026-09-19: An output with nothing plugged into it is not listed

The owner: his laptop's three HDMI outputs, none of them plugged in, sat in the
sidebar; Windows leaves an unplugged endpoint out (`e.state != DEVICE_STATE_ACTIVE`,
outputs.cpp). PipeWire says the same thing, but about the card rather than the
sink: its EnumRoute params carry an availability each, "[Out] HDMI3" no,
"[Out] Speaker" unknown, with the card's device index each route is for.

So `PipewireOutputs` binds the Audio/Device cards and reads their routes, and a
sink is connected unless every route for its `card.profile.device` says no
(`sink_connected`). A route that cannot tell says unknown, which counts as
plugged, as does a card whose routes have not arrived yet: an output is never
hidden for want of an answer. Virtual sinks have no card and are always shown.

Two things this needed. The registry's properties for a node carry `device.id`
but not `card.profile.device`, so each sink node is bound as well, for the
properties its info carries (found by printing them, 2026-09-19). And a card's
routes are published as each param arrives rather than at the end of the
enumeration: nothing says an enumeration ended, and a route more can only free
an output, never hide one. A cable in or out makes the server send the device's
info again, which is when they are read afresh.

Measured on the laptop: before, four outputs, all four named "Alder Lake PCH-P
High Defi..."; now, Speaker + Headphones alone, with Easy Effects' sink and the
rig's.

## 2026-09-19: Edits followed a daemon that had restarted

The owner, on his laptop: the EQ did nothing and the spectrum was dead, with the
app showing the right output. Two causes, one of them ours.

**EasyEffects had the stream.** It runs there in service mode and moves every
playback stream into its own sink, then feeds the hardware itself, so Isotone
was not in the path at all. Two programs that both capture every stream cannot
share a machine; whichever gets there first wins. Isotone left the stream alone
because it already carried EasyEffects' target, which is the rule that keeps a
stream a user has moved where they put it. With EasyEffects stopped, the stream
still was not taken: nothing re-examines a stream whose target is cleared. Not
fixed yet, and worth fixing.

**The app kept writing into the region of the daemon that had gone.** A POSIX
shm object outlives its name: a daemon that restarts unlinks its region and
creates another, and the old one stays mapped, writable and readable for
whoever holds it. So every edit went somewhere nothing reads, and the spectrum
drained a ring nobody fills, while the app looked healthy. `SharedRegion` can
now say whether its name still leads to the object it holds
(`still_named()`, the object's device and inode against a fresh shm_open), and
`DeviceLink` checks that no more than once every 500 ms, the interval it
already used before trying a region again, and reopens when it does not.

Found by the test that fails without it: a fake daemon, an edit, a second fake
daemon over the same name, and the next edit must reach the second region and
not the first. The first attempt at the fix passed that test and did nothing on
the laptop: the spectrum calls into the same path every frame, and the timestamp
was updated on every call, so the interval never elapsed and the check never ran.
Fixed, and measured again on the laptop: with the app running, the daemon
restarted, and a band added, the band reached the new daemon's region.

---

---

# Stage 6 begins: the bug pass before packaging (2026-09-19)

The owner's order: fix the bugs and explore for more, then package (2026-09-19).
Versions are `x.y.z`, major for an overhaul, minor for a feature, patch for
fixes; `0` is the beta, so the first release is **0.1.0**, which is what
`project(VERSION)` already said. GitHub releases, when the owner says.

Baselines first, all green before anything changed: Windows ctest 7/7, the APO
self test, the transport check, `gen_reference.py --check` (largest difference
0.000e+00), `ui_tests` 50, `ui_model_tests` 125, `ui_qml_tests` 290; Linux ctest
7/7 and all three stages of `ci-audio.sh`.

## A stream another program claimed was lost for the rest of the run

The one left open on 2026-09-19. EasyEffects moves a stream by writing
`target.object` into the default metadata for that node, and clears it again
when it quits. The daemon saw the claim, erased the stream from `moved_streams`
so its own exit would not undo someone else's choice, and then never looked at
that stream again. EasyEffects quitting gave the stream back to the default
sink, not to Isotone.

`on_metadata_property` now takes the stream back when the target is cleared and
the node is one it would have captured. Reproduced first, in the rig rather than
by reading: `reclaim` in `linux/daemon/measure.py` plays a stream, waits for
Isotone to capture it, claims it away to the 5.1 sink with `pw-metadata`, clears
the claim, and requires it to come back. Before the fix: `claimed away: yes,
back: NO`. After: both yes, with every other figure in the suite unchanged to
the last decimal.

**A guard that could never fire, removed rather than kept.** The first version
of the fix carried a `releasing` flag, on the reasoning that `release_streams`
clears these same targets on the way out and runs the loop (`roundtrip`) to see
the clears land, so it would re-capture everything it had just released.
Mutating the flag out did not break the suite, which is the point at which a
guard has to be justified or deleted. A `fprintf` in the branch settled it: with
another program's claim the handler fires twice (the claim, then the clear), and
during `release_streams` it fires **zero** times. PipeWire does not deliver a
metadata change back to the client that made it, so only another program's clear
ever reaches the handler. The flag was dead code and is gone; the reason is in
the comment where the hazard looked real.

The measurement that could not see it either: `released()` only checked which
sink the stream was linked to. A daemon that re-claimed on the way out still
leaves the stream falling back to the default, because its own sink has gone, so
the link cannot tell the two apart. It now also requires the `target.object`
entry to be gone from the metadata, which is what `release_streams` is for.

## The CRT under audiodg, settled

`windows/apo/CMakeLists.txt` had deferred this to stage 6. Measured rather than
assumed:

| | imports | size |
|---|---|---|
| `/MD`, as built until now | `MSVCP140`, `VCRUNTIME140`, `VCRUNTIME140_1`, 7 `api-ms-win-crt-*` | 996,352 bytes |
| `/MT` | `ole32`, `SHELL32`, `ADVAPI32`, `KERNEL32` | 2,379,264 bytes |

`/MT` it is. audiodg loads the APO as LocalService, and a DLL that needs the
redistributable there fails in the process that carries all system audio; 1.4 MB
in an installer is not a reason to keep that failure available. `isotone_core_mt`
and `isotone_transport_mt` are second targets rather than a property on the
first, because `/MT` and `/MD` static libraries cannot both be linked into one
binary and the UI links Qt, which is `/MD`. Nothing crosses a heap boundary
between them: everything the APO hands out is COM-allocated.

`isotone-apo-selftest` passes unchanged with the `/MD` host loading the `/MT`
DLL, which is the same mismatch audiodg has, and is the evidence this is safe
rather than the reasoning.

## Every shipped Windows binary now carries its version

`IsoAPO.dll` had no version resource at all, so Settings, About showed the
engine's version as blank: `diagnostics.h` reads the registered DLL's file
version, and there was none to read. `windows/version.rc.in` and
`isotone_version_resource()` in `windows/version.cmake` give one to `IsoAPO.dll`,
`isotone.exe`, `isotone-devicetool.exe` and `isotone-compat.exe`, configured from
`project(VERSION)` so the number lives in one place. Test executables get none:
nothing reads their version.

The self test checks it, and the mutation check confirms it catches the old
state: with `isotone_version_resource` commented out, `FileVersion = '',
expected '0.1.0'` and two failures.

## A CI failure on a docs-only commit: the ring test's clock

CI went red on `Where things stand before packaging`, which changed nothing but
`docs/`. `core_tests` failed at `test_audio_ring.cpp:414`, `CHECK(chunks > 100)`,
in the case where a reader races a writer for 500 ms and every chunk it reads
must be contiguous. `bad == 0` is vacuous if the reader saw nothing, so the chunk
count was asserted as well, against a fixed slice of wall clock. That makes it a
claim about scheduling, and on a starved runner it is false with the code
correct.

The scale says it was not ordinary slowness: this machine reads 1,517,430 chunks
in that 500 ms, and 1,010,464 with three busy processes per core, against the
runner's fewer than 101. Whatever the runner was doing, the answer is the same.
The loop now runs its 500 ms and then keeps going until it has seen
`kMinChunks` (101), with a 30-second backstop so a ring that delivers nothing at
all still fails rather than hangs. A fast machine does the full 500 ms, which is
where the coverage of the race comes from; a starved one takes longer instead of
failing.

Checked by forcing the new path rather than waiting for another flake: with
`kMinChunks` temporarily at 3,000,000, the loop ran 1,188 ms, stopped at exactly
3,000,000 chunks and passed, where the 500 ms window alone would have stopped at
about 1.5 million.

## The installer's machine-wide half lives in devicetool

Everything the installer does to HKLM and to the ProgramData ACL is a
devicetool command, not NSIS script: `machine-install --dll <path>` and
`machine-uninstall`, both with `--dry-run` and both printing the same JSON every
other command prints. NSIS is left copying files and calling this. That keeps
the owner's rule workable, because a dry run of the whole machine-wide install
is one command he can read, and it puts the work where the tests already are.

**The COM class is registered by the DLL's own `DllRegisterServer`**, loaded and
called, rather than reimplemented in devicetool. `RegisterAPO` and the
`APO_REG_PROPERTIES` live in `dllmain.cpp` and two copies would drift. The cost
is that a dry run cannot run it, since it goes at the registry directly and has
no `RegistryDryRun` to install; so the dry run checks every precondition it can
and says the call would follow. The preconditions are the ones that matter: an
absolute local path, the file exists, it is not under a user profile, and
LOCAL SERVICE can read and execute it. audiodg runs as LocalService, and a DLL
it cannot load registers cleanly and then fails every endpoint with
E_ACCESSDENIED.

**`DisableProtectedAudioDG` is not ours to remove.** This machine already had it
at 1, set by Equalizer APO, which is installed here and stops working without
it. So `machine-install` records whether it was the one to set the value
(`HKLM\SOFTWARE\IsoAPO\ProtectedAudioDGSetByIsotone`), and `machine-uninstall`
removes it only when that record says yes and Equalizer APO is not installed.
The dry run says which of the three it is and why.

**The data directory's ACL**, which `docs/ui-spec.md` has been carrying a note
about since stage 4 ("a file one Windows account writes cannot be replaced by
another until the installer sets the directory's ACL"):

```
D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;LS)(A;OICI;0x1301bf;;;AU)(A;OICI;0x1200a9;;;<Audiosrv>)
```

Protected, so ProgramData's inheritance (Users: read and create only) does not
apply as well, and OICI so files created inside carry it. `0x1301bf` is modify
rather than write because the app replaces the state file with `MoveFileEx`,
which needs DELETE on the file already there. The Audiosrv ACE is resolved
through `LookupAccountName("NT SERVICE\Audiosrv")` rather than written out: the
SID is derived from the service name and is the same everywhere, but a literal
that is wrong is an ACE that silently grants nothing, and the value differed
from the one memory offered.

Each of the four is mutation-checked: dropping the LOCAL SERVICE ACE, taking
DELETE off Authenticated Users, unprotecting the DACL and not resolving the
Audiosrv SID each fail a test. The first attempt at the ACL test passed all four
mutations, because it read `registration.dll` where the field is
`isoapo.registration.dll` and skipped itself. The command was changed rather than
only the test: the plan (the directory, the ACL, the protected-audio state) is
now reported even when the DLL checks fail, which is what a dry run should do
anyway, and the test no longer depends on what is installed on the machine
running it.

## The logo, and the mark it ended up being

Six rounds, and the useful part is what was rejected.

Round one put nine marks up across the three briefs the owner asked for (evolve
the wave, fresh, fresh but still audio). He picked **Bands**, five bars about a
zero line, and said the rest looked like ass. Round two opened Bands out to
sixteen variants and shotgunned seventeen more fresh ideas; nothing beat the
original. Round three refined that one mark, one measure at a time: bar width,
spacing, reach, cap shape, six height profiles.

**Round three's profile was a middle finger.** Short, short, tall, short, short:
the owner spotted it immediately. Round four therefore took it as a structural
constraint rather than something to nudge, and tried six ways of never having a
lone tall bar: peak off centre, two tall together, a ramp, a valley, two peaks,
and even bar counts (no middle bar to be the tall one). Worth recording that
moving the peak sideways does **not** fix it, which round four's sheet showed and
said: the silhouette is the same, just shifted.

The owner picked the off-centre-peak and two-peak families, round five expanded
both to fourteen each, and he asked for more below the zero line. Round six is
that: two axes kept apart, deeper (the same bars reaching further down) and wider
(bars pushed under the line that were above it). **m4-w2** won: two peaks with the
bar between them well under, cuts at both ends.

**Centring is on the ink, not the zero line** (owner). The bars hang from y = 12
but are not symmetric about it: the ink spans 4.60 to 17.18, whose middle is
10.89. Centring on the line left 56 px above and 75 px below on a 240 px tile.
Everything is shifted +1.11 down, which gives 5.71 units of margin above and
below, and 3.10 left and right. Horizontally it was already right.

The geometry, which is now the specification:

| bar | centre x | gain | top | height |
|---|---|---|---|---|
| 1 | 4.60 | -0.45 | 11.61 | 4.83 |
| 2 | 8.30 | +1.00 | 5.71 | 8.90 |
| 3 | 12.00 | -0.70 | 11.61 | 6.68 |
| 4 | 15.70 | +0.70 | 7.93 | 6.68 |
| 5 | 19.40 | -0.45 | 11.61 | 4.83 |

24 x 24 box, bar width 3.00, pill caps, padding 4.60, bars 3.70 apart. Tile
radius 22% of the side, glyph inset 14%.

**The bar heights are designed, not a filter response.** The curve marks of round
one did trace real responses, computed with the same RBJ peaking formula
`core/biquad.cpp` uses. A bar mark cannot: a two-band response over three decades
is within a decibel of flat at most of the five points a five-bar mark samples,
so tracing one gives a tall bar and four stubs. Said here because round one's
sheet claimed otherwise before the bars existed.

**Four copies of the geometry, one test.** `tools/gen_icons.py` cuts
`ui/res/isotone.ico` and `.svg`, the installer's two bitmaps and eight Linux
hicolor PNGs; `ui/src/logomark.cpp` draws the tray and window icon;
`ui/qml/Icon.qml` strokes it as five round-capped segments at width 3 (the
renderer already used round caps, so a segment is a pill bar). Nothing shares the
numbers at build time, so `ui/tests/test_logomark.cpp` measures them back out of
a rendered pixmap: five bars at the spec's positions, equal margins on the ink,
the ink 17.80 x 12.58, and the inset leaving 86%. Mutation-checked three ways:
reverting the centring, a bar 0.4 units short, and width 3.0 to 2.7 each fail it.
`gen_icons.py --check` reports any asset that no longer matches.

## Install rules, and 31 MB of Qt that was not needed

`cmake --install build --prefix <dir>` now lays out exactly what ships, on both
platforms. Nothing in the tree had an `install()` rule before this.

Windows is flat in the install directory, because that is where the APO is
registered by full path and where `devicetoolPath()` looks for the devicetool
beside `isotone.exe`. Linux follows GNUInstallDirs, so the `.deb` is the same
tree under `/usr`:

```
bin/isotone  bin/isotone-daemon  bin/isotone-state
lib/systemd/user/isotone-daemon.service
share/applications/isotone.desktop
share/icons/hicolor/{16,22,24,32,48,64,128,256}x*/apps/isotone.png, and scalable
share/metainfo/io.github.isotone.Isotone.metainfo.xml
```

What is **not** installed: `isotone-compat`, `isotone-shm`, `isotone-measure`, the
self tests and the spike. They are for this repository. The app shells out to
`isotone-devicetool` and nothing else, which is what decided the list.

The Qt runtime is staged by `windeployqt` rather than listed, because which DLLs
and QML modules an `isotone.exe` needs is a question about the binary. Its first
answer was **120.9 MB**, and two thirds of the excess was junk:

| | |
|---|---|
| `vc_redist.x64.exe` | 18 MB of installer for 1.5 MB of DLLs. `--no-compiler-runtime` plus `InstallRequiredSystemLibraries` ships MSVCP140, VCRUNTIME140 and VCRUNTIME140_1 instead. `isotone.exe` needs them because it links Qt, which is /MD; `IsoAPO.dll` does not, being /MT |
| `dxcompiler.dll`, `dxil.dll` | 15.5 MB of runtime HLSL compiler. Nothing in `qml/` is a ShaderEffect and nothing calls `qt_add_shaders`, so they are only ever loaded to compile nothing. windeployqt has no flag for them, so the install removes them |

**89.6 MB**, and the app runs from the staged tree. The QtQuick.Controls styles
(Imagine, Material, Universal, Fusion, FluentWinUI3, about 9 MB) are still
there: they arrive through QtQuick.Dialogs, whose FileDialog is native on
Windows, so they are probably unused, but an attempt to test that was
inconclusive and they stay until it is not. Worth noting the attempt failed
usefully: the pruned and unpruned trees produced byte-identical behaviour on the
same command, so the blank screenshot that looked like a pruning failure was an
invalid `--click` argument, which takes `x,y` and not an object name.

The desktop half is new: `linux/packaging/isotone.desktop` and an AppStream
`metainfo.xml`. `appstreamcli validate` is clean apart from `url-not-reachable`
on the GitHub URLs, which is the repository being private rather than anything
in the file.

The systemd user unit installs to `lib/systemd/user`, written out rather than
through CMAKE_INSTALL_LIBDIR, which expands to `lib/x86_64-linux-gnu` on Debian
where systemd does not look. It is installed **disabled**: starting the daemon
moves every application's audio through Isotone, which is not something a
package does to a machine on its own.

## The .deb

CPack's DEB generator off the same `install()` rules, so the package and a
staged tree are the same layout. 1.6 MB; dependencies read off the binaries by
`dpkg-shlibdeps` rather than written out, so a Qt or PipeWire version bump needs
no edit here. `pipewire`, `wireplumber` and a PulseAudio shim are added by hand,
because they are programs rather than libraries the linker records.

**The daemon is enabled on install** (owner, 2026-09-19). The recommendation
here was the other way, and the reason is worth keeping: starting the daemon
moves every application's audio through Isotone, and it cannot share a machine
with EasyEffects or anything else that captures every stream. The owner's call
is that installing it is the point of installing it.

Two details that are easy to get wrong:

- `systemctl --global enable`, not a `user-preset` file. A preset is consulted
  by `preset-all`, which nothing runs, so it would only ever affect accounts
  created afterwards. `--global` writes `/etc/systemd/user/` and applies to
  every account that already exists.
- On a **first install only** (`[ -z "$2" ]`). On an upgrade, re-enabling would
  undo a user who had turned it off by hand. `prerm` disables it again on
  removal, and stops it for whoever is logged in so the audio goes back to the
  hardware without a sign-out.

**lintian found four things the build was getting wrong**, none of which any
test would have:

| | |
|---|---|
| `control-file-has-bad-permissions 0777` | The source tree is on a Windows filesystem when built from WSL, which reports every file as 0777. `CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION` |
| `no-copyright-file` | Now a machine-readable `copyright`, which also records the vendored Equalizer APO code, doctest and Instrument Sans |
| `no-changelog` | Written and gzipped |
| `no-manual-page` | `isotone(1)`, `isotone-daemon(1)`, `isotone-state(1)` |

The changelog and man pages are gzipped at **configure** time. The first attempt
did it in an `install(CODE)` block, which worked for `cmake --install` and broke
under CPack: CPack stages through `DESTDIR`, so `${CMAKE_INSTALL_PREFIX}` inside
an install script is the real `/usr` and writing there is refused.

What lintian still says is three `maintainer-script-calls-systemctl`. It wants
`dh_installsystemd`, which is debhelper's, for system units; this is a CPack
package shipping a user unit. Deliberate.

Measured, not assumed: the rig was pointed at `/usr/bin` rather than the build
directory, and the **packaged** daemon passed every case, `live` through
`reclaim`, each to the analytic filter. Install, remove and purge all leave the
machine clean, and the `--global` symlink appears and disappears with them.

## A CI failure in the compat writer: reading the DACL of a file that is going away

`compat_tests` went red in CI on 2026-09-20, in the case where three threads
write the same path 200 times each: `first_error = 5`, ERROR_ACCESS_DENIED. The
commit had not touched `windows/compat`, and the two runs before it passed, so
it was intermittent.

**The first diagnosis was wrong, and measuring is what caught it.** The obvious
suspect was `replace_denied_for_good`, which decides whether an
ERROR_ACCESS_DENIED from `MoveFileExW` is worth retrying. A target another
writer has left pending deletion refuses a DELETE open with
ERROR_ACCESS_DENIED, exactly as an ACL that denies delete does, and the comment
there said a pending deletion could be treated as permanent because "the
caller's next attempt sees the name gone" -- true of one writer, false of two.
That reasoning is sound and the change was written. A `fprintf` in the retry
loop then showed it never runs: **zero** iterations in the failing case. The
change was reverted; it was speculation.

The failure is earlier, in `dacl_for`, which reads the DACL the replacement file
should be given. It falls back to "the DACL a file created in this directory
would get" only on ERROR_FILE_NOT_FOUND. A target pending deletion answers
`GetFileSecurityW` with ERROR_ACCESS_DENIED, which was returned straight out of
`write_file_atomically` before the replace and its retry loop were ever reached.
That is the 5.

It now waits out an ERROR_ACCESS_DENIED for 50 ms, which is far longer than a
pending deletion lasts and inside every deadline above it. Waited out rather
than treated as missing, because that error is also what a target genuinely out
of reach gives, and that one should still be reported.

**The test is deterministic, not a stress run.** Twelve stress runs under load
passed both with the fix and with it mutated out, on this machine, which proves
nothing: the race needs a slower machine than this one. Setting the delete
disposition on an open handle puts a name in the pending-deletion state at once
and holds it there, so the condition can be created exactly. The first attempt
used FILE_FLAG_DELETE_ON_CLOSE, which does not: other opens still succeed until
the last handle closes, and the test leaked the probe handle and so held the
file pending for ever. `SetFileInformationByHandle(FileDispositionInfo)` does
what was wanted, and the test asserts the state it has made is the right one
(an open refused, and refused with ERROR_ACCESS_DENIED) before testing anything
else. Mutating the 50 ms to 0 fails it; restoring it passes.

## The .deb on real hardware

Installed on the owner's own laptop (Mint 22.3, PipeWire 1.0.5, Intel SOF HDA
DSP), which is the only place the package could be shown to work on real
hardware: WSL's rig is two null sinks and the Mint VM has no audio out at all.
The owner ran the one command that needs a password; everything after it was
unelevated.

**Measured through the packaged binary**, `/usr/bin/isotone-daemon`, not a build
directory: flat -6.021 dBFS, with a -12 dB peaking band at 1 kHz -18.021, so
-12.000 dB against -12.000 analytic, error -0.0000 dB. Through a null test sink
made by `rig-up.sh` in the running PipeWire, so nothing was audible, the default
sink never changed and nothing was written to his configuration.

The app runs from `/usr/bin` on his Cinnamon desktop and shows what it should:
"Daemon stopped" with a Start button, because the package's global enable takes
effect at the next login and his session predates it.

**The unit trap the VM hid.** On the VM the daemon appeared to start from the
package and was in fact running `~/build/linux/daemon/isotone-daemon`, from a
user unit at `~/.config/systemd/user/` left over from earlier testing, which
shadows `/usr/lib/systemd/user/`. That would have been a false pass for the one
requirement the package has to meet. It was caught by reading the `Loaded:` line
rather than trusting `is-active`, and the check is now to ask systemd for
`FragmentPath` and to move the build tree out of the way entirely. The laptop
has no such override, so its result stands on its own.

Left installed and enabled at the owner's choice: from his next login his audio
goes through Isotone. `sudo apt-get purge isotone` removes it.

## The Flatpak, and the answer to plan question 5

Plan section 12 asked: "Flatpak for a daemon that must own a virtual sink: what
permissions, and does the Electron app inside a sandbox still reach the
`shm_open` names the daemon created?" Measured on 2026-09-20.

**The answer is no, and shipping both halves in one Flatpak does not fix it.**
The obvious reasoning is that app and daemon in one Flatpak are in one sandbox
and so share `/dev/shm`. They are not: two `flatpak run` instances of the *same*
application each get their own. A daemon started in one instance and an app in
another cannot see each other's region at all. Tested directly, by creating a
name in `/dev/shm` from one instance and looking for it from another:

```
  A: created /dev/shm/isotone.probe
  B sees:
  B CANNOT see it -> separate /dev/shm per instance
```

So `--device=shm` is needed whoever runs the daemon. With it, the same probe
finds the name, and there is one good thing about needing it: a daemon on the
host from the `.deb` and a sandboxed app then find each other too.

The rest of the permissions: `--filesystem=xdg-run/pipewire-0` for PipeWire
itself, since the daemon creates a node and links it and the PulseAudio shim
cannot do either; `--socket=pulseaudio` as well, because the portal and Qt
expect an audio socket to exist; `--talk-name` for the Desktop and
GlobalShortcuts portals; wayland with an X11 fallback.

**One name everywhere.** Flatpak exports only files named for the application
ID, so `isotone.desktop` and `isotone.png` were silently dropped, with the whole
reason given as "non-allowed export filename" in the build log: the app would
have installed with no menu entry and no icon. Renaming them in the manifest
then broke the metainfo, whose `launchable` still named the old file, and
`appstreamcli compose` refused the build with `gui-app-without-icon`. Two hacks
inviting a third, so the desktop entry and the icons are now
`io.github.isotone.Isotone.*` everywhere, which is what the AppStream ID already
was and what a `.deb` is equally happy with.

**Measured end to end on the owner's laptop**, which is the only machine with a
real PipeWire session and real hardware: the daemon inside the sandbox, its
region written from `/usr/bin/isotone-state` on the host, outside the sandbox,
and the result measured from the host. -12.000 dB against -12.000 analytic,
error -0.0000. That one test exercises the sandbox boundary in both directions.

**Left open: the Flatpak has no way to start its daemon.** On Linux the app runs
`systemctl --user start isotone-daemon.service`, which inside a sandbox reaches
nothing, and a Flatpak cannot install a unit into the host's systemd. The app
should start the daemon itself when it is sandboxed.

### Starting the daemon from inside the sandbox

`systemctl --user start isotone-daemon.service` reaches the sandbox, not the
session, and a Flatpak cannot put a unit where the host's systemd would find
one. When `FLATPAK_ID` is set the app now starts `isotone-daemon` directly and
detached, rather than through systemd.

Detached, because a child would go when the window is closed to the tray or
quit, and the EQ with it. Measured: a detached daemon does outlive its
`flatpak run`, and keeps its region, which is what matters. The cost is that
`flatpak run` does not return while it lives, because the launcher waits for the
sandbox to empty and the daemon is in it. That is inherent rather than a fault:
the sandbox has to stay up for the daemon to keep running at all.

**Not done, and worth knowing before this is called finished: the Flatpak does
not start at login.** The `.deb` enables a user unit; the Flatpak has none, and
the app's "launch at sign-in" writes an autostart file into
`~/.var/app/io.github.isotone.Isotone/config`, which the desktop does not read.
Doing it properly means the Background portal
(`org.freedesktop.portal.Background`, `RequestBackground` with autostart), which
is not wired up. Until it is, the Flatpak's EQ starts when the app is opened.

**Found on the way: the sandboxed app ignores the desktop's light or dark
setting.** The `.deb` app came up light on the owner's Cinnamon desktop and the
Flatpak came up dark on the same desktop, minutes apart. Inside the sandbox the
app cannot read the host's theme, the portal reports no preference, and
`Theme.qml` reads anything that is not explicitly Light as dark
(`systemDark: Qt.styleHints.colorScheme !== Qt.ColorScheme.Light`). Not a
Flatpak quirk: the same would happen anywhere the scheme is unknown.

## Launch at sign-in in a Flatpak

The toggle in Settings wrote `$XDG_CONFIG_HOME/autostart/isotone.desktop`, and
in a sandbox `XDG_CONFIG_HOME` is `~/.var/app/io.github.isotone.Isotone/config`.
Measured inside the installed Flatpak: `$HOME` is the real home and
`XDG_CONFIG_HOME` is that one, so the file went somewhere no desktop reads and
the toggle then read it back and reported success. The whole feature was a
file written to itself.

**The Background portal is the way in, and the machine already had the
precedent.** `~/.config/autostart/com.github.wwmm.easyeffects.desktop` on the
owner's laptop was written by it, and says exactly what to expect:

```
[Desktop Entry]
Type=Application
Name=com.github.wwmm.easyeffects
X-XDP-Autostart=com.github.wwmm.easyeffects
Exec=flatpak run --command=easyeffects com.github.wwmm.easyeffects --service-mode --hide-window
X-Flatpak=com.github.wwmm.easyeffects
```

Everything below was measured in the installed Flatpak on the laptop before any
of it was written, with `gdbus` in the sandbox against the real
xdg-desktop-portal 1.20 (backend: xdg-desktop-portal-xapp, which is what
provides `org.freedesktop.impl.portal.Background` on Cinnamon; the gtk backend
does not):

- `org.freedesktop.portal.Background` answers for version 2 from inside the
  sandbox. **No `--talk-name` was needed for it**: one bus name,
  `org.freedesktop.portal.Desktop`, carries every portal interface, and the
  manifest already had it. The `--talk-name=org.freedesktop.portal.GlobalShortcuts`
  line beside it has always been granting nothing for the same reason.
- `RequestBackground` with `autostart: true` and
  `commandline: ['isotone', '--tray']` wrote
  `~/.config/autostart/io.github.isotone.Isotone.desktop` on the host, with
  `Exec=flatpak run --command=isotone io.github.isotone.Isotone --tray`. No
  dialog, no interaction: the permission store already said
  `background background io.github.isotone.Isotone yes`, which
  xdg-desktop-portal's own background monitor had put there.
- The same call with `autostart: false` removed the file.

**Reading the state back is what cost a permission.** The portal has no getter:
`RequestBackground` is a setter that answers with `background` and `autostart`
in its Response, and there is nothing to ask afterwards. EasyEffects does not
read it back at all, which is why its manifest has no autostart filesystem
permission; that leaves a toggle that goes stale the moment someone unticks the
entry in the desktop's own Startup Applications. Since `autostart_xdg.h` has
said since stage 4 that the file is the whole of the state, the file is what is
read, and that needs `--filesystem=xdg-config/autostart:ro`.

**A directory, not the one file.** `--filesystem=xdg-config/autostart/io.github.isotone.Isotone.desktop:ro`
is narrower and does not work, measured both ways: a file bound that way is
bound at sandbox start, so an entry created afterwards is invisible, which is
every first time the toggle is turned on. The same test with the directory shows
a file the host created while the sandbox was running. Read-only either way; a
write inside fails with `Read-only file system`, which is right, because the
entry is the portal's.

**The call is synchronous and the entry is the answer.** The toggle is
synchronous, so `request_autostart` waits for the Request object's Response in a
nested event loop, which keeps the window alive if a desktop decides to ask
first; 30 seconds is a backstop against a portal that takes the call and never
answers, not a normal path. Nothing is decided on the Response results, because
what a portal puts in them is its own business: `setLaunchAtSignIn` reports
`launchAtSignIn()` read back off the entry. For the record, the real portal
answers `autostart=true background=true`.

**Measured end to end on the laptop**, in the installed Flatpak on the owner's
real Cinnamon desktop, driving the app's own window rather than a test:

| | |
|---|---|
| before | no entry; the toggle's track grey (144, 150, 157) |
| `--view settings/general --click 1030,194` | `accepted (autostart=true background=true)`; `~/.config/autostart/io.github.isotone.Isotone.desktop` appears with the `flatpak run` line; the track blue (106, 167, 244) |
| a second, fresh `flatpak run` | still blue: the entry is read back through the read-only mount, which is the restart case |
| `flatpak run --command=isotone io.github.isotone.Isotone --tray`, the entry's own command | the app comes up, `isotone --tray` in `ps` |

The tests are `ui_tests`' Flatpak case for the directory and the file's name, and
a new `ui_model_tests` case against `mock_portal.py`, which grew the Background
interface and writes the entry the real portal writes.
`run_portal_test.sh` runs it in a second run of the binary with `FLATPAK_ID`
set, so the variable reaches nothing else. Mutation-checked three ways, each
failing: the sandbox branch skipped, the directory left as `XDG_CONFIG_HOME`'s,
and the entry left named `isotone.desktop`.

### The Flatpak starts at sign-in, and its EQ still does not

Worth being exact about what this fixes, because it is half of what the `.deb`
gives. The `.deb` enables a user unit, so **the daemon** runs from sign-in and
the EQ is on whether or not the app is opened. This makes **the app** run from
sign-in. The daemon in a Flatpak is started by the app, on the Devices page, by
hand (2026-09-20, "Starting the daemon from inside the sandbox"), and nothing
presses that at login.

Measured, rather than reasoned: the entry's own command was run, the app came up
in the tray, `ps` showed `isotone --tray` and no daemon, and the default sink
was still the laptop's hardware. So a Flatpak-only machine signs in to an app
with no EQ behind it.

The obvious fix, the app starting the daemon when it was launched by the
autostart entry, is **not done, and is the owner's call**, because of what it
would do on a machine that has both packages: his laptop has the `.deb` with its
unit enabled *and* the Flatpak. Two daemons would race for the same virtual
sink at sign-in. A "start it only if none is running" check is a race at exactly
the moment both start. It wants deciding, not guessing.

## 2026-09-20: GNOME and KDE, on Wayland

Two VMs built overnight for the desktops stage 6 had left (docs/notes/linux-vm-setup.md
has how, and the two traps): **Isotone GNOME**, Ubuntu 26.04.1 with GNOME on
Wayland, and **Isotone KDE**, Kubuntu 26.04.1 with Plasma 6 on Wayland. Both run
a much newer stack than the 24.04 baseline: GCC 15, Qt 6.10.2, PipeWire 1.6.2,
systemd 259.5, kernel 7.0.

**Kubuntu 26.04 has no Plasma X11 session at all.** `/usr/share/xsessions` does
not exist and `/usr/share/wayland-sessions` holds one file. So "Plasma on X11",
which the plan listed as a thing to check, is not a thing to check any more.

### The portal refuses global shortcuts to an app that is not in a Flatpak

The headline, and it needed the real desktop to find. On Plasma Wayland the app
takes the portal path correctly, calls `GlobalShortcuts.CreateSession`, and gets

```
error time=... sender=:1.37 -> destination=:1.103
      error_name=org.freedesktop.portal.Error.NotAllowed reply_serial=14
```

so `BindShortcuts` never follows, nothing is registered, and a global hotkey
does nothing at all. Nothing in the app reports this, because
`setGlobalFailed` is only reached from the bind response, which never comes.
Taken off a full `dbus-monitor` of the session while the app started;
`GetConnectionCredentials` in the same trace shows why: `LinuxSecurityLabel`
is `unconfined`, so the portal has no application ID to name the desktop's
shortcut record with, and refuses.

**GNOME does exactly the same thing**, with the same error on the same method,
which rules out one desktop's quirk:

```
error ... sender=:1.59 -> destination=:1.99
      error_name=org.freedesktop.portal.Error.NotAllowed reply_serial=12
```

**The same app in the Flatpak is allowed on both**, and it is worth saying what
that looks like, because it is the shipping path on a Wayland desktop:

- **Plasma** puts up **Global Shortcuts Requested**, "Isotone wants to register
  the following 4 shortcuts", and lists EQ on / off, Mute, Next preset and
  Previous preset against `Ctrl+Alt+Shift+E`, `+M`, `+PgDown` and `+PgUp`.
  **GNOME** puts up **Add Keyboard Shortcuts** with the same four and the same
  keys, written its way round (`Shift+Ctrl+Alt+E`). Both are the desktop
  spelling `toPortalTrigger` asked for.
- After the dialog, `~/.config/kglobalshortcutsrc` gains

  ```
  [io.github.isotone.Isotone]
  _k_friendly_name=Isotone
  eq=Ctrl+Alt+Shift+E,Ctrl+Alt+Shift+E,EQ on / off
  mute=Ctrl+Alt+Shift+M,Ctrl+Alt+Shift+M,Mute
  nextPreset=Ctrl+Alt+Shift+PgDown,Ctrl+Alt+Shift+PgDown,Next preset
  previousPreset=Ctrl+Alt+Shift+PgUp,Ctrl+Alt+Shift+PgUp,Previous preset
  ```

  and on GNOME the trace shows `BindShortcuts` going on to
  `org.gnome.Settings.GlobalShortcutsProvider`.
- And it fires, on both. Measured with another window focused (Dolphin on
  Plasma, Files on GNOME) and the keys put in at the virtual machine's keyboard
  controller, which is a real keyboard as far as the compositor is concerned,
  rather than through XTEST, which a Wayland compositor does not see. The EQ
  toggle's track, in pixels off the screenshot:

  | | before | after one press | after another |
  |---|---|---|---|
  | Plasma | (106, 167, 244) | (144, 150, 157) | (106, 167, 244) |
  | GNOME | (106, 167, 244) | (144, 150, 157) | (106, 167, 244) |

So global hotkeys on Wayland are a Flatpak feature, not a `.deb` feature, and
the `.deb` should say so rather than leave a toggle that silently does nothing.
**It is a scope, not a service, and that one word is the whole thing.**
xdg-desktop-portal derives a host application ID from the systemd unit an app
runs in, and the XDG specification says a launcher starts an app in
`app-<application id>-<random>.**scope**`. The first attempt used
`systemd-run --user --unit=...`, which makes a *service*, and the portal
answered `NotAllowed`; that is what the first version of this entry concluded
from, wrongly. With `--scope`, and the cgroup read back to prove it:

```
systemd-run --user --scope --unit=app-io.github.isotone.Isotone-5151 ... isotone
  /user.slice/user-1000.slice/user@1000.service/app.slice/app-io.github.isotone.Isotone-5151.scope
```

`CreateSession` is answered, `BindShortcuts` follows with all four shortcuts
and their triggers, and **the key fires**: with Dolphin raised and focused, one
`Ctrl+Alt+Shift+E` took the EQ toggle from (48, 114, 193) to (100, 105, 112),
and Dolphin's title bar is in both frames so there is no question about which
window had focus.

So global hotkeys on Wayland are **not** a Flatpak-only feature. An app gets
them when it runs where a desktop launcher would have put it, which is what
happens when someone opens it from the menu. What refuses is an app started
from a terminal or an ssh session, which lands in `session-N.scope`, and that
is every way this was launched during the first night's checks. The lesson is
narrower and more annoying than "Wayland cannot": *this cannot be tested from a
shell*.

### An application ID is what the portal wants

Measured to the end on both VMs, and the rule turns out to have two halves. The
portal reads the systemd unit the app is running in and wants:

1. **One of the two shapes the XDG specification defines.**
   `app-<id>-<random>.scope`, which a launcher makes, and
   `app-<id>@autostart.service`, which systemd's xdg-autostart generator makes
   for an entry in `~/.config/autostart`. Both are accepted. A
   `app-<id>-<random>.**service**` is not a shape anything defines, and it is
   refused: that was the first attempt, and it is why the entry above once said
   there was no way out at all.
2. **An `<id>` that resolves to an installed desktop file.** This is the half
   that was actually wrong in Isotone.

`write_autostart` named the entry `isotone.desktop`, so every sign-in gave the
desktop an application ID of `isotone`, and what a package installs is
`io.github.isotone.Isotone.desktop`. Nothing resolved, and the keys were refused
on GNOME and on Plasma alike. Renaming the entry to the application ID fixes it
on both, and nothing else had to change. The trace either way, from a full
`dbus-monitor` across a reboot:

| | unit at sign-in | CreateSession |
|---|---|---|
| GNOME, `isotone.desktop` | `app-gnome-isotone-2073.scope` | `NotAllowed` |
| Plasma, `isotone.desktop` | `app-isotone@autostart.service` | `NotAllowed` |
| GNOME, `io.github.isotone.Isotone.desktop` | `app-gnome-io.github.isotone.Isotone-2081.scope` | answered, BindShortcuts follows to `org.gnome.Settings.GlobalShortcutsProvider` |
| Plasma, same | `app-io.github.isotone.Isotone@autostart.service` | answered, BindShortcuts follows |

**And the key fires at a real sign-in**, which is the check this was all for: on
both VMs, rebooted, autologin, no window open and nobody touching anything, the
tray menu's EQ item read `toggle-state 1`; one `Ctrl+Alt+Shift+E` at the virtual
machine's keyboard controller took it to `0`, and another took it back to `1`.

`kAutostartFileName` is now the application ID. The old name is kept as
`kLegacyAutostartFileName`, and only ever removed: an entry left under it would
go on starting the app with an ID that resolves to nothing, so the toggle reads
it when it is the only one there, and writing or clearing the toggle takes it
away. Mutation-checked both ways, the name and the cleanup.

**Not needed after all:** the app re-executing itself into a scope. Both
desktops already put it somewhere the portal accepts; they just needed to be
told who it was.

**What was ours, and is fixed: the refusal was invisible.** `bind()` sent
CreateSession with `asyncCall` and threw the reply away, so an error reply
meant `onCreateResponse` never ran, `bound` was never emitted, and
`setGlobalFailed` was never reached. Settings, Shortcuts sat there showing
`Ctrl+Alt+Shift+E` as if it worked. Both portal calls now watch their method
reply as well as the Request, and an error marks every enabled global shortcut
refused, which is what that page has shown since stage 4 for the X11 BadAccess
case. `mock_portal.py` grew a `REFUSE_SESSION` mode that answers CreateSession
with `org.freedesktop.portal.Error.NotAllowed` and no Response, as both
desktops do, and `run_portal_test.sh` runs a third leg against a second mock in
that mode. Mutation-checked: putting the bare `asyncCall` back fails it.

### What does work on both

| | GNOME on Wayland | Plasma 6 on Wayland |
|---|---|---|
| the window | yes | yes, with the desktop's own decoration |
| the tray icon | yes outside the sandbox, through the `ubuntu-appindicators` extension | yes outside the sandbox, natively |
| the tooltip | `Isotone\n<output> · <preset>` | the same |
| the tray menu | EQ and Mute with their keys, Output with every sink, Preset, Open Isotone, Quit | the same |
| light or dark | follows the desktop | follows it in the Flatpak too, because the KDE runtime carries `xdg-config/kdeglobals:ro` |
| `ctest` | 7 of 7 | 7 of 7 |
| the Background portal | writes the same entry as Cinnamon's, no dialog | the same |

**And one thing that did not work on either, until it was fixed the same day:
the Flatpak's tray icon.** Asking the watcher for
`RegisteredStatusNotifierItems` listed the item when the same build ran outside
the sandbox, on both desktops, with `GetConnectionUnixProcessID` pointing back
at the app; with the Flatpak it listed only kded6's on Plasma and only the
update notifier's on GNOME. Found only because the tray was checked by asking
the watcher rather than by looking at a panel, which is the lesson.

**It takes two lines in the manifest and the manifest had neither.** Qt's
QSystemTrayIcon publishes the item under a bus name of its own,
`org.kde.StatusNotifierItem-<pid>-<n>`, and then hands that name to the
watcher. So a sandbox needs to reach the watcher *and* to own that name:

```
- --talk-name=org.kde.StatusNotifierWatcher
- --own-name=org.kde.StatusNotifierItem-2-1
```

The pid is 2 because the sandbox has a pid namespace of its own and the app is
the first thing in it (`flatpak run --command=sh ... -c 'echo $$'` prints 2),
and the counter is 1 because there is one tray icon. No wildcard covers that
shape: `org.kde.StatusNotifierItem.*` is refused, since `-2-1` is not a `.`
subtree, and `org.kde.*` is granted but is far too broad to ship. Without the
`own-name`, `RequestName` comes back
`org.freedesktop.DBus.Error.ServiceUnknown`, which is how xdg-dbus-proxy
refuses, and Qt carries on and hands the watcher a name it does not own.

**Worth recording the wrong turn**, because it cost an hour and a commit that
had to be taken back. The first fix was the `talk-name` alone; the check said
it had not worked, so it was written up as not fixed. The check was the thing
that was wrong: Isotone is single-instance, so every "restart the app and look
again" was quietly measuring the same old process from before the change. The
rule that came out of it is to `flatpak kill` before every one of these, and
the one that should have applied already is that a negative result from a
measurement needs the measurement checked as hard as a positive one.

Verified on both desktops with the shipped manifest and no overrides: a fresh
install and a fresh launch register an item whose owner is the sandboxed
process, with `Isotone
<output> · <preset>` on it and the whole menu behind
it. On Plasma the owner is pid 5030 of `bwrap ... isotone`, on GNOME pid 3268
of the same.

**The Flatpak follows the desktop's light and dark on Plasma and not on GNOME**,
which is not a contradiction of the entry of 2026-09-20 that made it dark: the
KDE runtime grants `xdg-config/kdeglobals:ro` by default, so on Plasma the
sandboxed app reads the colour scheme and comes up light with the desktop. On
GNOME there is no kdeglobals and it comes up dark, as designed.

The tray was checked over D-Bus rather than by eye:
`org.kde.StatusNotifierWatcher.RegisteredStatusNotifierItems` lists the item,
`GetConnectionUnixProcessID` on it gives the app's own pid, and
`com.canonical.dbusmenu.GetLayout` gives the menu above, with
`toggle-state` on EQ and Mute.

## The icons, the status mark and the output dropdown (2026-09-20)

The owner had someone look at the UI who is redoing the logo. His complaint was
that it reads as generated, and he named the parts: the icon set, the status
dots, the palette, the toggles. Two of those turned into changes; the rest are
recorded and untouched.

### The icons were the conventional drawings

26 icons, all inline SVG in `ui/qml/Icon.qml`, no pack and no licence to worry
about. The problem was never the licence: **20 of the 26 were the conventional
drawing**, hairline strokes on a 24 box with round caps, which is Feather's
idiom. `eq` was Feather `sliders`, `settings` was Feather `settings`, `output`
was `volume-2`.

The sidebar toggle was worse than similar. Lucide draws `panel-left` as a
rounded rect plus a vertical line at x=9. So did we. He picked exactly that one
out as identical to ChatGPT's and Claude's, and he was right in the strongest
sense available.

**The set is now Phosphor Bold** (phosphoricons.com, MIT), used as published.
The alternatives were looked at first: Tabler is 2 px outline, the same idiom
heavier; Heroicons is 316 icons and tied to Tailwind; Hugeicons is mostly paid.
Phosphor is the one that is actually drawn differently, filled geometry rather
than hairline strokes.

It also fits this app for a reason nobody planned. Phosphor Bold's `waveform` is
**five round-capped bars of varying heights about a centre line**, which is the
mark, drawn by someone else, years earlier. No other pack speaks the language the
logo is already in. That icon is now the collapsed rail's Outputs button, where
it beat a third speaker glyph sitting next to `speaker-hifi` and `speaker-high`.

`Icon.qml` carries two branches: Phosphor's filled paths on a 256 box, and the
older stroked 24-box machinery for the two icons that are ours, the mark and the
sidebar toggle. The whole set moved at once on purpose. A half-swapped set, some
filled and some hairline, looks worse than either.

The toggle took four attempts and the reason is worth keeping. The version
before the last one was a hairline panel with a 3.1 bar inside it, and magnified
it looked right. At 18 px it does not: 3.1 against a 1.6 outline is too little
contrast to read as a separate object, so it came out looking like a thick
divider line. The rail is outside the panel now, two shapes, which cannot be
read that way. **Judge an icon at the size it is drawn at**, not in the
inspector.

### The status dot was the tell he named

His own list of things that say "generated" included "green or grey dots". The
Devices table stacks **eleven coloured dots down one column**, which is the
worst instance of it in the app.

`StatusDot` is now a 3 x 14 bar rather than a 6 px circle: the same information
as a channel-strip mark rather than a dashboard LED. One component, eight call
sites, so everything moved together.

`off` draws nothing at all. That status is exactly `not_installed` and
`unplugged` (`ui/src/devicestatus.cpp`), and the old hollow ring was still a dot.
A mark that says "there is nothing here" was the wrong idea; the dimmed row
already says it. An idle output keeps its bar, darkened, so "not playing" still
reads differently from "not there".

The box stays 6 wide so no call site reflows. Two of the eight positioned the old
dot by hand and needed their centres moved, and one of those only showed up in a
render: in the collapsed rail a 14 px bar inherited the 6 px dot's corner-badge
offset and hung off the top of the speaker icon like a stray mark.

### The output list is a dropdown

Shut it is one row; open it discloses the rest in place, growing upward, because
the foot it lives in is anchored to the sidebar's floor, so the divider and
Settings below never move. No "Manage devices" footer: Devices is still in the
nav, so that route was never lost.

Two things came out of building it:

- **One output is not a choice.** With one there is nothing to choose between
  and with none nothing to show, so the caret is hidden and the control is
  inert. A caret that promises something which never happens is worse than no
  caret.
- **The click did nothing at first.** The row inside the shut control kept its
  own `MouseArea`, which took the press and re-selected the output that was
  already current, so the box never opened and the screen never changed. A bug
  with no symptom other than nothing happening. The box is the target now; the
  row is a label.

`OutputList`'s model is a property defaulting to the `Outputs` singleton. That
singleton enumerates the machine and is **empty under Qt Quick Test**, so with no
way to inject one, none of this was reachable by a test at all.

### Recorded, not changed

- **The palette** is the category default and he is right about it: near-neutral
  greys and a mid blue, which is Cursor, VS Code and most audio software. Every
  colour is a token in `ui/qml/Theme.qml`, one file, so a different default is an
  edit to that file and nowhere else. Two cheap moves if it is ever wanted: bias
  the neutrals off neutral, and move accent 1 off blue. Band colours stay.
- **There is no second typeface.** All 191 `font.family` uses in the QML are
  `Theme.font` and the app font is Instrument Sans. What he was reacting to is
  real but different: **there is no type scale.** Ten sizes, with 11, 12, 13, 14
  and 15 doing five nearly identical jobs. Mechanical to fix, not yet done.
- **The toggle** is the iOS pill, which is also the Cursor pill and the macOS
  pill. Consistent with itself, one component everywhere, so this is taste
  rather than a defect.
- **The graph fill** was the accent at 0.30 alpha along the top *and* bottom of
  the plot: a gradient across the whole card rather than a fill under the curve,
  so a boosted band read as a glow around itself. Now 0.17 and 0.03 dark, 0.11
  and 0.015 light.
- **Settings leaves 390 px of empty page** to the right of its content on all
  five tabs, and the sidebar is empty for 509 px between the nav and the output
  control, 57% of the window height. Both measured off the render rather than
  judged by eye. The owner turned down the density work, so both stand.

## The Flatpak's daemon, and one daemon at a time (2026-09-20)

The last thing open in stage 6. The app starts at sign-in in a Flatpak, through
the Background portal, and the daemon did not, so a Flatpak-only machine signed
in with no EQ until someone opened Devices and pressed Start.

The reason it had been left alone was written down here as: making the app start
the daemon "would race the `.deb`'s unit on a machine with both, which his laptop
is". That was the right worry and the wrong conclusion. **The race is the bug**,
not the app starting the daemon, and nothing stopped it before either: the `.deb`
was safe only because systemd will not start a unit twice, and anyone running
`isotone-daemon` twice by hand got two of them.

Two daemons are worse than none. Both create a sink of the same name, both
follow the default sink, and what the UI reads out of the regions is whichever
of them wrote last.

### The lock

`linux/transport/daemon_lock.h`: an advisory exclusive `flock` on an object in
the same POSIX shared memory the regions live in, `shm_open("/isotone. daemon")`
beside `"/isotone.<sink>"`. A space, because no sink key contains one.

That location is the whole point. `/dev/shm` inside a Flatpak is the host's,
which is what `--device=shm` buys and what the regions already depend on, so a
daemon inside the sandbox and one started by the `.deb`'s unit on the host
contend for the *same* lock. A lock under `XDG_RUNTIME_DIR` would not have done
that.

The kernel drops an `flock` when the process dies, however it dies, so a daemon
that is killed leaves nothing behind to clean up. No stale lock file, no pid file
to disbelieve.

A daemon that finds the lock held **says so and exits 0**. Being asked to start
one while one already runs is not a failure: the state asked for is the state
there is. `--allow-second` skips the lock, for tests that want two.

Which makes the app's side trivial and unconditional. In a Flatpak, at startup,
not under `--screenshot`, it starts `isotone-daemon` detached and lets the lock
sort out who wins. Detached because a child would go when the window is closed to
the tray, and the EQ would go with it.

On a machine with both, whichever starts first keeps it and the other leaves
quietly. That is the behaviour the old note wanted and could not have.

### Measured

The binary, not the theory. One daemon running, a second started against the
same sink:

```
first daemon pid: 1114
--- second attempt ---
isotone-daemon: another daemon is already running
exit: 0
daemons now running: 1
```

And the thing it was all for, on the KDE VM with the Flatpak installed for real
and a host build of the daemon beside it:

| first | then | daemons left | which |
|---|---|---|---|
| nothing | the Flatpak app starts | 1 | `/app/bin/isotone-daemon`, in the sandbox |
| a host daemon | the Flatpak app starts | 1 | the host one; the sandboxed one refused |
| the Flatpak's daemon | a host daemon is run | 1 | the sandboxed one; the host one said so and left with 0 |

Both directions, across the sandbox boundary, which is the case the old note
said could not be had.

`flock` belongs to the open file description rather than to the process, so a
second `DaemonLock` contends even from inside the same process. That is what
makes it testable without starting two daemons, and
`linux/transport/tests/test_daemon_lock.cpp` does exactly that: the second
holder is refused with `EWOULDBLOCK` specifically, releasing hands it on,
acquiring twice on one object is not a refusal. Mutating `flock` away so it
never blocks fails two of the four.

### What it cost to find

The Flatpak build failed on the first attempt with `Cannot find source file:
daemon_lock.cpp`. `flatpak-builder` is fed a tree built from `git ls-files`,
which lists tracked files only, so a new file that has not been staged is
invisible to it however complete the working tree looks. Worth remembering: a
Flatpak build can fail for a reason that has nothing to do with the Flatpak.

## A review pass over everything, and five fixes (2026-09-21)

A full read of the tree with every suite run on every target the project has,
plus sanitizers and property probes the suites do not carry. Five defects came
out of it, four in the product and one in a test, each fixed with a check that
fails without the fix.

### What was run, and what it said

Everything already in the project was green on the first pass, which is worth
stating because it is what made the findings findings rather than noise. The one
exception arrived part way through, when the owner's AirPods connected and
`devices_tests` started failing and kept failing; that is the fifth finding
below, and it would have failed the same way on the first run had they been
connected then.

| check | result |
|---|---|
| `ctest` on Windows (MSVC 19.51) | 7 of 7 |
| `ctest` on Linux (WSL, g++ 13.3) | 7 of 7 |
| the APO self test, `check_shm_transport.py`, `gen_reference.py --check`, `gen_icons.py --check` | all pass |
| `ui_tests` 50, `ui_model_tests` 129, `ui_qml_tests` 305 on Windows | pass |
| `linux/ci-audio.sh`: stage 1c, stage 3, stage 4 through a real PipeWire graph | pass, the app's tone at -12.011 against -12.000 |
| the whole tree built and tested on the owner's laptop (Mint 22.3, Qt 6.4.2) | 7 of 7 |

Three things were added for this pass and are not part of the build:

- **ASan and UBSan** over `core_tests`: 213 cases, 2,669,046 assertions, clean.
- **ThreadSanitizer** over the same: one race, and it is the documented one.
  `audio_ring_read`'s `memcpy` of the sample area against the writer's store
  into it is deliberate: the reader copies what may be being written and
  then drops it by comparing `pending_index`, which is how every lock-free audio
  ring works. Nothing else races, the seqlock included. TSan in CI would need a
  suppression for that one frame; it is not there today.
- **Property probes** against the core, in the scratch directory rather than the
  tree: 60,000 rounds of random and adversarial text through `parse_apo_config`,
  `parse_curve`, `fit_curve`, `parse_speaker_setup` and `remap_channels` under
  ASan/UBSan, clean; and 700 random states across five layouts, four rates and
  six frequencies measured through `Processor` and compared with `magnitude_db`,
  plus 1,400 rounds of hostile parameters and 700 of random block splitting.
  Clean, which is the "what is drawn is what is heard" contract holding for the
  engine. It is the drawing that was wrong, below. And 2,500 random states
  through the Equalizer APO backend, each written as a device block beside
  another device's and read back: the curve on every channel within 0.02 dB, the
  speaker setup, mute and bypass unchanged, the other device's bytes untouched
  and not one parse warning.

### The curve was drawn at 48 kHz on every output

`ResponseGraph` designed every band at a fixed `kSampleRate = 48000.0` while the
engine designs them at the output's own rate, which the session already knows
and already uses for the Auto preamp. A filter's shape depends on the rate it is
designed at, so on any output that is not 48 kHz the curve was not the curve
heard. Measured, worst error over 20 Hz to the output's Nyquist:

| band | at 44.1 kHz | at 96 kHz | at 32 kHz |
|---|---|---|---|
| peak 15 kHz Q 4 +9 dB | 0.79 dB | 2.64 dB | 5.46 dB |
| notch 12 kHz Q 10 | 0.82 dB | 2.88 dB | 6.53 dB |
| low pass 18 kHz Q 0.707 | 3.56 dB | 3.89 dB | 0.25 dB |
| peak 1 kHz Q 1 +6 dB | 0.003 dB | 0.014 dB | 0.024 dB |

So it never showed below about 3 kHz and never on a 48 kHz output, which is why
it survived the owner's own use: both his outputs run at 48 kHz. The AirPods in
the Devices list are 44.1 kHz.

The graph now asks the session for the rate, as `autoPreampValue` does. One
thing that follows from it and is worth knowing: the core's `response()` is
unity above Nyquist, so on an output whose rate is under 40 kHz the plot is flat
from its Nyquist to 20 kHz. That is the honest answer, since nothing up there
reaches the output at all, and it is a great deal closer than drawing the
48 kHz shape across the whole range; an 8 kHz Bluetooth headset, which this
machine has in its list while it is unplugged, was being drawn entirely wrong.
Whether that stretch deserves a mark of its own is a question for the owner. The
import preview (`CurvePreview`) drew at 48 kHz for the same reason and now takes
the rate of the output the file is for. Not changed: `filterglyph` and the
Appearance preview, which draw a shape rather than an output, and
`fit_curve`'s own rate, since a preset is for every output unless it is narrowed
and 48 kHz is the neutral choice there.

The test is in `ui_model_tests`: the graph's composite at 16.2 kHz against
`magnitude_db` at 44.1, 48, 96 and 192 kHz, with a check that the rates really
do differ so it cannot pass vacuously. Reverted, it reads 2.94 dB where the
engine plays 5.58 dB.

### A pre-mix IsoAPO reported the post-mix class

`IsoApo` was always constructed with `regPostMixProperties`, and
`ClassFactory::clsid_` was stored and never used, so an instance created as
`ISOAPO_PRE_MIX_GUID` answered `GetRegistrationProperties` with the post-mix
CLSID. The flags in the two registrations are identical, which is why it has
never been felt, but an object that misreports what class it is is wrong and the
factory already knew the answer. The class now reaches the constructor and picks
its own properties. The self test creates one of each and reads the CLSID back.

### The installer did not notice a running Isotone

Windows keeps a running image open for reading and deleting only, so its exe
cannot be opened for writing while the app is up (measured: `File.Open` with
`Write` is denied at every share mode while it runs and succeeds the moment it
exits). Two consequences, neither handled:

- An install over a running one stopped at NSIS's "error opening file for
  writing" with Abort, Retry, Ignore, and Ignore left a stale exe.
- An uninstall ran `machine-uninstall`, unregistered the class, and then left
  `isotone.exe` and the whole Qt runtime in `$INSTDIR`: `RMDir /r` cannot take
  the files that are open and `RMDir /REBOOTOK` will not take a directory that
  still has files in it, so nothing was scheduled to remove them either. The app
  went on running with a tray icon offering an engine that was no longer
  registered.

Both sections now call `CheckNotRunning` first, which opens
`$INSTDIR\isotone.exe` for append and asks the person to quit the app and
click Retry when that is refused; a silent run aborts and says why. `FileOpen`
rather than a process list: it asks exactly the question that matters and needs
no plugin. Checked both ways against a harness built from the macro itself,
with the app running and with it stopped.

### The spectrum took the host's sample rate on trust

`DeviceLink::read_audio` on Windows wrote the shared header's `sample_rate` into
the caller's variable whatever it held; the Linux side already ignored a zero.
A region carries zero until a stream has locked, and the header is in memory any
authenticated user can write, which the ring reader already treats as hostile.
A zero reached `SpectrumAnalyzer::update`, which stored it, and then every bin
index was a frequency divided by zero and `push_silence(sample_rate * elapsed)`
pushed nothing, so a spectrum that had stopped arriving froze instead of falling
at the decay the settings ask for. Both sides now keep the last good rate: the
Windows reader as the Linux one does, and the analyzer itself, which is where it
is divided by. Reverted, the analyzer reads -120 dB where it should read
-1.3 dB.

### `devices_tests` could not pass while the AirPods were connected

Found on the last confirmation run and then reproduced six times out of six,
which is what made it worth chasing rather than writing off as a flake. The
cross-check "endpoints, formats and engines match isotone-devicetool" requires
that every render endpoint `devicetool list` names was also returned by
`enumerate_render_endpoints`. Windows does not guarantee that.

`devicetool` reads `MMDevices\Render`; the library asks the audio API. The
owner's AirPods keep a render key for their Hands-Free endpoint,
`{136126fa-...}`, and the key says `DeviceState = 0x1`, active. Measured with a
scratch probe over the API itself:

```
read_render_endpoint:  0x80070490   (ERROR_NOT_FOUND)
GetDevice(id):         0x00000000   GetState: 0x00000001  (active)
EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL): 35 endpoints, not among them
```

So it is not a stale key: the API will hand that endpoint over by id and call it
active, and leave it out of its own full enumeration. That is what Windows does
with a Bluetooth Hands-Free render endpoint while the device is in A2DP, and it
appeared here only because the AirPods connected part way through the pass.

Following the enumeration is right for the app. An endpoint Windows will not
list is one nothing plays through, and Windows' own Sound settings does not
offer it either; `Outputs` needs `DEVICE_STATE_ACTIVE` and a format from the
library, so no phantom can become an editable output. It is the test that was
asserting something untrue. It now reports such an endpoint and carries on, and
keeps its teeth: the library's two entry points must still agree with each other
(`read_render_endpoint` has to refuse it too), the counts must still add up, and
a library that enumerated nothing at all would leave every endpoint hidden,
which is a failure. 18 of 18, four runs in a row, 35 endpoints compared and the
one reported.

**Left alone, deliberately** (owner, 2026-09-21). `read_render_endpoint` is
built on the same enumeration, so it refuses an endpoint `GetDevice` would
resolve: `devicetool status {136126fa-...}` describes the endpoint and
`devicetool layouts` on the same GUID in the same second answers "no such render
endpoint". Asking `GetDevice` first would make the by-id path agree with
Windows, and it is what `check_speaker_layout` and `set_speaker_layout` resolve
through. Three things argue against doing it: it is unreachable from the app,
since `Outputs` and `Devices` both list from the enumeration and a hidden
endpoint therefore never becomes a row with a Test button or a layout picker;
the only way to reach it is to type the GUID into `isotone-devicetool` by hand;
and no test can be written that fails without the change, because the case needs
Windows to be of two minds about a real endpoint and cannot be synthesised.
Recorded here so the next person who meets it knows it is known.

### The application ID is renamed for the account the repository is under

`io.github.isotone.Isotone` reads as `github.com/isotone`, which is not the
owner's and is not one he wants (owner, 2026-09-21). The ID is now
`io.github.jackwangxyw.Isotone`, which is the repository's own account and the
form Flathub asks for if it is ever submitted there.

Done now rather than after `v0.1.0` because the ID is the app's identity on
Linux and a rename after a release is a different app to everyone who has it:
their old Flatpak stays installed, and the autostart entry the Background portal
wrote keeps starting it under an ID that resolves to nothing, which is silently
no global hotkeys.

Twelve files renamed (the desktop entry, the AppStream metadata, the Flatpak
manifest and nine icon theme files) and the string changed in
`ui/CMakeLists.txt`, `tools/gen_icons.py`, `ui/backend/autostart_xdg.h`,
`CLAUDE.md` and four test files. `<developer id>` in the metadata was already
`io.github.jackwangxyw`, so it now agrees with the component ID instead of
contradicting it.

The one piece of real work was the upgrade path. `autostart_xdg.h` already knew
one old entry name, `isotone.desktop` from before 2026-09-20, read as a fallback
and removed on any write. That is now a list, newest first, with
`io.github.isotone.Isotone.desktop` at its head, and `startup.cpp` reads the
whole list and removes the whole list. So a `.deb` install that had launch at
sign-in on keeps it on, and the stale entry goes the first time the toggle is
touched.

**A Flatpak cannot do that for itself.** The host's autostart directory is
mounted read-only (`--filesystem=xdg-config/autostart:ro`) and the entry belongs
to the Background portal, which only ever writes its own. So an existing
sandboxed install leaves `io.github.isotone.Isotone.desktop` on the host, and
uninstalling the old Flatpak does not take it: it has to be deleted by hand,
once, on each machine that has one.

The one rough edge, and it is the same one the previous rename had: outside a
Flatpak the app reads an old entry as "on" and reports the toggle on, but does
not rewrite it until the toggle is touched. So a `.deb` or build-tree install
that had launch at sign-in on keeps starting under the old entry, and its
application ID resolves to nothing, so global hotkeys do not bind until the
toggle is used once. Migrating at startup instead was considered and left: in a
Flatpak the write is a portal call that can prompt, so an app cannot do it
unasked, and doing it on one platform and not the other is worse than doing it
nowhere.

### The renamed Flatpak, verified at a real sign-in on both desktops

The packaging was rebuilt and the machines were moved over rather than left for
later (owner, 2026-09-21).

| check | result |
|---|---|
| `.deb` rebuilt, contents listed | ships the desktop entry, the metadata and all nine icons under the new name |
| `lintian` | clean but for the three deliberate `systemctl` warnings |
| `appstreamcli validate` | the ID is accepted; only the two known unreachable-URL warnings, which want the repository public |
| `desktop-file-validate` | clean |
| Flatpak rebuilt and bundled under the new ID | exports `io.github.jackwangxyw.Isotone.{desktop,metainfo.xml}` |

Then the three machines. The laptop had the old Flatpak and, because launch at
sign-in had been turned back off there on 2026-09-20, no autostart entry to
clean up; the new bundle installed and runs. Both VMs had the old Flatpak
**and** a stale entry, and the two were not the same thing, which is worth
recording: KDE's was the Background portal's (`X-XDP-Autostart`, running
`flatpak run`), GNOME's was the unsandboxed app's own, naming itself for the
application ID but running `/home/jackw/build/ui/isotone --tray`.

On both, the old Flatpak was removed, the new one installed, the stale entry
deleted, and launch at sign-in turned on again through the new Flatpak. The
portal wrote `io.github.jackwangxyw.Isotone.desktop` on each. Then a real power
cycle, and at sign-in:

- **Plasma**: the app came up in its sandbox, started its own daemon, which
  created the region and linked both channels; the tray item registered; and
  `xdg-desktop-portal-kde` logged `BindShortcuts` with all four shortcuts and
  their triggers. No `NotAllowed`, which is what a shell-started app still gets.
- **GNOME**: the same, and the journal names the process
  `io.github.jackwangxyw.Isotone.desktop[2548]`, so the session knows it by the
  new ID; `org.gnome.Settings.GlobalShortcutsProvider` was activated, which is
  the path that works there.

So the rename carries the whole stage 6 Linux story with it, checked rather than
assumed. Both VMs were powered off again afterwards. GNOME now autostarts the
Flatpak rather than the build tree, and its `~/build` predates the rename, so a
build-tree check there wants the tree synced again first.

One aside worth keeping: `systemctl reboot` over ssh is refused by polkit on
these VMs ("interactive authentication required") and returns 0, so it looks
like it worked and nothing happens. Two minutes went into reading a state that
had never rebooted. Power cycle them with VirtualBox's ACPI button instead, and
answer Plasma's confirmation with Enter.

### Looked at and left alone

- `speaker_channel`'s bit walk looked like it could run for ever at the top bit.
  It cannot: `bit` reaches the speaker bit exactly and the loop ends. Only a
  multi-bit argument above 0x80000000 would wrap, and every caller passes one of
  eight fixed single bits. No change.
- `EqSession::nextBandId` can return an id already in use, but only for a state
  carrying both `UINT32_MAX` and `UINT32_MAX - 1`, which nothing but a crafted
  param block produces, and the processor is specified to tolerate shared ids.
  No change.
- The Flatpak's `--own-name=org.kde.StatusNotifierItem-2-1` is exact rather than
  general. It holds because the app makes one `QSystemTrayIcon`, on the stack,
  shown once and never hidden, and the sandbox's pid namespace makes the app
  pid 2. Worth knowing it is a pair of facts rather than a rule.
- The application ID is `io.github.isotone.Isotone` while the repository is
  `github.com/jackwangxyw/Isotone`. The reverse-DNS form claims the GitHub
  account `isotone`. It is the owner's call, and it is the kind of thing that is
  cheap now and expensive after a release: the ID names the Flatpak, the desktop
  file, the icon, the autostart entry the portal resolves and the D-Bus names.
- `gen_icons.py --check` is in CLAUDE.md's local list and not in CI.

### Two things about the owner's own machines

- The laptop's installed `.deb` is the 0.1.0 built before 2026-09-20: its unit
  still has `PrivateDevices=true` and its daemon has no lock. `PrivateDevices`
  is harmless on systemd 255, which is what Mint 22.3 has, so nothing there is
  broken; the missing lock is. A second daemon started by hand ran alongside the
  first, made a second virtual sink of the same name, and on exit unlinked the
  region the first was still using, which is exactly what the lock was added to
  stop. The current build, synced and built on the laptop for this pass, refuses
  the second daemon and leaves with 0. He wants the new `.deb` when he next
  installs one.
- The daemon was inactive on the laptop because the session has been up since
  2026-09-08 and the unit was enabled on 2026-09-19: a symlink does not start a
  unit in a user manager that is already running. It starts at his next sign-in.
  Started by hand for this pass and stopped again, and the region it left behind
  was removed; the lock object stays, as designed.

The GNOME and KDE VMs were started for this pass and powered off again. On
Plasma the Flatpak was up from its own sign-in with its tray icon, its own
daemon and the lock in `/dev/shm`, which is the whole stage 6 Flatpak story
working unattended.
`systemctl --user --machine=<uid>@.host`, which `prerm` uses, answers on
systemd 259 as well as 255. The Mint VM was started as well and never left the
VirtualBox splash, which is the stall of 2026-09-19 again; nothing was done in
it and it was powered off.

## Check for updates on startup (2026-09-20)

Added before 0.1.0, because a 0.1.0 without it cannot tell anyone about 0.2.0.
The owner's decisions: all three builds check, including the `.deb` and the
Flatpak that a package manager also updates; a newer release shows a notice that
links to its page, nothing is downloaded; on by default, with Check for updates
on startup in Settings, General; and a start in the tray stays silent until the
window is opened.

- `UpdateCheck` (`ui/src/updatecheck.h`) asks
  `api.github.com/repos/jackwangxyw/Isotone/releases/latest` once per start. A
  release counts when it is not a draft or a pre-release and its tag is
  `v?X.Y.Z`, compared as numbers (0.10.0 is after 0.9.0). The page it opens is
  built from the tag, not taken from the reply. A failed request finds nothing
  and says why on stderr; GitHub answers 404 until the first release exists.
- `UpdateNotice.qml`: the owner's mock (approved 2026-09-20), 320 px, 24 px in
  from the bottom right of the window, over whatever is there. It shows once a
  release is found and the window is visible, fades out after 8 s, held while
  the pointer is over it; Later, close and View release put it away until the
  next start.
- The Flatpak gains `--share=network`, which it had not needed until now.
- `--update-url <url>` points the check elsewhere, `file://` included, and runs
  it under `--screenshot`, which otherwise checks nothing.

Measured: the app reaches GitHub over HTTPS from the Windows build (Schannel,
already shipped as `tls/qschannelbackend.dll`) and the Linux one (OpenSSL,
Qt 6.4.2), and from inside the Flatpak built with the new permission
(`shared=network;ipc;` in its metadata), and gets the 404 that curl gets. `test_updatecheck.cpp` (5 cases)
and `tst_updatenotice.qml` (6) cover it, and five mutations (the window
ignored, the pointer ignored, no fade, a textual version comparison, a
pre-release accepted) each fail a test.

## An upgrade could not replace the loaded engine (2026-09-21)

The first upgrade trial, on the owner's machine, stopped at NSIS's "Error
opening file for writing: C:\Program Files\Isotone\IsoAPO.dll". audiodg has
the engine loaded on every output it is on, and Windows does not open a loaded
image for writing. The running-app check only asks about `isotone.exe`, so it
passed. The first install never met this because there was no old DLL.

Measured with a DLL held by `LoadLibrary`: it cannot be opened for writing, it
can be renamed, a new file can be written at its path, and the renamed one
cannot be deleted until it is unloaded (error 5). So the installer now moves the
old engine aside to a temporary name in `$INSTDIR` (`move_aside.nsh`), copies
the tree, registers, and on an upgrade restarts Windows audio so the new engine
is what plays, then deletes the old file or leaves it to the next boot. A first
install moves nothing and restarts nothing.

`windows/setup/tests/test_move_aside.py` builds a test installer around the same
macro and runs it unelevated against a loaded DLL: 10 checks. With the macro
made a no-op, 5 fail, and one of them shows a second defect the fix also
closes: a silent install (`/S`) over a loaded engine exited 0 and kept the old
DLL without a word. The test needs NSIS, so it is not in CI.

The directory page loses its "Program Files only" sentence (owner): its top
text is a single space, as the licence page's is, because an empty one makes
NSIS show its own paragraph.

## The lock tests took the daemon's own lock (2026-09-21)

Resyncing the GNOME VM after the release, `transport_posix_tests` failed there:
all four `test_daemon_lock.cpp` cases at their first `acquire()`. The tests used
the daemon's lock, `/isotone. daemon`, and the Flatpak's daemon, up since the
VM's sign-in, held it. So they failed on every machine with a daemon running
(the owner's laptop, both Wayland VMs) and passed only where none runs, which is
CI. The product was fine; the tests were not isolated.

`DaemonLock` takes a name now (`explicit DaemonLock(std::string)`; the default
constructor keeps the daemon's), the tests use `/isotone. daemon test`, and a
fifth case checks that the two do not contend. Measured in WSL with the daemon's
lock held by a live `flock` holder: 4 failures before, 0 after, and 0 with no
holder; then on both VMs with their Flatpak daemons running, all 7 suites green.

# Where things stand (2026-09-21)

## 0.1.0 is released

`v0.1.0` is tagged on `ab557f2` and published as a full release (not a
pre-release, so `releases/latest` returns it and the update check sees it):
https://github.com/jackwangxyw/Isotone/releases/tag/v0.1.0. The three assets
were downloaded back from GitHub and match what was built and tested:

| Asset | SHA-256 |
|---|---|
| `isotone-0.1.0-setup.exe` | `fc0788aac13f5212ed3888ddaf7da0deca4739594d5bcdebe69671b32fe3fd3f` |
| `isotone-0.1.0-Linux.deb` | `94498828a79fa9a7b887a356128e902bca172dd2f7a8d9942d83ccaa0f290469` |
| `isotone-0.1.0.flatpak` | `8d0a956d6a0d2ec6bc38481ecd03e2bb8d063097cc7b551051044a9ffbd69077` |

The installer is the one the owner upgraded his machine with, byte for byte
(NSIS rebuilt it identically). The Flatpak's source copy matched `ab557f2` file
for file apart from CRLF (below). `appstreamcli validate` passes; its one
pedantic note is the uppercase letter in the application ID, which stays.
Release notes are the owner's.

Every release after this one has to be a full release too: the update check
ignores pre-releases, so a 0.2.0 marked as one would not reach anyone on 0.1.0.

## Open

- **Windows launch at sign-in**, the one stage 6 item left: whether Windows runs
  the Run value at a sign-in. The value is right (`"C:\Program
  Files\Isotone\isotone.exe" --tray`). The owner checks at a later restart.
- **The laptop's and the Mint VM's `~/Isotone`** still predate the rename. The
  laptop did not answer over Tailscale on 2026-09-21; the Mint VM was not
  started, because it takes 12 GB and 16.7 GB was free. GNOME and KDE were
  resynced to `v0.1.0` that day (the rename's 12 stale files removed, the rest
  replaced from `git archive`), plus the lock-test fix, and both build and pass
  all 7 suites.
- **Nine working-tree files on this machine are CRLF on disk** although
  `.gitattributes` asks for LF: `CMakeLists.txt`, `core/CMakeLists.txt`,
  `core/tests/test_audio_ring.cpp`, `core/tests/test_dynamics.cpp`,
  `linux/daemon/daemon.cpp`, `linux/daemon/measure.py`, and the
  `CMakeLists.txt` of `windows/compat`, `windows/devicetool` and
  `windows/transport`. Git normalises them, so commits are LF and nothing builds
  differently; a copy of the working tree (rather than `git archive`) carries the
  CRLF.
- **Qt 6.4 prints about 20 "Cyclic dependency detected" warnings** at every
  start on Linux, between `GeneralSettings.qml` or `PresetActions.qml` and other
  files of the module. Present at `181913a`, before this session's changes; not
  investigated.
- **Not in CI:** `gen_icons.py --check`, and `windows/setup/tests/
  test_move_aside.py`, which needs NSIS.
- **The capture blocklist** (excluding applications and outputs from the
  daemon's capture) is 0.2.0.

Settled and not to be reopened without a reason: `read_render_endpoint`, the
QtQuick.Controls styles, and the one unexplained `ui_model_tests` flake.

## Done

| Stage | State | Evidence |
|---|---|---|
| 0. Repo, CI, decisions log | complete | CI workflow builds MSVC + GCC with warnings as errors, runs tests and the APO self test |
| 1a. Compat backend spike | complete | measured differential matched the analytic filter to 0.001 dB |
| 1b. Fork spike (IsoAPO) | complete | measured in audiodg to 0.0002 dB rms |
| 1c. Linux spike | complete | the PipeWire topology measured at -12.000 against -12.000 (2026-09-17), in CI since 2026-09-18 |
| 2. Core | complete | 212 cases green on MSVC 19.51 and GCC 16.1.0 (curve import added 2026-09-15) |
| 3. Hosts on shared memory | Windows: transport measured in audiodg; devicetool installed IsoAPO on CABLE Input; delay, polarity and mute measured in audiodg; compat backend merged and measured against the installed Equalizer APO; every speaker feature measured live at 7.1 in both backends. Windows side complete. Linux: the daemon, measured in CI (2026-09-17 and 18); capturing applications' streams (2026-09-19); stereo only | live curve matched scipy to 0.0001 dB rms through the region; ring exact; the final review's compat changes matched the core live within 0.0004 dB |
| 4. UI | complete | every screen of the prototype except EQ by ear, in Qt 6 Quick (`ui/`); reviewed and fixed over 2026-09-15 and 16 from the owner's own use, on his real output as well as the cable. `ui_tests` 42, `ui_model_tests` 110, `ui_qml_tests` 267; `docs/notes/stage4-*.md`; the entries of 2026-09-14, 15 and 16 |
| 4. UI, Linux | complete on Cinnamon under X11, in the VM and on the owner's own laptop; GNOME, KDE and Wayland not checked live, and after packaging (owner, 2026-09-19) | the whole Qt layer on Qt 6.4.2; `measure_linux.py` in CI (the app's edits, saved state and EQ by ear's tone through the daemon, to 0.011 dB); `ui_tests` 42, `ui_model_tests` 91, `ui_qml_tests` 232 (+49 Windows-only skipped), X11 and portal hotkey tests; the desktop checks by hand on both machines, and the fixes they turned up (the entries of 2026-09-19) |
| 5. EQ by ear | complete: the sweep, approved by the owner in use; A/B set aside for later (owner) | the tone measured through IsoAPO live (level to 0.004 dB, no step past the sine's own slope, a band gain drag without a click, the sweep at 1.0001 oct/s); `ui_tests` 50, `ui_model_tests` 119, `ui_qml_tests` 290; the entries of 2026-09-16 from "Stage 5 begins" |

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
                       processor (TDF-II + smoothing + crossfades, speaker
                       routing, bass management, delay), APO config
                       parser/formatter, param block schema
                       and the audio ring
core/tests/            10 test files; reference data from scipy at 4 sample rates
tools/gen_reference.py independent scipy implementation that generates it
tools/check_shm_transport.py  cross-process transport check, runs in CI
windows/transport/     the named shared mapping and the per-endpoint saved state
windows/apo/           IsoAPO.dll, IsoAPO-selftest.dll, the self test
windows/measure/       isotone-measure: endpoint list, stepped-sine measurement; measure_tests
windows/shmtool/       isotone-shm: status / write / persist / forget / capture
windows/devicetool/    isotone-devicetool, with upstream's registration code vendored
windows/devices/       isotone_devices: endpoints, format, engine, notifications; devices_tests
windows/compat/        isotone-compat: the Equalizer APO backend (Isotone.txt)
docs/decisions.md      this file
docs/ui-spec.md        the stage 4 build brief
```

Local only (gitignored): `docs/design/screens/*.png` (the approved screens),
`docs/design/generator/` (regenerates them), and `.vscode/settings.json`, which
points IntelliSense at `build/compile_commands.json`.

## Not started, in dependency order

**Stage 3** remaining:

1. **Linux daemon**: done and measured, through "The Linux daemon finished to the
   edge of stage 4" (2026-09-17), and since then capturing applications' streams
   and stereo only (2026-09-19).

**Stage 4 on Linux is done** as far as one desktop can show it: the Qt layer, the
tests and the measurements are in CI, and every desktop check passed on Cinnamon
under X11 (2026-09-19). Left for Linux: GNOME and KDE, and Wayland, which need
their VMs (docs/notes/linux-vm-setup.md); packaging (a .deb first, then Flatpak).
Applications captured by the daemon, as EasyEffects does, is left as it is; the
layout picker was dropped for stereo only (2026-09-19).

**Stage 4 is done.** The owner ran the whole list on his machine on 2026-09-16: a
real install, repair and uninstall from Devices with the Windows prompt;
attaching config.txt; the tray and its menu; import, export and a drag from
Explorer; and the default output change, which turned up the preset scope work of
that day. Launch at sign-in is verified as far as it can be without signing out:
the Run value is right, the command it holds starts the app into the tray, and
the toggles write and remove it correctly (checked against a sandboxed key, the
real one untouched). Windows actually running that key at sign-in is the one
thing left, for whenever he next restarts.

The open decisions were settled on 2026-09-16: global hotkeys on by default,
preset edits reaching other outputs on save, and output names left as the devices
give them.

**Stage 5** (EQ by ear): the sweep is done (2026-09-16), with what the owner added
while using it: New preset, adding to a peak already there, the channel taken from
the top bar, a press on the graph moving the tone. Along the way, from his use:
short windows (Settings, General, Short window), always on top, the graph repainting
only the spectrum on each frame (the maximized lag), the frequency grid's three
weights, and the placeholder icon. **A/B is set aside for later** (owner): plan 8.4
and the `EqByEarAB` board are its design if it is wanted.
**Stage 6** (packaging) is built and measured on all three targets
(2026-09-20). Versions are `x.y.z`; the first release is **0.1.0**, not yet
tagged.

| | State |
|---|---|
| Windows installer | NSIS 3.12, six pages, no output page (first run picks outputs). Run for real on the owner's machine: registered, ACL set, ARP entry, uninstaller. Measured through the installed engine at 0.0001 dB |
| `.deb` | 1.6 MB, lintian clean but for three deliberate warnings. Daemon enabled on install. Measured on the owner's laptop through `/usr/bin/isotone-daemon` at -0.0000 dB |
| Flatpak | `io.github.isotone.Isotone`, needs `--device=shm` (plan question 5, answered below). Measured across the sandbox boundary at -0.0000 dB |

All HKLM and ACL work is in `isotone-devicetool` (`machine-install`,
`machine-uninstall`), each with `--dry-run` and tests, so the NSIS script only
copies files and calls it.

**Left in stage 6, in the order they matter:**

1. ~~The Flatpak does not start at login.~~ Done, both halves. The app starts
   through the Background portal (2026-09-20; "Launch at sign-in in a Flatpak"),
   measured in the installed Flatpak on the owner's laptop and again on both new
   VMs. ~~The daemon does not.~~ It does now: the app starts it, and the race
   this note used to worry about is gone because the daemon takes a lock in the
   shared `/dev/shm` and a second one leaves with 0 ("The Flatpak's daemon, and
   one daemon at a time").
2. **Launch at sign-in on Windows** still needs one sign-out to confirm Windows
   runs the Run value. The value itself is right and the app is installed now.
   The owner's recollection (2026-09-21) is that a prerelease did try to start
   at a sign-in and was stopped by a Qt error, which is not written down
   anywhere and is worth reading carefully: it says Windows *does* run the Run
   value, and that whatever ran then could not find its Qt runtime. A build
   directory started from the Run key has no Qt beside it; the installed tree
   does, because `cmake --install` runs windeployqt. So the likely reading is
   that the mechanism works and the old failure was the path, not the key. It
   is still one sign-out from being known rather than inferred.
3. ~~GNOME, KDE and Wayland, one VM each; none built.~~ Both built and run
   overnight on 2026-09-20 (docs/notes/linux-vm-setup.md, and "GNOME and KDE,
   on Wayland"). Four things came out of it. Two are fixed and tested: the UI
   backend not compiling on PipeWire 1.6, and the daemon's unit not starting on
   systemd 259. Two are open:
   - ~~The Flatpak has no tray icon.~~ Fixed: it wanted a `--talk-name` and an
     `--own-name`, and verified on Plasma with the shipped manifest.
   - ~~Global hotkeys on Wayland want an app scope.~~ Fixed: the autostart
     entry is named for the application ID, which is what the portal resolves
     to a desktop file. Measured firing at a real sign-in on both desktops.
     What is still refused, correctly, is an app started from a shell, which
     lands in `session-N.scope` and has no application ID at all; that now says
     so on the Shortcuts page instead of showing a live-looking key.

   Plasma on X11 no longer exists to test: Kubuntu 26.04 ships a Wayland
   session only.
4. **`v0.1.0`**, when the owner says. The repository should be public first: the
   AppStream metadata points at it, and `appstreamcli` warns the URLs are
   unreachable until it is.

**Open, not stage 6:**

- **The capture blocklist** is 0.2.0 (owner, 2026-09-20). The daemon moves every
  application's playback into its sink and there is no way to exclude one, or an
  output; EasyEffects has both, and two programs that capture every stream
  cannot share a machine. The owner has EasyEffects installed.
- **One unexplained flake**: `ui_model_tests` failed once in six runs on
  2026-09-20 and passed five more; which case failed was not captured. Hunted on
  2026-09-20 and **not reproduced**: 150 runs of `ui_model_tests` offscreen on
  Linux, 150 on Windows, and 60 more of all three ctest variants (offscreen, the
  X11 one under Xvfb and the portal one on its own bus) while two VMs were
  installing on the same machine, which is the loaded case a timing flake would
  like. 570 runs, no failure. Still open, and now with the search recorded so
  the next attempt starts somewhere else.
- **The QtQuick.Controls styles**, about 9 MB, ship in the Windows installer and
  are probably unused: they arrive through QtQuick.Dialogs, whose FileDialog is
  native on Windows. The owner's call was to leave them.
- ~~**The application ID claims an account that is not the repository's**.~~
  Settled the same day: it is `io.github.jackwangxyw.Isotone` now. What is left
  is one manual step per machine that has the old Flatpak, below.
- **`gen_icons.py --check` is not in CI** (2026-09-21), though it is in
  CLAUDE.md's local list. An icon that stops matching the mark is caught only by
  whoever runs it by hand.

**Laptop brought up to date (2026-09-21).** The `.deb` built from this tree is
installed: 0.1.0, the daemon carries the one-at-a-time lock, and the unit no
longer sets `PrivateDevices` (only the comment saying why remains;
`systemctl --user show` reads `PrivateDevices=no`). Enabled, inactive, so it
starts at his next sign-in. The Flatpak there is
`io.github.jackwangxyw.Isotone` now. `~/Isotone-src`, a second tree synced and
built there for that pass, was removed again; `~/Isotone` stays and is a copy
of 2026-09-19, so it wants a resync before it is built.

**State of the owner's machines.** Windows: Isotone installed at
`C:\Program Files\Isotone` by the installer, on two outputs (below). Laptop: the
`.deb` installed and enabled, the Flatpak installed (reinstalled 2026-09-20 with
the Background portal in it), the KDE 6.11 runtime installed, screen blanking
and lock turned off with `~/Isotone/.work/power-restore.sh` ready to put them
back. His Flatpak's launch at sign-in was turned on to measure it and **turned
back off**, because of the tray bug above: with it on he would sign in to an
Isotone with no window and no tray icon, which is a process he cannot reach
rather than a feature. The toggle in Settings, General turns it on again when
the tray is fixed.

The laptop's `.deb` is the 0.1.0 built before 2026-09-20 (2026-09-21): its unit
still carries `PrivateDevices=true`, which systemd 255 accepts, and its daemon
has no lock, which a hand-started second daemon proved by running alongside the
first and unlinking its region on the way out. Nothing is broken by it as long
as only the unit starts the daemon, and the next `.deb` he installs fixes both.
The current tree is built there as `~/Isotone-src/build`, where all seven suites
pass on Qt 6.4.2 and the second daemon leaves with 0.

Mint VM: the `.deb` installed, powered off. **Two new VMs**, *Isotone GNOME*
and *Isotone KDE*, built on 2026-09-20 and left powered off; `ssh isotone-gnome`
and `ssh isotone-kde` start working as soon as they are started. Both carry the
tree, a build, the daemon's unit pointing at it, the rig's null sinks and the
0.1.0 Flatpak. Nothing on them is installed system-wide except the desktop
itself. Mint VM: the `.deb`
installed, powered off, and a build-directory user unit moved aside to
`~/isotone-daemon.service.build-dir.bak`.

Left for Linux after packaging: GNOME, KDE and Wayland, one VM each
(docs/notes/linux-vm-setup.md). Two smaller things, neither started:

- ~~A stream another program has aimed elsewhere is left alone, and nothing
  looks at it again when that program lets go.~~ Fixed in stage 6
  (2026-09-19); the `reclaim` case in `linux/daemon/measure.py` covers it.
- Two programs that both capture every stream cannot share a machine. Isotone
  has no setting for its capture, where EasyEffects has one, a per-application
  blocklist and a per-output one. Still open, and a feature rather than a bug:
  0.2.0 material, to decide with the owner.

## State of the owner's machine

**IsoAPO is installed on two outputs** (2026-09-20, after the installer was run
for real):

- **CABLE Input**, MFX (`,6`), by devicetool with `--replace-equalizerapo`. SFX
  (`,5`) is empty: Equalizer APO no longer runs on CABLE Input (its pre-mix
  class there had been applying `peace.txt`). Nothing the owner listens to
  routes through the cable.
- **Headphones** (Anker USB Audio), EFX, mode `SFX_EFX`. The two vendor APOs it
  came with are still in SFX and MFX. This is the owner's real output, from his
  own use of the app during stages 4 and 5.

Until 2026-09-20 this section said CABLE Input only, which was true when it was
written and had stopped being true: the headphones were added by the owner's own
use, and the stale note was caught by `machine-uninstall` enumerating the
endpoints rather than by anyone reading it. CABLE Output, the capture side,
still has Equalizer APO. Every other endpoint is untouched.

`C:\Program Files\Isotone` is now a **real install**, put there by
`isotone-0.1.0-setup.exe`: the whole staged tree, `IsoAPO.dll` at 0.1.0 with the
static CRT, an Add/Remove Programs entry in the 64-bit view, and
`%ProgramData%\IsoAPO` carrying the installer's ACL, so the app can write a
saved state unelevated whichever account wrote it last. It is no longer the
hand-staged DLL of the backend review.

`%ProgramData%\IsoAPO\devices` holds both outputs' saved state, neither touched
by the install and uninstall. CABLE Input's install record predates
`Isotone.InstallMode`; `status` derives MFX from the slot, and a repair after a
detach needs `--mode mfx`.

**CABLE Input and CABLE Output are stereo again** (2 channels, 24-bit, 48 kHz,
`0x3`; float32 stereo mix format), set back on 2026-09-14 by the owner with the
same `IPolicyConfig::SetDeviceFormat` call that made them 7.1, and checked in
the registry and with `devicetool status`. A multichannel live test sets them
to 7.1 first.

To remove it, elevated:
`.\build\windows\devicetool\isotone-devicetool.exe uninstall '{798436d2-8c71-4834-9248-00ccbaaca00a}'`
then `Restart-Service Audiosrv -Force`. Since the owner ran the record takeover
(2026-09-13), Equalizer APO's record for CABLE Input is gone and IsoAPO's record
says every slot was empty before either EQ; the uninstall dry run deletes `,6`
and the record and leaves CABLE Input with no effects. The older
`windows/apo/IsoAPO-backup-Render-798436d2-....reg` still holds Equalizer APO's
classes if they are wanted back.

## Standing rules

- No commits or pushes without the owner saying so each time.
- Never modify the owner's live Equalizer APO configuration.
- Never hand the owner registry code that has not been run.
  `isotone-devicetool install|uninstall|repair --dry-run` and `roundtrip` run
  every check unelevated with no side effects.
