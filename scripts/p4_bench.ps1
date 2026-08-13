# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Flash one P4 arm to the nRF54L15 DK and capture its log console.

.DESCRIPTION
    The six runs of the P4 sitting differ only in which image is on the board and
    what the host is doing to it, so the flash-and-capture half is factored out
    here rather than retyped six times. Retyping it is how a sitting ends up with
    one arm that was reset and one that was not.

    THE TWO VCOMs ARE NOT INTERCHANGEABLE AND BOTH ARE USED AT ONCE. The nRF54L15
    DK presents a pair: COM8 carries the ANT serial protocol that
    tools/ant_verify.py speaks, and COM7 carries the Zephyr log - which is where
    the 1 Hz gate dump and the thread-load report come out. This script captures
    COM7 ONLY, deliberately, so that ant_verify.py can hold COM8 for the same
    run. A capture that took COM8 would lock out the measurement it exists to
    annotate.

    DTR IS ASSERTED, ALWAYS. Both DK VCOMs return nothing at all without it and
    the board then looks hung. That has cost this project a session more than
    once, so it is not optional here and there is no switch to turn it off.

    THE BOARD IS RESET BEFORE EVERY RUN, by the J-Link script's own `r`. The
    counters this sitting is read from are cumulative since boot, so an arm taken
    on a board that has been up since the previous arm is not a reading.

    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.

.PARAMETER Arm
    p4ctrl, p4med or p4sed - the build directory under build\.

.PARAMETER Seconds
    How long to capture. Match it to the ant_verify.py run length plus the
    thread-load start delay, or the arm ends before the load does.

.PARAMETER Label
    Free text folded into the output filename, e.g. "ant-idle" or "ant-loaded".

.PARAMETER NoFlash
    Reset and capture without reprogramming. For the second arm on an image that
    is already on the board - which is the ANT-idle/ANT-loaded pair.

.EXAMPLE
    .\scripts\p4_bench.ps1 -Arm p4ctrl -Seconds 260 -Label control
    .\scripts\p4_bench.ps1 -Arm p4med -Seconds 300 -Label ant-idle -NoFlash
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('p4ctrl', 'p4med', 'p4sed')]
    [string]$Arm,

    [int]$Seconds = 260,
    [string]$Label = 'run',
    [switch]$NoFlash,

    # The nRF54L15 DK. -SelectEmuBySN is mandatory with two probes attached and
    # a bare CommanderScript has bricked a probe on this bench before.
    [string]$Serial = '001057737173',
    [string]$Device = 'nRF54L15_M33',
    [string]$LogPort = 'COM7'
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$hex  = Join-Path $repo "build\$Arm\merged.hex"
if (-not (Test-Path $hex)) {
    $hex = Join-Path $repo "build\$Arm\ant_dongle\zephyr\zephyr.hex"
}
if (-not (Test-Path $hex)) { throw "no image for $Arm - run scripts\build_p4.ps1" }

# The backend read-back again, at the point of use. build_p4.ps1 checks it at
# build time; this checks the thing actually about to be flashed, because the
# silent fallback to the null backend is indistinguishable from every failure
# this sitting is chasing and one check at the wrong moment is no check at all.
$cfg = Join-Path $repo "build\$Arm\ant_dongle\zephyr\.config"
if (-not (Select-String -Path $cfg -Pattern '^CONFIG_RADIANT_CORE_BACKEND_NRF_GATE_MPSL=y' -Quiet)) {
    throw "$Arm was not built with the MPSL gate. Do not take a reading from it."
}

$jlink = 'C:\Program Files\SEGGER\JLink_V966\JLink.exe'
if (-not (Test-Path $jlink)) { throw "no JLink.exe at $jlink" }

$tmp = Join-Path ([IO.Path]::GetTempPath()) 'radiant_p4'
New-Item -ItemType Directory -Force $tmp | Out-Null

function New-JLinkScript([string]$path, [string]$tail) {
    # DisableAutoUpdateFW first, ALWAYS. This JLink carries newer OB firmware
    # than these probes do, its upgrade attempt fails at "Emulator did not
    # re-enumerate" every time, and it leaves the probe in bootloader until
    # somebody physically replugs it. Losing that first line is how a probe gets
    # bricked, and this bench has done it once already.
    @"
exec DisableAutoUpdateFW
si SWD
speed 4000
device $Device
connect
r
h
$tail
q
"@ | Out-File -FilePath $path -Encoding ascii
}

$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    if (-not $NoFlash) {
        $flashScript = Join-Path $tmp 'flash.jlink'
        New-JLinkScript $flashScript "loadfile $hex"
        Write-Host "flashing $hex -> $Serial" -ForegroundColor Cyan
        & $jlink -NoGui 1 -SelectEmuBySN $Serial -CommanderScript $flashScript > (Join-Path $tmp 'flash.log')
        if ($LASTEXITCODE -ne 0) { throw "JLink flash failed; see $tmp\flash.log" }
    }

    # Open the console BEFORE releasing the core, for the same reason
    # run_ztest_hw.ps1 does: the first seconds carry the gate's session_open rc
    # and the backend banner, and a listener that opens afterwards misses
    # exactly the lines that say whether this image has a radio.
    $port = New-Object System.IO.Ports.SerialPort $LogPort, 115200, 'None', 8, 'One'
    $port.DtrEnable = $true
    $port.RtsEnable = $true
    $port.Open()

    try {
        $goScript = Join-Path $tmp 'go.jlink'
        New-JLinkScript $goScript 'g'
        & $jlink -NoGui 1 -SelectEmuBySN $Serial -CommanderScript $goScript > (Join-Path $tmp 'go.log')
        if ($LASTEXITCODE -ne 0) { throw "JLink go failed; see $tmp\go.log" }

        Write-Host "capturing $LogPort for $Seconds s (DTR asserted)" -ForegroundColor Cyan
        $sb = New-Object System.Text.StringBuilder
        $deadline = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $deadline) {
            [void]$sb.Append($port.ReadExisting())
            Start-Sleep -Milliseconds 250
        }
        [void]$sb.Append($port.ReadExisting())
        $text = $sb.ToString()
    } finally {
        $port.Close()
    }
} finally {
    $ErrorActionPreference = $prev
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outDir = Join-Path $repo 'build\p4logs'
New-Item -ItemType Directory -Force $outDir | Out-Null
$out = Join-Path $outDir "$stamp-$Arm-$Label.log"
[IO.File]::WriteAllText($out, $text)

if ($text.Length -eq 0) {
    throw "nothing arrived on $LogPort. DTR is already asserted here, so the " +
          "next suspect is the OTHER VCOM of the pair, not a hung board."
}

Write-Host ""
Write-Host "captured $($text.Length) bytes -> $out" -ForegroundColor Green

# The lines that decide whether the run is usable at all, surfaced now rather
# than left in the file. A gate that never opened a session and a thread load
# that never attached both produce a full-looking log.
foreach ($pat in @('session_open', 'backend', 'thread coexistence load',
                   'thread role ->', 'MPSL', 'ASSERT', 'FATAL')) {
    $hits = $text -split "`r?`n" | Where-Object { $_ -match $pat } | Select-Object -First 2
    foreach ($h in $hits) { Write-Host "  | $($h.Trim())" }
}

Write-Host ""
Write-Host "last gate line:" -ForegroundColor Cyan
$gate = $text -split "`r?`n" | Where-Object { $_ -match 'gate: acq=' } | Select-Object -Last 1
if ($gate) { Write-Host "  $($gate.Trim())" } else { Write-Host "  (none - is CONFIG_RADIANT_CORE_SWEEP_DEBUG on?)" }

Write-Host "last thread lines:" -ForegroundColor Cyan
foreach ($p in @('thread load:', 'thread lat us:', 'thread mac d:')) {
    $l = $text -split "`r?`n" | Where-Object { $_ -match $p } | Select-Object -Last 1
    if ($l) { Write-Host "  $($l.Trim())" }
}
