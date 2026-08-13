# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Run one arm of the P4 coexistence sitting, end to end, with the bench
    discipline built in rather than remembered.

.DESCRIPTION
    Six runs make up the sitting and the difference between them is two switches:
    which image is on the DUT, and whether the host is holding an ANT channel.
    Everything else must be identical across all six or the arms are not
    comparable - which is why it is a script and not a checklist.

    WHAT IT DOES IN ORDER, AND WHY EACH STEP IS NOT OPTIONAL:

    1. `ot state` on the peer BEFORE the run. A dead peer and a broken DUT both
       present as `detached` then silence, and after the fact they are
       indistinguishable. Asked again at the end for the same reason: a peer that
       died halfway invalidates the arm, and only the pair of readings can say
       so.

    2. Flash and RESET the DUT. The gate counters are cumulative since boot, so
       an arm taken on a board that has been up since the previous arm is not a
       reading. This is also the step whose absence produced two "0 packets"
       results earlier in this sitting that looked exactly like a dead radio and
       were nothing of the kind.

    3. Capture the DUT log VCOM (COM7) for the whole arm, while ant_verify.py
       holds the ANT VCOM (COM8). Two ports, two instruments, one run - the gate
       dump and the loss figure have to come from the same 240 seconds or the
       scheduler cost cannot be read against the air cost.

    4. In an ANT-LOADED arm, ant_sim.py drives the master and ant_verify.py
       measures. In an ANT-IDLE arm the host opens NO channel at all - that is
       the whole point of it: it is the reference that says what the second stack
       costs when we are placing no windows. An arm whose other stack is idle is
       not a coexistence arm, and this project has paid for that once already.

    THE PROFILE IS POWER, NOT HEART-RATE, AND THAT IS A CORRECTION.
    `loss (exact)` is the figure this whole gate is read against, and
    ant_verify.py only computes it when the transmitter's event counter never
    once stands still (`std_event_still == 0`) - which proves the counter steps
    per message rather than per pedal stroke. A heart-rate master's counter steps
    per BEAT: at 145 bpm against a 4 Hz channel it advances on about 107 of 180
    packet pairs, so no exact loss is produced at all and the run can only be
    read on the wall clock. Measured on this bench, both ways round. Power's
    page 0x10 counter steps per message and yields `loss (exact) 0.00 %` on the
    same rig - over a SHORT run.

    AND OVER A 240 s RUN IT DOES NOT, WHICH IS A LIMITATION OF THIS SITTING AND
    HAS TO BE STATED RATHER THAN WORKED AROUND.

    `loss (exact)` needs std_event_still == 0: the transmitter's counter must
    never once repeat between two page 0x10 packets. With ant_sim.py driving a
    dongle as the master, it eventually does - an ANT master re-broadcasts its
    current payload until the host loads the next one, and any window in which
    the receiver catches the same payload twice delivers the same event count
    twice. Measured here: 40 s clean, 240 s with 9 repeats in 944 pairs. So the
    exact figure is simply absent on the long runs this gate requires.

    THE ARMS ARE STILL COMPARABLE, and that is what the gate reads. Every arm
    uses the identical transmitter, receiver, reader and rig, so a systematic
    bias in the transmitter cancels in the arm-to-arm delta - which is the
    quantity [gates.coexistence] compares. What is lost is the ability to
    compare this sitting's ABSOLUTE loss against the characterised ~0.4 % floor,
    and the write-up must say so.

    THE PROPER FIX IS A DIFFERENT TRANSMITTER, not a different reading:
    archive/benchmarks/baseline.schema.json already refuses ant_sim_py as a
    baseline transmitter for this reason and wants sim_firmware or a real
    sensor. The Feather that normally carries it would not enumerate after two
    flashes in this session, and the nRF52840 Dongle it was replaced with is the
    only master left on the bench. Re-take these numbers against sim_firmware
    when the Feather is back.

    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.

.EXAMPLE
    .\scripts\p4_arm.ps1 -Arm p4ctrl -Label control
    .\scripts\p4_arm.ps1 -Arm p4med -Label ant-idle -AntIdle
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('p4ctrl', 'p4med', 'p4sed', 'p4direct')]
    [string]$Arm,

    [Parameter(Mandatory = $true)]
    [string]$Label,

    # ANT-idle: the host never opens a channel, so no window is ever placed.
    [switch]$AntIdle,

    [int]$Seconds       = 240,
    [string]$Profile    = 'power',
    [int]$DeviceNumber  = 777,
    [string]$AntPort    = 'COM8',
    [string]$LogPort    = 'COM7',
    [string]$PeerPort   = 'COM9',
    [switch]$SkipPeerCheck
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$py   = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'

$outDir = Join-Path $repo 'build\p4logs'
New-Item -ItemType Directory -Force $outDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$base  = Join-Path $outDir "$stamp-$Arm-$Label"

function Get-PeerState {
    # Short and non-destructive. flash_p4_peer.ps1 does the thorough version;
    # this one only has to answer "is it still there", twice per arm.
    try {
        $c = New-Object System.IO.Ports.SerialPort $PeerPort, 115200, 'None', 8, 'One'
        $c.DtrEnable = $true    # without it this port returns nothing at all
        $c.RtsEnable = $true
        $c.NewLine = "`n"
        $c.ReadTimeout = 500
        $c.Open()
        try {
            [void]$c.ReadExisting()
            $c.WriteLine('ot state')
            Start-Sleep -Milliseconds 900
            $t = $c.ReadExisting()
        } finally { $c.Close() }
        $s = $t -split "`r?`n" |
             Where-Object { $_.Trim() -match '^(leader|router|child|detached|disabled)$' } |
             Select-Object -Last 1
        if ($s) { return $s.Trim() }
        return '<no answer>'
    } catch {
        return "<error: $($_.Exception.Message)>"
    }
}

Write-Host "=== $Arm / $Label ===" -ForegroundColor Cyan

$peerBefore = if ($SkipPeerCheck) { 'n/a' } else { Get-PeerState }
Write-Host "peer before: $peerBefore"

# Step 2: flash and reset. Always, even when the image is already on the board.
& (Join-Path $PSScriptRoot 'p4_flash_only.ps1') -Dir $Arm

Get-Process python -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

# Step 3: the log capture, started before anything else so the boot banner and
# the gate's session_open rc are inside the arm.
$capJob = Start-Job -Name p4cap -ArgumentList $LogPort, ($Seconds + 18) -ScriptBlock {
    param($p, $secs)
    $c = New-Object System.IO.Ports.SerialPort $p, 115200, 'None', 8, 'One'
    $c.DtrEnable = $true
    $c.RtsEnable = $true
    $c.Open()
    $sb = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $deadline) {
        [void]$sb.Append($c.ReadExisting())
        Start-Sleep -Milliseconds 250
    }
    [void]$sb.Append($c.ReadExisting())
    $c.Close()
    $sb.ToString()
}

$verifyText = ''
if ($AntIdle) {
    Write-Host "ANT IDLE - no channel is opened by the host; placing no windows." -ForegroundColor Yellow
    Start-Sleep -Seconds ($Seconds + 10)
} else {
    # The master first, with enough lead that it is already on the air when the
    # receiver starts searching - and long enough to outlast the measurement.
    $simJob = Start-Job -Name p4sim -ArgumentList $py, $repo, $Profile, $DeviceNumber, ($Seconds + 40) -ScriptBlock {
        param($py, $repo, $prof, $dev, $secs)
        Set-Location $repo
        & $py -u tools\ant_sim.py --profile $prof --device-number $dev --seconds $secs 2>&1
    }
    Start-Sleep -Seconds 10

    Write-Host "measuring $Seconds s on $AntPort (profile $Profile, device #$DeviceNumber)"
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $verifyText = & $py tools\ant_verify.py --port $AntPort --profile $Profile `
                          --device-number $DeviceNumber --seconds $Seconds 2>&1 | Out-String
    } finally {
        $ErrorActionPreference = $prev
    }
    [IO.File]::WriteAllText("$base-verify.txt", $verifyText)

    $simText = (Receive-Job -Name p4sim -Keep | Out-String)
    [IO.File]::WriteAllText("$base-sim.txt", $simText)
}

# WAITED FOR, NOT SLEPT AT. The capture job returns its whole transcript as a
# single string from the END of its script block, so Receive-Job on a job that
# is still running hands back nothing at all - and "nothing at all" from the log
# VCOM is the exact signature of the DTR mistake this bench keeps making, so it
# gets misread rather than noticed. The first arm of this sitting reported
# "gate: (none)" for precisely this reason and the gate was working perfectly.
Wait-Job -Job $capJob -Timeout ($Seconds + 60) | Out-Null
$logText = (Receive-Job -Job $capJob | Out-String)
[IO.File]::WriteAllText("$base-console.log", $logText)
Get-Job | Remove-Job -Force -ErrorAction SilentlyContinue
Get-Process python -ErrorAction SilentlyContinue | Stop-Process -Force

$peerAfter = if ($SkipPeerCheck) { 'n/a' } else { Get-PeerState }

# ---------------------------------------------------------------------------
# The summary. Everything below is read off the two captures, so that an arm
# can be judged usable or not before the next one is started rather than after
# the sitting is over.
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "peer before/after: $peerBefore / $peerAfter" -ForegroundColor `
    $(if ($peerBefore -eq $peerAfter -and $peerBefore -in @('leader','router')) { 'Green' } else { 'Red' })

$lines = $logText -split "`r?`n"

Write-Host "gate (last 1 Hz line):" -ForegroundColor Cyan
$g = $lines | Where-Object { $_ -match 'gate: acq=' } | Select-Object -Last 1
if ($g) { Write-Host "  $($g.Trim())" } else { Write-Host "  (none)" -ForegroundColor Red }

Write-Host "thread load:" -ForegroundColor Cyan
foreach ($p in @('thread load:', 'thread lat us:', 'thread mac d:')) {
    $l = $lines | Where-Object { $_ -match $p } | Select-Object -Last 1
    if ($l) { Write-Host "  $($l.Trim())" }
}
$role = $lines | Where-Object { $_ -match 'thread role ->' } | Select-Object -Last 1
if ($role) { Write-Host "  $($role.Trim())" }

foreach ($bad in @('ASSERT', 'FATAL', 'Stack overflow', 'OVERSTAY')) {
    $h = $lines | Where-Object { $_ -match $bad }
    if ($h) { Write-Host "  !! $bad seen $($h.Count) time(s)" -ForegroundColor Red }
}

if (-not $AntIdle) {
    Write-Host "ant_verify:" -ForegroundColor Cyan
    ($verifyText -split "`r?`n") |
        Where-Object { $_ -match 'loss|packets|heard on channel|^PASS|^FAILED' } |
        ForEach-Object { Write-Host "  $($_.Trim())" }
}

Write-Host ""
Write-Host "logs: $base-*" -ForegroundColor Green
