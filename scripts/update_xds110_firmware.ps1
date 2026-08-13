# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Update an onboard XDS110 debug probe's firmware, deliberately and with the
    version written down.

.DESCRIPTION
    This exists because the alternative is typing xdsdfu by hand, and
    [[jlink-manual-commands-trap]] is this bench's standing record of what that
    costs. Every probe-firmware write this project makes goes through here, and
    the version it wrote goes in the log above.

    WHY IT IS NEEDED AT ALL. UniFlash's dslite reaches the LAUNCHXL-CC26X2R1
    over cJTAG and enumerates its Cortex_M4_0 core, and then refuses:

        fatal: IcePick_C: Error connecting to the target: (Error -263 @ 0x0)
        A firmware update is required for the XDS110 debug probe.

    The probe on this bench shipped at 3.0.0.13. The XDS emulation package's
    debug drivers want newer, and UniFlash 9.6.0 ships firmware_3.0.0.43.bin
    next to its own xdsdfu. Those two - the firmware image and the xdsdfu that
    writes it - must come from the SAME tree, which is why this script derives
    both from one -Uscif directory rather than taking two paths.

    WHY IT IS RECOVERABLE, which is the part worth knowing before running it.
    The XDS110's boot loader is a separate image at a separate address and this
    does not touch it. A write that fails leaves the device enumerating in DFU
    mode (PID 0xbef4 rather than 0xbef3), and re-running this script finds it
    there and writes again. The way to lose the probe is to write a boot
    loader, which this script has no path to do.

    A NOTE ON TI'S OTHER UTILITIES. `dbgjtag -S integrity` printed "Updating
    the XDS110 firmware ... complete." on this bench without being asked - an
    unrequested write of exactly the kind this script exists to make explicit.
    Do not run uscif tools casually.

.PARAMETER Uscif
    Directory holding xdsdfu.exe and firmware_<version>.bin. Defaults to the
    newest UniFlash install's copy, because that is the one whose debug drivers
    asked for the update.

.PARAMETER Serial
    Probe serial number, for when more than one XDS110 is attached. With
    several present and no serial given this script refuses rather than
    guessing, because writing firmware to the wrong board is not undone by
    reading the message afterwards.

.PARAMETER ListOnly
    Enumerate and report versions. Writes nothing. Run this first.

.EXAMPLE
    .\scripts\update_xds110_firmware.ps1 -ListOnly
    .\scripts\update_xds110_firmware.ps1

.NOTES
    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM, so an
    em-dash in this file is a parse error rather than a typo.
#>
[CmdletBinding()]
param(
    [string]$Uscif,
    [string]$Serial,
    [switch]$ListOnly
)

$ErrorActionPreference = 'Stop'

if (-not $Uscif) {
    $Uscif = Get-ChildItem 'C:\ti' -Filter 'uniflash*' -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'deskdb\content\TICloudAgent\win\ccs_base\common\uscif\xds110' } |
        Where-Object { Test-Path (Join-Path $_ 'xdsdfu.exe') } |
        Select-Object -First 1
}

if (-not $Uscif -or -not (Test-Path (Join-Path $Uscif 'xdsdfu.exe'))) {
    throw @"
No xdsdfu.exe found. Pass -Uscif <directory containing xdsdfu.exe and
firmware_*.bin>. On a UniFlash install that is

  C:\ti\uniflash_<ver>\deskdb\content\TICloudAgent\win\ccs_base\common\uscif\xds110
"@
}

$xdsdfu = Join-Path $Uscif 'xdsdfu.exe'

# Newest firmware in the SAME directory as that xdsdfu. Pairing them is the
# point: an emupack's 3.0.0.13 image written by UniFlash's newer xdsdfu, or the
# reverse, is a combination nobody at TI tested.
$fw = Get-ChildItem $Uscif -Filter 'firmware_*.bin' -File |
    Sort-Object { [version]($_.BaseName -replace '^firmware_', '') } -Descending |
    Select-Object -First 1

if (-not $fw) { throw "No firmware_*.bin next to $xdsdfu." }
$fwVersion = $fw.BaseName -replace '^firmware_', ''

Write-Host "xdsdfu:   $xdsdfu"
Write-Host "firmware: $($fw.Name)  (version $fwVersion)"
Write-Host ''

Write-Host '--- probes present ---'
& $xdsdfu -e
if ($LASTEXITCODE -ne 0) { throw "xdsdfu -e failed ($LASTEXITCODE)." }

if ($ListOnly) {
    Write-Host ''
    Write-Host 'Nothing written (-ListOnly).'
    return
}

Write-Host ''
Write-Host "Writing firmware $fwVersion. Do not unplug the board."
Write-Host ''

# Two steps, and they are two because the device re-enumerates between them:
# -m puts the runtime image into DFU mode (PID 0xbef3 -> 0xbef4), and only then
# does the device accept a download. -r resets back into the new runtime image
# so the probe is usable again without a replug.
$modeArgs = @('-m')
if ($Serial) { $modeArgs = @('-s', $Serial) + $modeArgs }
& $xdsdfu @modeArgs
if ($LASTEXITCODE -ne 0) { throw "xdsdfu -m failed ($LASTEXITCODE): could not enter DFU mode." }

Start-Sleep -Seconds 2

& $xdsdfu -f $fw.FullName -r
if ($LASTEXITCODE -ne 0) {
    throw @"
xdsdfu download failed ($LASTEXITCODE).

The probe is most likely still in DFU mode and is NOT bricked - the boot
loader is a separate image this never touches. Confirm with

  & '$xdsdfu' -e

and expect PID 0xbef4 and Mode: DFU. Re-run this script to write again.
"@
}

Start-Sleep -Seconds 3

Write-Host ''
Write-Host '--- after ---'
& $xdsdfu -e
