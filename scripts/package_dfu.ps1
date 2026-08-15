# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Package a built image as a Nordic DFU zip for the nRF52840 Dongle.

.DESCRIPTION
    The nRF52840 Dongle (PCA10059) has no mass-storage bootloader, so there is
    no UF2 to drag anywhere. Its onboard USB bootloader speaks Nordic's DFU
    protocol and takes a signed-or-unsigned zip package, which is what
    `nrfutil nrf5sdk-tools pkg generate` produces and what the nRF Connect for
    Desktop Programmer app expects.

    The application carries no SoftDevice - the ANT stack is a linked library -
    so the package declares --sd-req 0x00, meaning "runs against whatever is
    there, requires no SoftDevice".

    Before packaging, the hex is checked to start at 0x1000. That is where this
    board's bootloader hands control over. A Feather image starts at 0x26000
    and would package without complaint into a zip that installs cleanly and
    then does nothing at all, which is a miserable thing to debug on a board
    with no debugger on it.

    ASCII only, deliberately: Windows PowerShell 5.1 reads .ps1 files as ANSI
    unless they carry a BOM, so non-ASCII characters here become parse errors.

.PARAMETER HexPath
    The application hex to package. Defaults to the dongle release build.

.PARAMETER OutPath
    Where to write the zip. Defaults to dist\radiant_dongle_nrf52840dongle.zip.

.PARAMETER AppVersion
    Application version integer stamped into the package manifest. Derived from
    the repository-root VERSION file when not given, and NOT defaulted to a
    constant - see the Get-RadiantAppVersion comment for why that mattered.

.PARAMETER ExpectedOffset
    Address the image must start at. Only change this if you know why.

.EXAMPLE
    .\scripts\package_dfu.ps1

.EXAMPLE
    .\scripts\package_dfu.ps1 -HexPath build\dongle\merged.hex -AppVersion 2
#>
[CmdletBinding()]
param(
    [string]$HexPath = 'build\dongle\dongle\zephyr\zephyr.hex',
    [string]$OutPath = 'dist\radiant_dongle_nrf52840dongle.zip',
    $AppVersion = $null,
    [int]$ExpectedOffset = 0x1000,
    [string]$NrfutilPath
)

$ErrorActionPreference = 'Stop'

# Derive the DFU application version from the repository-root VERSION file.
#
# THIS IS A BUG FIX, NOT HYGIENE. Until 2026-08-14 this parameter defaulted to
# the literal 1 and the release workflow passed a literal 1 as well, so every
# DFU package this project has ever published declared itself version 1. The
# nRF52840 Dongle's bootloader compares that number against the installed one
# to refuse a downgrade; a constant defeats the check outright, and it does so
# silently - the package installs, the board runs, and nothing anywhere reports
# that the protection is inert.
#
# It THROWS rather than falling back. A fallback here would restore exactly the
# failure it replaces: a package that looks versioned and is not. A missing or
# malformed VERSION file is a broken build, and it should read as one.
function Get-RadiantAppVersion {
    $versionFile = Join-Path (Split-Path -Parent $PSScriptRoot) 'VERSION'
    if (-not (Test-Path $versionFile)) {
        throw "No VERSION file at $versionFile. It is the single source of truth for the DFU application version; refusing to guess."
    }

    $fields = @{}
    foreach ($line in (Get-Content $versionFile)) {
        if ($line -match '^\s*([A-Z_]+)\s*=\s*(.*?)\s*$') {
            $fields[$Matches[1]] = $Matches[2]
        }
    }
    foreach ($required in @('VERSION_MAJOR', 'VERSION_MINOR', 'PATCHLEVEL')) {
        if (-not $fields.ContainsKey($required) -or $fields[$required] -eq '') {
            throw "VERSION is missing $required. Expected Zephyr's VERSION format."
        }
    }

    # Monotonic and readable in a manifest: 0.1.0 -> 100, 1.2.3 -> 10203.
    # Each field is capped at 99 by the caps below, so the encoding cannot
    # collide (0.1.0 and 0.0.100 would otherwise both be 100).
    [int]$major = $fields['VERSION_MAJOR']
    [int]$minor = $fields['VERSION_MINOR']
    [int]$patch = $fields['PATCHLEVEL']
    foreach ($pair in @(@('VERSION_MINOR', $minor), @('PATCHLEVEL', $patch))) {
        if ($pair[1] -lt 0 -or $pair[1] -gt 99) {
            throw ("VERSION field {0} is {1}; this packaging encodes it in two decimal digits, so it must be 0-99. Widen the encoding here and in scripts\check_version.py together, or the two will disagree." -f $pair[0], $pair[1])
        }
    }

    return ($major * 10000) + ($minor * 100) + $patch
}

if ($null -eq $AppVersion) {
    $AppVersion = Get-RadiantAppVersion
}
[int]$AppVersion = $AppVersion
Write-Host "DFU application version: $AppVersion (from VERSION)"

if (-not (Test-Path $HexPath)) {
    throw "No hex at $HexPath. Build it first:`n" +
          "  west -z C:\ncs\v3.2.4\zephyr build -s <app> -d <app>\build\dongle -b nrf52840dongle/nrf52840 -p always"
}

# Locate nrfutil. It ships inside the toolchain bundles rather than on PATH.
if (-not $NrfutilPath) {
    $NrfutilPath = (Get-Command nrfutil -ErrorAction SilentlyContinue).Source
}
if (-not $NrfutilPath) {
    $candidates = Get-ChildItem 'C:\ncs\toolchains' -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'nrfutil\bin\nrfutil.exe' } |
        Where-Object { Test-Path $_ }
    $NrfutilPath = @($candidates)[0]
}
if (-not $NrfutilPath) {
    throw "nrfutil.exe not found. It ships in the NCS toolchain bundles under <bundle>\nrfutil\bin, or pass -NrfutilPath."
}

# `pkg` lives in the nrf5sdk-tools command, which is a separate install.
#
# Do not inherit NRFUTIL_HOME from the build environment. scripts\env.ps1
# applies the toolchain bundle's environment.json, which points NRFUTIL_HOME at
# <bundle>\nrfutil\home - a home that carries the commands west needs, does not
# carry nrf5sdk-tools, and is deliberately locked against installing anything.
# Packaging a DFU zip has nothing to do with the Zephyr toolchain, so clear the
# variable and let nrfutil use the user's own home, where nrf5sdk-tools lives.
# Inheriting it made this script try to install into a locked directory and
# fail, on a machine that already had the command.
$savedHome = $env:NRFUTIL_HOME
$env:NRFUTIL_HOME = $null

try {
    # Probe by invoking the subcommand, not by parsing `nrfutil list`. What
    # `list` reports depends on which home is in effect and does not reliably
    # answer "can I run this"; the exit code does.
    & $NrfutilPath nrf5sdk-tools --help | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Installing the nrf5sdk-tools nrfutil command (provides 'pkg')..."
        & $NrfutilPath install nrf5sdk-tools
        if ($LASTEXITCODE -ne 0) {
            throw ("nrfutil install nrf5sdk-tools failed with exit code $LASTEXITCODE. " +
                   "If it reports a locked home directory, that is the toolchain " +
                   "bundle's home rather than yours - check NRFUTIL_HOME.")
        }
    }

# Walk the Intel hex far enough to learn the first data address. Type 00 is
# data, and the first one is where the image begins; the two record types
# before it carry the high bits of the address.
#
# Both types really do occur here. Zephyr writes zephyr.hex with type 02
# (extended segment address, value shifted left by 4) while Partition Manager
# writes merged.hex with type 04 (extended linear address, shifted by 16).
# Handling only one of them silently yields a base of 0 - which reads as a
# plausible-looking address rather than an error.
$base = 0
$start = $null
foreach ($line in (Get-Content $HexPath)) {
    if ($line -notmatch '^:') { continue }
    $type = [Convert]::ToInt32($line.Substring(7, 2), 16)
    if ($type -eq 2) {
        $base = [Convert]::ToInt32($line.Substring(9, 4), 16) -shl 4
    } elseif ($type -eq 4) {
        $base = [Convert]::ToInt32($line.Substring(9, 4), 16) -shl 16
    } elseif ($type -eq 0) {
        $start = $base + [Convert]::ToInt32($line.Substring(3, 4), 16)
        break
    }
}
if ($null -eq $start) {
    throw "No data records in $HexPath - is it a valid Intel hex file?"
}
if ($start -ne $ExpectedOffset) {
    throw ("$HexPath starts at 0x{0:X} but this board's bootloader hands over at 0x{1:X}. " -f $start, $ExpectedOffset) +
          "That looks like an image built for a different board - check you passed -b nrf52840dongle/nrf52840."
}
Write-Host ("Image starts at 0x{0:X}, as expected." -f $start)

$outDir = Split-Path -Parent $OutPath
if ($outDir -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}

# --hw-version 52 is the nRF52 family. --sd-req 0x00 says the application needs
# no SoftDevice present, which is true: the ANT stack links into the image.
& $NrfutilPath nrf5sdk-tools pkg generate `
    --hw-version 52 `
    --sd-req 0x00 `
    --application $HexPath `
    --application-version $AppVersion `
    $OutPath
if ($LASTEXITCODE -ne 0) {
    throw "nrfutil pkg generate failed with exit code $LASTEXITCODE."
}

$zip = Get-Item $OutPath
Write-Host ""
Write-Host ("Wrote {0} ({1:N0} bytes)" -f $zip.FullName, $zip.Length)
Write-Host ""
Write-Host "To flash:"
Write-Host "  1. Insert the dongle and press its side RESET button. The red LED"
Write-Host "     should pulse - that is the bootloader waiting."
Write-Host "  2. nRF Connect for Desktop -> Programmer -> select the device,"
Write-Host "     Add file -> $($zip.Name) -> Write."
Write-Host ""
Write-Host "  Or from the command line, with the dongle in bootloader mode:"
Write-Host "    $NrfutilPath nrf5sdk-tools dfu usb-serial -pkg $OutPath -p <COMx>"

} finally {
    # Put the caller's environment back. build_all.ps1 runs west after this.
    $env:NRFUTIL_HOME = $savedHome
}
