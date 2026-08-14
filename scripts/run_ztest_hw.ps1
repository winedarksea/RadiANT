# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Build, flash and run the radiant ztest suite on a development kit.

.DESCRIPTION
    native_sim does not build on Windows - no host C compiler, no QEMU - so for
    a long time the only place any C assertion in this project executed was CI
    on Linux, and a broken suite cost a push and a round trip to discover.

    It does not have to. The suites in radiant/tests touch no hardware at
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

.PARAMETER App
    Which ztest application to build, relative to the repository root. There are
    three, and each is a separate application because its prj.conf cannot be
    reconciled with the others':

      radiant\tests       the suites over the module's lower half, driven
                               against tests\fake_radio.c.
      radiant\tests\api   CONFIG_RADIANT_ANTR_API=y, which cannot be
                               flipped in the first one - both it and
                               test_event.c/test_channel.c define
                               radiant_event_crit_enter(),
                               radiant_event_crit_exit(),
                               radiant_event_wakeup() and
                               radiant_channel_event_out(), so the two would
                               collide at link, and radiant_api.c's versions
                               would change what those suites assert.
      radiant\tests\gate  the MPSL timeslot gate, compiled against fakes
                               for MPSL, MPSL_TIMER0 and the five
                               radiant_nrf_gate_* callbacks. Separate because it
                               SHADOWS <mpsl_timeslot.h>, <mpsl_hwres.h> and
                               <hal/nrf_timer.h> for its own app target, which
                               is not a thing to do to the other two.

    BuildDir follows App unless it is given explicitly, so all three can be run
    back to back without any of them rebuilding the others.

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
    . .\scripts\env.ps1 -NcsVersion v3.4.0
    .\scripts\run_ztest_hw.ps1                                  -NcsVersion v3.4.0
    .\scripts\run_ztest_hw.ps1 -App radiant\tests\api  -NcsVersion v3.4.0
    .\scripts\run_ztest_hw.ps1 -App radiant\tests\gate -NcsVersion v3.4.0

    -NcsVersion is not optional on anything built against v3.4.0: the default
    below is v3.2.4, and the mismatch surfaces as a Zephyr-SDK error that reads
    exactly like a broken toolchain.

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
    [string]$App        = 'radiant\tests',
    [string]$Board      = 'nrf5340dk/nrf5340/cpuapp',
    [string]$Port       = 'COM9',
    [string]$Serial     = '1050006310',
    [string]$Device     = 'nRF5340_xxAA_APP',
    [string]$NcsVersion = 'v3.2.4',
    [string]$BuildDir   = '',
    # How long to listen for the run to finish.
    #
    # This was 60 s while every suite in the application was effectively
    # instant. It is not any more: docs/radiant-security.md section 11.3
    # publishes an epoch-recovery cost table - 50, 1 000 and 65 536 CMAC
    # operations for a receiver that many boots out of date - and
    # radiant/tests/src/test_sec_compat_attest.c asserts it by performing
    # those operations. With the software AES backend that is about 45 s on this
    # board, so the whole application now takes a little under a minute of
    # execution on top of the flash.
    #
    # The loop below stops on ztest's own completion line, so this is a ceiling
    # on a hang rather than a delay anybody waits out.
    [int]$Seconds       = 240,
    [switch]$SkipBuild,
    # Assert DTR and RTS before releasing the core.
    #
    # Not the default, because it is not free: some USB-CDC consoles treat the
    # DTR edge as a reset request, and a reset here would land in the middle of
    # the run this script is trying to capture. The J-Link OB VCOM on both DKs
    # does not, and a CDC stack that gates its TX on DTR produces exactly the
    # symptom this script's help attributes to the wrong VCOM - zero bytes on a
    # port that is in fact the right one. So: try the other VCOM first, and try
    # this second, before suspecting the firmware.
    [switch]$Dtr
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent

$appPath = Join-Path $repo $App
if (-not (Test-Path (Join-Path $appPath 'CMakeLists.txt'))) {
    throw "no ztest application at $appPath (App is relative to the repo root)"
}

# One build directory per application. Sharing one would make every alternation
# between the two suites a full -p always rebuild of the other, which is exactly
# the round trip this script exists to remove.
if (-not $BuildDir) {
    $leaf = Split-Path $App -Leaf
    $BuildDir = if ($leaf -eq 'tests') { 'build\ztest_hw' }
                else { "build\ztest_hw_$leaf" }
}

$out  = Join-Path $repo $BuildDir
$hex  = Join-Path $out 'zephyr\zephyr.hex'

if (-not $SkipBuild) {
    if (-not (Get-Command west -ErrorAction SilentlyContinue)) {
        throw "west is not on PATH. Dot-source the environment first:`n" +
              "  . .\scripts\env.ps1 -NcsVersion $NcsVersion"
    }

    # `west build` is an extension command discovered through the workspace
    # manifest, so west must run with its cwd inside the workspace.
    #
    # Its output goes to a file rather than to the console, and that is not
    # tidiness: west writes progress to stderr, PowerShell wraps native stderr
    # in ErrorRecords, and with $ErrorActionPreference = 'Stop' that terminates
    # this script the moment anyone pipes its output anywhere. The build log is
    # more useful next to the image than on the terminal anyway.
    #
    # REDIRECTING STDOUT IS NOT ENOUGH ON ITS OWN, and that is the part this
    # comment got wrong for as long as nobody piped the script. `> $buildLog`
    # sends stdout to the file and leaves stderr going to the host, where the
    # FIRST ordinary progress line - "Loading Zephyr default modules" - becomes
    # a NativeCommandError and kills the run under 'Stop'. It reads exactly like
    # a broken toolchain, which is the same misdiagnosis -NcsVersion already
    # causes here, so it is worth being explicit: stderr is folded into the log
    # as well, and $LASTEXITCODE is the only verdict trusted.
    $buildLog = Join-Path $repo "$BuildDir-build.log"
    New-Item -ItemType Directory -Force (Split-Path $buildLog) | Out-Null
    Push-Location "C:\ncs\$NcsVersion"
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        west -z "C:\ncs\$NcsVersion\zephyr" build `
            -s $appPath -b $Board -d $out -p always --no-sysbuild `
            > $buildLog 2>&1
        if ($LASTEXITCODE -ne 0) { throw "build failed; log at $buildLog" }
    } finally {
        $ErrorActionPreference = $prevEap
        Pop-Location
    }
    Write-Host "built (log: $buildLog)"
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
if ($Dtr) {
    $serialPort.DtrEnable = $true
    $serialPort.RtsEnable = $true
}
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
