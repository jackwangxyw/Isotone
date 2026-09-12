# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# Installs or removes IsoAPO on ONE named audio endpoint. Must be run elevated.
#
#   .\install.ps1 -Endpoint '{798436d2-...}' -Dll C:\path\IsoAPO.dll
#   .\install.ps1 -Endpoint '{798436d2-...}' -Uninstall
#
# This is the stage 1 spike installer, not the shipping one. The shipping path is
# `devicetool` (plan 5.2), which vendors upstream Equalizer APO's RegistryHelper
# rather than reimplementing registry handling. In particular this script assumes
# BUILTIN\Administrators already holds SetValue on the endpoint's FxProperties
# key, which is true for many endpoints but not all: some are locked to
# TrustedInstaller and need an ownership takeover first.
#
# The FxProperties key is exported to a .reg next to this script before anything
# is written, and -Uninstall replays that file.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Endpoint,

    [string] $Dll,

    [switch] $Uninstall,

    # Runs every check and prints every intended write, but changes nothing and
    # does not require elevation. Exists so this script can be tested before it
    # is pointed at a real machine.
    [switch] $DryRun,

    [ValidateSet('Render', 'Capture')]
    [string] $Flow = 'Render',

    # Where the DLL is staged before registration. It CANNOT be registered from a
    # build tree under the user's profile: audiodg.exe hosts the APO as
    # LocalService, which has no access to user directories (plan 5.4), so the
    # load fails and IAudioClient::Initialize returns E_ACCESSDENIED (0x80070005)
    # for the whole endpoint. Program Files grants BUILTIN\Users ReadAndExecute
    # by inheritance, which is what Equalizer APO relies on too.
    [string] $InstallDir = (Join-Path $env:ProgramFiles 'Isotone'),

    [string] $BackupDir = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Registry layout
#
# The APO CLSIDs live under PKEY_FX_* (audioenginebaseapo.h):
#     ,1 PreMixEffectClsid   (LFX)   ,2 PostMixEffectClsid (GFX)
#     ,5 StreamEffectClsid   (SFX)   ,6 ModeEffectClsid    (MFX)
#     ,7 EndpointEffectClsid (EFX)
#
# Do NOT confuse that with {d3993a3f-99c2-4402-b5ec-a92a0367664b}, whose ,5/,6/,7
# are PKEY_SFX/MFX/EFX_ProcessingModes_Supported_For_Streaming: REG_MULTI_SZ
# lists of signal-processing *modes*, not APOs. They normally hold
# {C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}, which is
# AUDIO_SIGNALPROCESSINGMODE_DEFAULT and appears on every endpoint. An earlier
# version of this script mistook those for APO slots and overwrote them, which
# silently disabled the endpoint's effects rather than replacing them. This
# script never writes to that key, and refuses to write any slot that is not
# already a REG_SZ CLSID.
$FxKey = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}'

# Which slot takes which of our two CLSIDs, mirroring how Equalizer APO installs.
$SlotPlan = [ordered]@{
    5 = @{ Label = 'StreamEffectClsid (SFX)'; Clsid = '{F1DFFD14-9A30-45C5-BAB2-C820C7EC718F}' }
    6 = @{ Label = 'ModeEffectClsid (MFX)';   Clsid = '{BAF30F18-9FA2-4E55-97D9-007CEA179824}' }
}

# Windows resolves an effect CLSID through here, not through
# HKLM\SOFTWARE\Classes\CLSID. A missing entry means the effect is skipped with
# no error anywhere, which is why Equalizer APO works with no COM registration.
$ApoRegRoot = 'HKLM:\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects'

# ---------------------------------------------------------------------------
# Minimal-rights registry writes
#
# Set-ItemProperty cannot be used on these keys. The PowerShell registry provider
# opens a key for writing with KEY_WRITE, which is SetValue + CreateSubKey +
# STANDARD_RIGHTS_WRITE. FxProperties grants Administrators only
# "SetValue, ReadKey", so the *open* is refused before any value is touched and
# the error reads like a privilege problem rather than an over-broad request.
# reg.exe import fails the same way. Opening with exactly SetValue + QueryValues
# works.

function Open-FxKeyForWrite {
    param([string] $SubKeyPath)
    $rights = [System.Security.AccessControl.RegistryRights] 'SetValue, QueryValues'
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
        $SubKeyPath,
        [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
        $rights)
    if ($null -eq $key) { throw "Could not open HKLM\$SubKeyPath for writing" }
    return $key
}

function Set-RegistryStringMinimal {
    param([string] $SubKeyPath, [string] $Name, [string] $Value)
    $key = Open-FxKeyForWrite $SubKeyPath
    try {
        $key.SetValue($Name, $Value, [Microsoft.Win32.RegistryValueKind]::String)
    } finally { $key.Close() }
}

function Set-RegistryMultiStringMinimal {
    param([string] $SubKeyPath, [string] $Name, [string[]] $Value)
    $key = Open-FxKeyForWrite $SubKeyPath
    try {
        # The cast matters: .NET wants a String[] for MultiString and PowerShell
        # would otherwise hand it an Object[], which throws "the type of the
        # value object did not match the specified RegistryValueKind".
        $key.SetValue($Name, [string[]] $Value, [Microsoft.Win32.RegistryValueKind]::MultiString)
    } finally { $key.Close() }
}

function Show-FxProperties {
    param([string] $SubKeyPath)
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($SubKeyPath)
    try {
        foreach ($n in ($key.GetValueNames() | Sort-Object)) {
            Write-Host ("  {0,-13} {1,-44} {2}" -f $key.GetValueKind($n), $n,
                        (@($key.GetValue($n)) -join ' '))
        }
    } finally { $key.Close() }
}

# Copies the DLL somewhere audiodg can actually read it, and proves it can.
function Install-ApoBinary {
    param([string] $Source, [string] $Dir, [switch] $WhatIfOnly)

    $dest = Join-Path $Dir (Split-Path $Source -Leaf)
    if ($WhatIfOnly) {
        Write-Host "  would copy $Source"
        Write-Host "          to $dest"
        return $dest
    }

    if (-not (Test-Path $Dir)) {
        New-Item -ItemType Directory -Path $Dir -Force | Out-Null
        Write-Host "  created $Dir"
    }
    Copy-Item -LiteralPath $Source -Destination $dest -Force
    Write-Host "  copied to $dest"

    # audiodg runs as LocalService. Program Files normally grants BUILTIN\Users
    # ReadAndExecute by inheritance, which covers it, but verify rather than
    # assume: getting this wrong breaks the endpoint rather than failing loudly.
    $acl = Get-Acl $dest
    $canRead = $acl.Access | Where-Object {
        $_.AccessControlType -eq 'Allow' -and
        $_.FileSystemRights.ToString() -match 'ReadAndExecute|FullControl|Read' -and
        $_.IdentityReference -match 'BUILTIN\\Users|Everyone|LOCAL SERVICE|ALL APPLICATION PACKAGES'
    }
    if (-not $canRead) {
        Write-Host "  granting LOCAL SERVICE read access explicitly"
        $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
            'NT AUTHORITY\LOCAL SERVICE', 'ReadAndExecute', 'Allow')
        $acl.AddAccessRule($rule)
        Set-Acl -Path $dest -AclObject $acl
    } else {
        Write-Host "  readable by: $(($canRead.IdentityReference | Select-Object -Unique) -join ', ')"
    }
    return $dest
}

function Assert-Elevated {
    $principal = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "This script must be run from an elevated prompt."
    }
}

function Get-EndpointPath {
    param([string] $Guid, [string] $Direction)
    if ($Guid -notmatch '^\{[0-9a-fA-F-]{36}\}$') {
        throw "Endpoint must be a GUID in braces, e.g. {798436d2-8c71-4834-9248-00ccbaaca00a}"
    }
    $path = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\$Direction\$Guid"
    if (-not (Test-Path $path)) { throw "No such endpoint: $path" }
    return $path
}

function Get-EndpointName {
    param([string] $Path)
    $p = Get-ItemProperty "$Path\Properties" -ErrorAction SilentlyContinue
    $desc = $p.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
    $name = $p.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
    return "$desc ($name)"
}

# Replays a .reg export written by `reg export`, using minimal-rights writes.
# Handles the two value types these keys actually use: REG_SZ, and REG_MULTI_SZ
# which `reg export` writes as hex(7).
function Restore-FromRegFile {
    param([string] $File, [string] $SubKeyPath)

    $raw = [System.IO.File]::ReadAllText($File, [System.Text.Encoding]::Unicode)

    # reg export wraps long hex values: a trailing backslash then a newline.
    $joined = [regex]::Replace($raw, '\\\r?\n[ \t]*', '')

    $restored = 0
    foreach ($line in [regex]::Split($joined, '\r?\n')) {
        $line = $line.Trim()

        $m = [regex]::Match($line, '^"([^"]+)"="(.*)"$')
        if ($m.Success) {
            $name  = $m.Groups[1].Value
            $value = $m.Groups[2].Value.Replace('\\', '\')
            Set-RegistryStringMinimal -SubKeyPath $SubKeyPath -Name $name -Value $value
            Write-Host ("  REG_SZ       {0,-44} {1}" -f $name, $value)
            $restored++
            continue
        }

        $m = [regex]::Match($line, '^"([^"]+)"=hex\(([0-9a-fA-F])\):(.*)$')
        if ($m.Success) {
            $name = $m.Groups[1].Value
            $kind = $m.Groups[2].Value
            if ($kind -ne '7') {
                Write-Warning "skipping $name : hex($kind) not handled by this restore"
                continue
            }
            $bytes = [byte[]] @(
                [regex]::Split($m.Groups[3].Value, ',') |
                    Where-Object { $_.Trim() -ne '' } |
                    ForEach-Object { [Convert]::ToByte($_.Trim(), 16) })
            $text  = [System.Text.Encoding]::Unicode.GetString($bytes)
            $parts = @([regex]::Split($text, "`0") | Where-Object { $_ -ne '' })
            Set-RegistryMultiStringMinimal -SubKeyPath $SubKeyPath -Name $name -Value $parts
            Write-Host ("  REG_MULTI_SZ {0,-44} {1}" -f $name, ($parts -join ' '))
            $restored++
            continue
        }
    }
    return $restored
}

# ---------------------------------------------------------------------------

if (-not $DryRun) { Assert-Elevated } else { Write-Host "DRY RUN: nothing will be written.`n" }

$endpointPath = Get-EndpointPath -Guid $Endpoint -Direction $Flow
$fxPath       = "$endpointPath\FxProperties"
$relative     = $fxPath.Substring('HKLM:\'.Length)
$backupFile   = Join-Path $BackupDir ("IsoAPO-backup-{0}-{1}.reg" -f $Flow, $Endpoint.Trim('{','}'))

Write-Host "Endpoint : $(Get-EndpointName -Path $endpointPath)"
Write-Host "Key      : $fxPath"
Write-Host "Backup   : $backupFile"
Write-Host ""

if ($Uninstall) {
    if (-not (Test-Path $backupFile)) {
        throw "No backup at $backupFile. Refusing to guess the original values."
    }

    Write-Host "Before:"
    Show-FxProperties $relative
    Write-Host ""
    if ($DryRun) {
        Write-Host "DRY RUN: would replay $backupFile into the key above."
        return
    }
    Write-Host "Replaying the backup:"
    $n = Restore-FromRegFile -File $backupFile -SubKeyPath $relative
    if ($n -eq 0) { throw "Parsed no values out of $backupFile" }

    Write-Host ""
    Write-Host "After:"
    Show-FxProperties $relative

    # Unregister whatever is actually registered, which is the staged copy under
    # InstallDir rather than whatever -Dll was passed.
    $stagedGuess = Join-Path $InstallDir 'IsoAPO.dll'
    if (Test-Path $stagedGuess) {
        Write-Host ""
        Write-Host "Unregistering $stagedGuess ..."
        regsvr32.exe /s /u $stagedGuess
        if ($LASTEXITCODE -ne 0) { Write-Warning "regsvr32 /u returned $LASTEXITCODE" }
        # audiodg may still have the DLL mapped until the audio service
        # restarts, so deletion can legitimately fail here. Say so rather than
        # swallowing it: a stale binary is harmless once unregistered, but it
        # should not silently look like it was cleaned up.
        try {
            Remove-Item -LiteralPath $stagedGuess -Force -ErrorAction Stop
            Write-Host "Removed the staged binary."
        } catch {
            Write-Warning ("Could not delete $stagedGuess (probably still mapped by " +
                           "audiodg). It is unregistered and inert. Delete it after " +
                           "Restart-Service Audiosrv -Force if you want it gone.")
        }
    }

    Write-Host ""
    Write-Host "Restored $n value(s). Now run: Restart-Service Audiosrv -Force"
    return
}

if (-not $Dll) { throw "Install needs -Dll pointing at IsoAPO.dll" }
if (-not (Test-Path $Dll)) { throw "No such file: $Dll" }
$Dll = (Resolve-Path $Dll).Path

# 1. Back up before touching anything. Never overwrite an existing backup: the
#    first one holds the pristine values.
if (Test-Path $backupFile) {
    Write-Host "Backup already exists, keeping it (it holds the pristine values)."
} else {
    $regPath = $fxPath -replace '^HKLM:', 'HKEY_LOCAL_MACHINE'
    reg.exe export $regPath $backupFile /y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "reg export failed with $LASTEXITCODE" }
    Write-Host "Backed up FxProperties."
}

# 2. Register the DLL. On its own this changes nothing about how audio is
#    processed; no endpoint references the new CLSIDs until step 4.
#
#    A non-zero exit is not automatically fatal. regsvr32 reports failure if
#    DllRegisterServer returns any failure HRESULT, and an older IsoAPO build
#    returns one when the APO is already registered. What matters is whether the
#    machine ends up in the right state, so that is what gets checked.
Write-Host "Staging the DLL where audiodg can read it ..."
$staged = Install-ApoBinary -Source $Dll -Dir $InstallDir -WhatIfOnly:$DryRun

Write-Host "Registering $staged ..."
if ($DryRun) {
    Write-Host "  (dry run: skipping regsvr32)"
    $regsvrExit = 0
} else {
    regsvr32.exe /s $staged
    $regsvrExit = $LASTEXITCODE
}
if ($regsvrExit -ne 0) {
    Write-Warning "regsvr32 returned $regsvrExit; verifying the registration directly."
}

# 3. Refuse to proceed unless everything looks the way it should.
foreach ($entry in $SlotPlan.GetEnumerator()) {
    $clsid = $entry.Value.Clsid

    if (-not (Test-Path (Join-Path $ApoRegRoot $clsid))) {
        throw "$clsid is not registered under $ApoRegRoot (regsvr32 exit $regsvrExit)."
    }

    # And the COM entry must point at the DLL we were asked to install, not at
    # some stale path from an earlier build in a different directory.
    $inproc = "HKLM:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32"
    if (-not (Test-Path $inproc)) {
        throw "$clsid has no InprocServer32 entry (regsvr32 exit $regsvrExit)."
    }
    $registeredPath = (Get-ItemProperty $inproc).'(default)'
    if (-not $DryRun -and $registeredPath -ne $staged) {
        throw "$clsid points at`n  $registeredPath`nbut the staged binary is`n  $staged"
    }
}
if ($regsvrExit -ne 0) {
    Write-Host "Registration is correct despite the exit code; continuing."
}

$probe = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($relative)
try {
    foreach ($entry in $SlotPlan.GetEnumerator()) {
        $name = "$FxKey,$($entry.Key)"
        $val = $probe.GetValue($name)
        if ($null -eq $val) { throw "$name does not exist on this endpoint." }
        $kind = $probe.GetValueKind($name)
        if ($kind -ne [Microsoft.Win32.RegistryValueKind]::String) {
            throw "$name is $kind, expected String. Not an effect slot; refusing to write."
        }
        if ($val -notmatch '^\{[0-9a-fA-F-]{36}\}$') {
            throw "$name holds '$val', which is not a CLSID. Refusing to write."
        }
    }
} finally { $probe.Close() }

Write-Host ""
Write-Host "Before:"
Show-FxProperties $relative

# 4. Point the effect slots at IsoAPO. Nothing else is written.
foreach ($entry in $SlotPlan.GetEnumerator()) {
    $name = "$FxKey,$($entry.Key)"
    if ($DryRun) {
        Write-Host ("  would set {0,-44} {1}  [{2}]" -f $name, $entry.Value.Clsid, $entry.Value.Label)
    } else {
        Set-RegistryStringMinimal -SubKeyPath $relative -Name $name -Value $entry.Value.Clsid
    }
}

if ($DryRun) {
    Write-Host ""
    Write-Host "DRY RUN complete: every check passed and nothing was written."
    return
}

Write-Host ""
Write-Host "After:"
Show-FxProperties $relative

$after = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($relative)
try {
    $wrong = 0
    foreach ($entry in $SlotPlan.GetEnumerator()) {
        if ($after.GetValue("$FxKey,$($entry.Key)") -ne $entry.Value.Clsid) { $wrong++ }
    }
    if ($wrong -gt 0) { throw "$wrong slot(s) did not take the new value. Restore with -Uninstall." }
} finally { $after.Close() }

Write-Host ""
Write-Host "Installed. Now run: Restart-Service Audiosrv -Force"
Write-Host "Binary  : $staged"
Write-Host "To undo: .\install.ps1 -Endpoint '$Endpoint' -Uninstall"
