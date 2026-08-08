<#
.SYNOPSIS
    Flash the ANT+ sensor simulator onto an nRF54L15 DK through J-Link.

.DESCRIPTION
    The nRF54L15 DK has neither a UF2 bootloader nor a working mass-storage
    drag-and-drop path: copying a hex to the JLINK volume is rejected with
    "The currently active SWD interface does not support MSD drag and drop".
    That is harmless, but it is not a route to a flashed board. J-Link Commander
    is.

    The first line of the script it generates is the whole trick.
    `exec DisableAutoUpdateFW` stops J-Link from trying to upgrade the onboard
    debugger's firmware. V9.66 carries newer OB firmware than these probes ship
    with and attempts the upgrade on every single connect; the upgrade fails at
    "Communication timeout. Emulator did not re-enumerate" every time and leaves
    the probe stuck in its bootloader (VID_1366&PID_0101) until it is physically
    replugged. Without that line this script does not flash a board, it breaks
    one.

    nRF54L15_M33 is the application core. nRF54L15_RV32 is the flexible
    peripheral processor, and pointing this at it silently programs the wrong
    target.

    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM, so an
    em-dash in this file is a parse error rather than a typo.

.PARAMETER HexPath
    Image to flash. Defaults to the newest merged.hex under build\sim*.

.PARAMETER JLinkExe
    Path to JLink.exe.

.PARAMETER Device
    J-Link device name. The application core unless you know otherwise.

.PARAMETER Speed
    SWD clock in kHz.

.EXAMPLE
    .\scripts\flash_sim_jlink.ps1
    .\scripts\flash_sim_jlink.ps1 -HexPath build\sim_l15\merged.hex

.NOTES
    Afterwards the simulator logs on the DK's first VCOM (COM7 here) and
    transmits on the ANT+ network. Point tools\ant_verify.py at the dongle
    under test, not at this board - a board cannot hear itself.
#>
[CmdletBinding()]
param(
    [string]$HexPath,
    [string]$JLinkExe = 'C:\Program Files\SEGGER\JLink_V966\JLink.exe',
    [string]$Device = 'nRF54L15_M33',
    [int]$Speed = 4000
)

$ErrorActionPreference = 'Stop'

if (-not $HexPath) {
    # sysbuild writes merged.hex at the top of the build directory and
    # zephyr.hex one level further in. merged.hex is the one to program: it
    # carries every image sysbuild produced, not just the application.
    $buildRoot = Join-Path $PSScriptRoot '..\build'
    $candidate = Get-ChildItem -Path $buildRoot -Filter merged.hex -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.DirectoryName -match 'sim' } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $candidate) {
        throw @"
No merged.hex found under $buildRoot for a sim build. Build first:

  . .\scripts\env.ps1 -NcsVersion v3.2.4
  Push-Location C:\ncs\v3.2.4
  west -z C:\ncs\v3.2.4\zephyr build -s <repo>\sim -d <repo>\build\sim_l15 ``
       -b nrf54l15dk/nrf54l15/cpuapp --sysbuild -p always
  Pop-Location
"@
    }

    $HexPath = $candidate.FullName
    Write-Host "Using newest image: $HexPath"
}

if (-not (Test-Path $HexPath)) {
    throw "No image at $HexPath. Build first (see README.md)."
}
$HexPath = (Resolve-Path $HexPath).Path

if (-not (Test-Path $JLinkExe)) {
    throw @"
No JLink.exe at $JLinkExe.
Install SEGGER J-Link, or pass -JLinkExe <path>.
"@
}

$script = Join-Path ([System.IO.Path]::GetTempPath()) 'ant_sim_flash.jlink'

# exec DisableAutoUpdateFW must be first: see the note above. The rest is
# reset, halt, program, reset, run, quit.
$lines = @(
    'exec DisableAutoUpdateFW',
    'si SWD',
    "speed $Speed",
    "device $Device",
    'connect',
    'r',
    'h',
    "loadfile $HexPath",
    'r',
    'g',
    'q'
)

Set-Content -Path $script -Value $lines -Encoding ascii

$sizeKb = [math]::Round((Get-Item $HexPath).Length / 1KB, 1)
Write-Host "Flashing $HexPath ($sizeKb KB) to $Device at $Speed kHz"

& $JLinkExe -NoGui 1 -CommanderScript $script
$code = $LASTEXITCODE

Remove-Item $script -ErrorAction SilentlyContinue

if ($code -ne 0) {
    throw @"
J-Link exited with $code.

If it reported a firmware update, the probe is now in its bootloader
(VID_1366&PID_0101, status Error) and has to be physically unplugged and
replugged. That should not happen - the script disables the auto-update - but
it is what the symptom looks like.
"@
}

Write-Host ''
Write-Host 'Flashed. The simulator starts transmitting immediately.'
Write-Host '  logs:   COM7 at 115200 (the DK VCOM)'
Write-Host '  verify: python tools\ant_verify.py --expect-watts 100 --expect-rpm 80 --seconds 60'
Write-Host '          run that against the *dongle*, on a second board.'
