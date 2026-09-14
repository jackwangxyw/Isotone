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
  skipped with a warning, as upstream's loader does.
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
a band the remap disables keeps its old mask; `format_apo_config` (export) does
not remap; a render stream that stalls without an error is not detected by
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

**Noted from the live run, not changed:** the CLI sets a state's layout and the
device's together (`--channels/--mask`), so a state for another layout was
written through the library; attach adds a blank line after a file that already
ends in a newline; attach's `EndIf:` also reaches devices a narrower `Device:`
line excluded, where upstream would log "EndIf without If!" and carry on;
`isotone-measure` records only the kept attempt's glitches, not why a retaken
attempt failed.

**Tests:** `core_tests` 199 cases (MSVC 19.51 and GCC 16.1, warnings as errors),
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

**Unexplained, not IsoAPO:** with a stream whose buffers are flagged silent,
one cable capture in four held a single stale sample of an earlier tone on all
8 channels; a ring capture over the same moment held none. It comes from after
IsoAPO (the engine or VB-Cable).

The Equalizer APO config dir matched its snapshot after every batch;
`%ProgramData%\IsoAPO\devices` is empty; no region is held.

---

# Where things stand (end of 2026-09-13)

## Done

| Stage | State | Evidence |
|---|---|---|
| 0. Repo, CI, decisions log | complete | CI workflow builds MSVC + GCC with warnings as errors, runs tests and the APO self test |
| 1a. Compat backend spike | complete | measured differential matched the analytic filter to 0.001 dB |
| 1b. Fork spike (IsoAPO) | complete | measured in audiodg to 0.0002 dB rms |
| 1c. Linux spike | deferred | no Linux environment on this machine; owner's decision |
| 2. Core | complete | 199 cases green on MSVC 19.51 and GCC 16.1.0 after the final backend review |
| 3. Hosts on shared memory | Windows: transport measured in audiodg; devicetool installed IsoAPO on CABLE Input; delay, polarity and mute measured in audiodg; compat backend merged and measured against the installed Equalizer APO; every speaker feature measured live at 7.1 in both backends. Windows side complete. Linux daemon deferred with 1c | live curve matched scipy to 0.0001 dB rms through the region; ring exact; the final review's compat changes matched the core live within 0.0004 dB |
| 4. UI | designed (17 screens), Qt 6 Quick chosen, not coded | `docs/ui-spec.md`, `docs/design/screens/*.png` |

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

1. **Linux daemon**, deferred with 1c.

**Stage 4** is the UI in Qt 6 Quick, designed and not coded. Start with
`docs/ui-spec.md`, "Start here". It does not wait on stage 3's remaining items:
the core, transport, compat, devices library and devicetool it needs exist, and
the multichannel controls can be built UI-first. The backend review before it is
the entry "Final backend review before stage 4"; the UI's contracts are in
`ui-spec.md`.

**Stage 5** (EQ by ear) is designed with the UI; its screens are in the spec.
**Stage 6** (packaging) is not started.

## State of the owner's machine

**IsoAPO is installed on CABLE Input only**, by devicetool with
`--replace-equalizerapo`, in MFX (`,6`). SFX (`,5`) is empty: Equalizer APO no
longer runs on CABLE Input (its pre-mix class there had been applying
`peace.txt`). CABLE Output, the capture side, still has Equalizer APO. Every
other endpoint is untouched. Nothing the owner listens to routes through the
cable. The staged `C:\Program Files\Isotone\IsoAPO.dll` is the final backend
review's build (SHA-256 EE9AECEE…8567, param block v5); the one before it is
beside it as `IsoAPO.previous.dll`. `%ProgramData%\IsoAPO\devices` holds no
saved state. CABLE Input's
install record predates `Isotone.InstallMode`; `status` derives MFX from the
slot, and a repair after a detach needs `--mode mfx`.

**CABLE Input and CABLE Output are 7.1** (8 channels, 24-bit, 48 kHz, `0x63F`),
set for the multichannel measurement; they were 2 channels, 24-bit, `0x3`.
CABLE In 16ch is unchanged at 2 channels. To go back, Sound control panel:
CABLE Input > Configure > Stereo, and CABLE Output > Properties > Advanced >
2 channel, 24 bit, 48000 Hz.

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
