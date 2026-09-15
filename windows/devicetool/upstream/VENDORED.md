# Vendored from Equalizer APO

Source: https://github.com/mirror/equalizerapo
Commit: 53d885f7f1a097b457e17a5206b7d60f647877a8 (2024-09-27 13:59:15 +0000)

Every file here carries upstream's header, "GNU General Public License ...
either version 2 of the License, or (at your option) any later version", and
`License.txt` is upstream's GPL v2 text. Line endings were converted from CRLF
to LF to match the repository; nothing else changed except as listed.

| File | Upstream path |
|---|---|
| `DeviceAPOInfo.cpp`, `DeviceAPOInfo.h` | top level |
| `AbstractAPOInfo.cpp`, `AbstractAPOInfo.h` | top level |
| `helpers/RegistryHelper.cpp`, `helpers/RegistryHelper.h` | `helpers/` |
| `helpers/StringHelper.cpp`, `helpers/StringHelper.h` | `helpers/` |
| `helpers/ServiceHelper.cpp`, `helpers/ServiceHelper.h` | `helpers/` |
| `helpers/PrecisionTimer.h` | `helpers/`, included by `ServiceHelper.cpp` |
| `stdafx.h` | top level, modified |
| `License.txt` | top level |

## Modifications

- `stdafx.h`: no longer includes `helpers/ScopeGuard.h`, which pulls in folly's
  `UncaughtExceptions.h` (Apache-2.0, no GPL header). Defines an equivalent
  `SCOPE_EXIT`, the only construct the vendored code uses from it.
- `helpers/RegistryHelper.h`:
  - `APP_REGPATH` is `HKEY_LOCAL_MACHINE\SOFTWARE\IsoAPO`.
  - Added `EQUALIZERAPO_REGPATH` (upstream's old `APP_REGPATH` value) and
    `ISOAPO_PRE_MIX_GUID` / `ISOAPO_POST_MIX_GUID`. The `EQUALIZERAPO_*` GUIDs
    are kept for detecting an existing install.
  - Added the `RegistryDryRun` interface and `RegistryHelper::dryRun`,
    including `RegistryDryRun::readMultiValue`.
- `helpers/RegistryHelper.cpp`: when `dryRun` is set, `writeValue`,
  `writeDWORDValue`, both `writeMultiValue`s, `deleteValue`, `createKey`,
  `deleteKey`, `makeWritable`, `takeOwnership` and `saveToFile` report to it
  and return without touching the registry or the file system; `readValue`,
  `readMultiValue`, `keyExists`, `valueExists` and `keyEmpty` ask it first.
  Leaks fixed, which a CLI called repeatedly by a UI would otherwise
  accumulate: `takeOwnership` closes its token and key handles, `makeWritable`
  its key handle, and both free their SID, ACL and security descriptors on
  every path, throws included (`SCOPE_EXIT` guards); `readValue`,
  `readDWORDValue`, `readMultiValue` and both `writeMultiValue`s free their
  `new[]` buffers with `delete[]` instead of `delete`. Each of those writes
  also reports itself to `RegistryHelper::log`, when one is installed, after it
  returns without throwing (`RegistryWriteReport`, `RegistryLog`); `createKey`
  does not report a key that already existed. `readValue` returns an empty
  string for a zero-length value instead of indexing before its buffer. Each
  line is marked `Isotone modification`.
- `DeviceAPOInfo.cpp`:
  - `EQUALIZERAPO_PRE_MIX_GUID` / `EQUALIZERAPO_POST_MIX_GUID` replaced by the
    `ISOAPO_*` GUIDs throughout (install detection, the values written, and
    `checkAPORegistration`).
  - `VoicemeeterAPOInfo.h` is not included and `loadAllInfos` no longer calls
    `VoicemeeterAPOInfo::prependInfos`.
  - The FxProperties title written for a newly created key is `IsoAPO`.
  - `checkAPORegistration(true)` would register `IsoAPO.dll`, not
    `EqualizerAPO.dll`.

## Upstream behaviour worth knowing

- `INSTALL_SFX_MFX` deletes the endpoint's LFX and GFX values, and
  `INSTALL_LFX_GFX` deletes SFX, MFX and EFX; the originals are kept in the
  `Child APOs` record and restored by `uninstall()`.
- `saveToFile` writes `[HKEY_LOCAL_MACHINE\` followed by a key that already
  begins `HKEY_LOCAL_MACHINE\`, so its `.reg` backup names a doubled root and
  would not import as written. `uninstall()` restores from the `Child APOs`
  record, not from that file. (Read in the source, not executed.)
- `load()` throws if a `Child APOs` record has a `Version` other than `2`.
- `ServiceHelper::restartService` (unmodified; read in the source, not executed
  here): stops the service's active dependents, then the service, and starts
  them in reverse. One timer covers the whole restart, so stopping and starting
  every service share 30 seconds. A service found `START_PENDING` is never sent
  a stop and times out. A service still not running 5 seconds into the restart
  is started again, and `StartService` on a service that is by then
  `START_PENDING` fails with `ERROR_SERVICE_ALREADY_RUNNING`, which throws. The
  `Service` constructor computes `desiredAccess` from `allowEnumerate` and then
  opens the service with `SERVICE_ENUMERATE_DEPENDENTS` regardless.
