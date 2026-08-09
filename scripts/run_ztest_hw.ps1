# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Build, flash and run the radiant_core ztest suite on a development kit.

.DESCRIPTION
    native_sim does not build on Windows - no host C compiler, no QEMU - so for
    a long time the only place any C assertion in this project executed was CI
    on Linux, and a broken suite cost a push and a round trip to discover.

    It does not have to. The suites in radiant_core/tests touch no hardware at
    all: they drive the module against tests/fake_radio.c, a mock HAL with a
    virtual clock. So the board is not the thing under test, it is merely a C
    runtime with a UART - and an attached DK is one that is available now.
    ztest's output over VCOM is the same output twister parses.

    This does not replace the CI job and is not meant to. CI runs on native_sim
    at 32 bits, which is the width that catches the struct-packing assumptions a
    64-bit host would hide; this runs on the real target width instead. Two
    different checks, and this one is the fast one.

    ASCII only, deliberately: Windows PowerShell 5.1 reads .ps1 files as ANSI
    unless they carry a BOM, so non-ASCII characters here become parse errors.

.PARAMETER Board
    Zephyr board target. Defaults to the nRF5340 DK's application core.

.PARAMETER Port
    VCOM carrying the console. On the nRF5340 DK this is the SECOND enumerated
    port, not the first - see the note below.

.PARAMETER Serial
    J-Link probe serial number. With two probes attached every non-interactive
    JLink command fails with "Cannot connect to the probe/programmer" unless one
    is named, because it cannot choose and cannot ask.

.PARAMETER Device
    J-Link device name for the target core.

.PARAMETER NcsVersion
    NCS version whose zephyr tree builds the suite.

.PARAMETER SkipBuild
    Flash and run whatever is already in the build directory.

.EXAMPLE
    . .\scripts\env.ps1 -NcsVersion v3.2.4
    .\scripts\run_ztest_hw.ps1

.NOTES
    Finding the port and the probe serial:

        Get-PnpDevice -Class Ports -Status OK | Select-Object FriendlyName, InstanceId

    The InstanceId carries the probe's USB PID - 1061 is the nRF5340 DK, 1069
    the nRF54L15 DK - and the composite device's InstanceId carries its serial.
    The console is on the port whose interface is MI_02, which enumerates as the
    higher-numbered COM port on some machines and the lower on others. If a run
    captures zero bytes, try the other one before suspecting the firmware.
#>
[CmdletBinding()]
param(
    [string]$Board      = 'nrf5340dk/nrf5340/cpuapp',
    [string]$Port       = 'COM9',
    [string]$Serial     = '1050006310',
    [string]$Device     = 'nRF5340_xxAA_APP',
    [string]$NcsVersion = 'v3.2.4',
    [string]$BuildDir   = 'build\ztest_hw',
    [int]$Seconds       = 60,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $repo $BuildDir
$hex  = Join-Path $out 'zephyr\zephyr.hex'

if (-not $SkipBuild) {
    if (-not (Get-Command west -ErrorAction SilentlyContinue)) {
        throw "west is not on PATH. Dot-source the environment first:`n" +
              "  . .\scripts\env.ps1 -NcsVersion $NcsVersion"
    }

    # `west build` is an extension command discovered through the workspace
    # manifest, so west must run with its cwd inside the workspace.
    Push-Location "C:\ncs\$NcsVersion"
    try {
        west -z "C:\ncs\$NcsVersion\zephyr" build `
            -s (Join-Path $repo 'radiant_core\tests') -b $Board -d $out -p always --no-sysbuild
        if ($LASTEXITCODE -ne 0) { throw "build failed" }
    } finally {
        Pop-Location
    }
}

if (-not (Test-Path $hex)) { throw "no image at $hex" }

$jlink = 'C:\Program Files\SEGGER\JLink_V966\JLink.exe'
if (-not (Test-Path $jlink)) { throw "no JLink.exe at $jlink" }

$tmp = Join-Path ([IO.Path]::GetTempPath()) 'radiant_ztest'
New-Item -ItemType Directory -Force $tmp | Out-Null

# DisableAutoUpdateFW first, always. This JLink carries newer OB firmware than
# these probes do, its upgrade attempt fails at "Emulator did not re-enumerate"
# every single time, and it leaves the probe in bootloader until somebody
# physically replugs it. Losing that first line is how a probe gets bricked.
function New-JLinkScript([string]$path, [string]$tail) {
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

$flashScript = Join-Path $tmp 'flash.jlink'
$goScript    = Join-Path $tmp 'go.jlink'
New-JLinkScript $flashScript "loadfile $hex"
New-JLinkScript $goScript    "g"

Write-Host "flashing $hex -> $Board ($Serial)" -ForegroundColor Cyan
& $jlink -NoGui 1 -SelectEmuBySN $Serial -CommanderScript $flashScript > (Join-Path $tmp 'flash.log')
if ($LASTEXITCODE -ne 0) { throw "JLink flash failed; see $tmp\flash.log" }

# Open the console BEFORE releasing the core. ztest starts running microseconds
# after reset and the whole run takes under a second, so a listener that opens
# afterwards captures nothing and looks exactly like a hung board.
$serialPort = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$serialPort.Open()
try {
    & $jlink -NoGui 1 -SelectEmuBySN $Serial -CommanderScript $goScript > (Join-Path $tmp 'go.log')

    $sb = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        [void]$sb.Append($serialPort.ReadExisting())
        if ($sb.ToString() -match 'PROJECT EXECUTION (SUCCESSFUL|FAILED)') { break }
        Start-Sleep -Milliseconds 200
    }
    $text = $sb.ToString()
} finally {
    $serialPort.Close()
}

$log = Join-Path $out 'ztest-console.txt'
[IO.File]::WriteAllText($log, $text)

if ($text.Length -eq 0) {
    throw "nothing arrived on $Port. The console is on the OTHER VCOM of the " +
          "pair more often than it is a firmware fault - see the notes in this " +
          "script's help."
}

$text -split "`r?`n" | Where-Object { $_ -match 'SUITE (PASS|FAIL)' } |
    ForEach-Object { Write-Host $_.Trim() }

# Every failure with the three lines before it, which is where ztest puts the
# assertion text and the message. Without this the summary says which test
# failed and nothing about why.
$lines = $text -split "`r?`n"
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s*FAIL - test_') {
        Write-Host ''
        $lines[[Math]::Max(0, $i - 4)..$i] |
            Where-Object { $_ -match '\S' } |
            ForEach-Object { Write-Host ("  " + $_.Trim()) -ForegroundColor Red }
    }
}

Write-Host "`nfull console log: $log"

if ($text -match 'PROJECT EXECUTION SUCCESSFUL') {
    Write-Host "PROJECT EXECUTION SUCCESSFUL" -ForegroundColor Green
    exit 0
}
throw "ztest run failed (or did not finish within $Seconds s)"
