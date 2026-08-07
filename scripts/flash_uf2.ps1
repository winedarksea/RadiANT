<#
.SYNOPSIS
    Copy the built UF2 image onto the Feather's bootloader volume.

.DESCRIPTION
    Replaces the macOS scripts/flash_uf2.sh. `west flash` is not usable here:
    the uf2 runner dies with "ValueError: uf2 doesn't support --dev-id option".

    Double-tap RESET on the Feather first so FTHR840BOOT mounts. The bootloader
    reboots into the new application as soon as the copy completes, which makes
    the volume vanish mid-write - Copy-Item reporting a disconnect at the very
    end is normal and is treated as success.

    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.

.PARAMETER Uf2Path
    UF2 image to flash. Defaults to the newest zephyr.uf2 under build\.

.PARAMETER VolumeLabel
    Bootloader volume label. FTHR840BOOT for the Feather nRF52840 Express.

.PARAMETER TimeoutSeconds
    How long to wait for the volume to appear. 0 means do not wait.

.EXAMPLE
    .\scripts\flash_uf2.ps1
    .\scripts\flash_uf2.ps1 -Uf2Path build\stage2\zephyr\zephyr.uf2 -TimeoutSeconds 30
#>
[CmdletBinding()]
param(
    [string]$Uf2Path,
    [string]$VolumeLabel = 'FTHR840BOOT',
    [int]$TimeoutSeconds = 0
)

$ErrorActionPreference = 'Stop'

if (-not $Uf2Path) {
    # sysbuild nests the image one level deeper (build\<dir>\ant_dongle\zephyr\)
    # than a plain application build does, so search rather than hardcode.
    $buildRoot = Join-Path $PSScriptRoot '..\build'
    $candidate = Get-ChildItem -Path $buildRoot -Filter zephyr.uf2 -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $candidate) {
        throw "No zephyr.uf2 found under $buildRoot. Build first (see README.md)."
    }

    $Uf2Path = $candidate.FullName
    Write-Host "Using newest image: $Uf2Path"
}

if (-not (Test-Path $Uf2Path)) {
    throw "No UF2 image at $Uf2Path. Build first (see README.md)."
}
$Uf2Path = (Resolve-Path $Uf2Path).Path

function Get-BootVolume {
    param([string]$Label)
    Get-Volume -FileSystemLabel $Label -ErrorAction SilentlyContinue |
        Where-Object { $_.DriveLetter }
}

$volume = Get-BootVolume -Label $VolumeLabel
if (-not $volume -and $TimeoutSeconds -gt 0) {
    Write-Host "Waiting up to $TimeoutSeconds s for $VolumeLabel - double-tap RESET now..."
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not $volume -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $volume = Get-BootVolume -Label $VolumeLabel
    }
}

if (-not $volume) {
    throw @"
Bootloader volume '$VolumeLabel' is not mounted.
Double-tap RESET on the Feather until it appears, then re-run.
If it never appears, try a different USB cable - some are charge-only.
"@
}

$target = "$($volume.DriveLetter):\$(Split-Path $Uf2Path -Leaf)"
$sizeKb = [math]::Round((Get-Item $Uf2Path).Length / 1KB, 1)
Write-Host "Copying $Uf2Path ($sizeKb KB) to $target"

try {
    Copy-Item -LiteralPath $Uf2Path -Destination $target -Force
} catch {
    # The bootloader resets the board the moment the last block lands, so the
    # volume can disappear before Copy-Item finishes. Only treat that as an
    # error if the drive is still mounted afterwards.
    Start-Sleep -Milliseconds 1500
    if (Get-BootVolume -Label $VolumeLabel) {
        throw
    }
    Write-Host "Volume ejected during write - that is the bootloader rebooting."
}

Write-Host "Flashed. The board should re-enumerate as VID_0FCF&PID_1008 within a few seconds."
Write-Host "Check with: Get-PnpDevice | Where-Object InstanceId -match 'VID_0FCF'"
