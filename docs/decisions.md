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

---

# Where things stand (2026-09-16)

## Done

| Stage | State | Evidence |
|---|---|---|
| 0. Repo, CI, decisions log | complete | CI workflow builds MSVC + GCC with warnings as errors, runs tests and the APO self test |
| 1a. Compat backend spike | complete | measured differential matched the analytic filter to 0.001 dB |
| 1b. Fork spike (IsoAPO) | complete | measured in audiodg to 0.0002 dB rms |
| 1c. Linux spike | deferred | no Linux environment on this machine; owner's decision |
| 2. Core | complete | 212 cases green on MSVC 19.51 and GCC 16.1.0 (curve import added 2026-09-15) |
| 3. Hosts on shared memory | Windows: transport measured in audiodg; devicetool installed IsoAPO on CABLE Input; delay, polarity and mute measured in audiodg; compat backend merged and measured against the installed Equalizer APO; every speaker feature measured live at 7.1 in both backends. Windows side complete. Linux daemon deferred with 1c | live curve matched scipy to 0.0001 dB rms through the region; ring exact; the final review's compat changes matched the core live within 0.0004 dB |
| 4. UI | complete | every screen of the prototype except EQ by ear, in Qt 6 Quick (`ui/`); reviewed and fixed over 2026-09-15 and 16 from the owner's own use, on his real output as well as the cable. `ui_tests` 42, `ui_model_tests` 110, `ui_qml_tests` 267; `docs/notes/stage4-*.md`; the entries of 2026-09-14, 15 and 16 |
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
   edge of stage 4" (2026-09-17). Stage 1c, the daemon, the transport, the audio
   ring, following the default sink, a rate change and channels to 7.1 are all
   done and measured; CI has a linux-host job that runs both measurements under a
   PipeWire of its own. What is left on the Linux side is item 4 (the UI's Windows
   layer) and item 5 (packaging). The Mint VM is wanted when the UI port starts.

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
**Stage 6** (packaging) is not started.

## State of the owner's machine

**IsoAPO is installed on CABLE Input only**, by devicetool with
`--replace-equalizerapo`, in MFX (`,6`). SFX (`,5`) is empty: Equalizer APO no
longer runs on CABLE Input (its pre-mix class there had been applying
`peace.txt`). CABLE Output, the capture side, still has Equalizer APO. Every
other endpoint is untouched. Nothing the owner listens to routes through the
cable. The staged `C:\Program Files\Isotone\IsoAPO.dll` is the final backend
review's build (SHA-256 EE9AECEE…8567, param block v5); the owner deleted the
previous DLL's backup. `%ProgramData%\IsoAPO\devices` holds CABLE
Input's saved state from the owner's use of the app on it (2026-09-15) and the
headphones'. CABLE Input's
install record predates `Isotone.InstallMode`; `status` derives MFX from the
slot, and a repair after a detach needs `--mode mfx`.

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
