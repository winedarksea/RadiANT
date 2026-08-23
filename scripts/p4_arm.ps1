# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Run one arm of the P4 coexistence sitting, end to end, with the bench
    discipline built in rather than remembered.

.DESCRIPTION
    Six runs make up the sitting and the difference between them is two
    switches: which image is on the DUT, and whether the host is holding an
    ANT channel. Everything else must be identical across all six or the
    arms are not comparable - which is why it is a script and not a
    checklist.

    - `ot state` on the peer before AND after the run: a dead peer and a
      broken DUT both present as `detached` then silence, and only the pair
      of readings can tell them apart.
    - Flash and RESET the DUT every arm, even if the image is already
      loaded: the gate counters are cumulative since boot, so a board left
      running from the previous arm gives a false "0 packets" reading.
    - The DUT log VCOM (COM7) and the ANT VCOM (COM8) are captured for the
      same window on two separate instruments, so the scheduler cost and
      the air cost come from the same run.
    - ANT-loaded arms run ant_sim.py as master and ant_verify.py as
      measurer; ANT-idle arms open no channel at all, as the reference for
      what the second stack costs while placing no windows.

    The profile is power, not heart-rate: `loss (exact)` is only computed
    when the transmitter's event counter steps once per message
    (`std_event_still == 0`). A heart-rate master's counter steps per BEAT -
    at 145 bpm on a 4 Hz channel it advances on only ~107 of 180 packet
    pairs, so no exact figure is produced. Power's page 0x10 counter steps
    per message and gives `loss (exact) 0.00 %` on a short run, but not over
    this gate's 240 s: measured 40 s clean vs. 9 repeats in 944 pairs at
    240 s, because ant_sim.py re-broadcasts its last payload until the host
    loads the next one, and a receiver catching the same payload twice
    double-counts the event.

    The arms are still comparable against each other - same
    transmitter/receiver/rig on every arm, so a systematic bias cancels in
    the arm-to-arm delta - but this sitting's ABSOLUTE loss is not
    comparable to the characterised ~0.4 % floor.
    archive/benchmarks/baseline.schema.json already refuses ant_sim_py as a
    baseline transmitter for this reason; use sim_firmware when available.

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

# WAITED FOR, NOT SLEPT AT: the capture job returns its whole transcript as a
# single string from the END of its script block, so Receive-Job on a job
# still running hands back nothing - which is the same signature as the DTR
# mistake this bench keeps making, so a premature read gets misread as a dead
# gate rather than noticed as a race.
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
