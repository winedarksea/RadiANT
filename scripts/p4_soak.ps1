# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Phase D of the P4 plan: the overnight, unattended grant-abort soak.

.DESCRIPTION
    ONE INVARIANT, RUN LONG ENOUGH TO BREAK IT:

        exactly one radiant_nrf_gate_on_grant_end() per granted timeslot.

    Everything this script does exists to make that invariant scoreable by a
    reader who was asleep while it ran. The scoring is not "did it crash" - a
    soak that only reports that is what phase A5 was written to prevent.

    WHY THE FAILURE IS SILENT, AND WHY THAT DICTATES THE SCORING. A skipped
    on_grant_end() means radio_endpoints_attach(false) never runs, so the
    802.15.4 driver's WRITE-ONCE SUBSCRIBE_RXEN is never restored. One miss and
    that receiver never ramps again for the rest of the boot. There is no fault,
    no reset and no log line - the board carries on looking healthy. On the 1 Hz
    gate line the signature is `granted` still climbing while the delivered
    count sits frozen, which is why this script scores the TREND of two counters
    against each other and not just their final values.

    HOW IT DIFFERS FROM p4_arm.ps1, and why it is a separate script:

    1. The console is appended to a FILE as it arrives, not accumulated in a
       StringBuilder and returned at the end. An eight-hour capture is tens of
       megabytes and a job that dies at hour seven with everything still in
       memory has produced nothing at all. p4_arm.ps1's in-memory capture is
       correct for a 240 s arm and wrong for this.
    2. The ANT channel is opened ONCE and held for the whole soak. Reopening it
       periodically would re-acquire, and re-acquisition is exactly the event
       that hides a wedged receiver - the thing being tested for.
    3. It scores against the whole log rather than the last line, so a
       counter that went bad at hour two and recovered is still caught.

    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.

.EXAMPLE
    .\scripts\p4_soak.ps1 -Hours 8
    .\scripts\p4_soak.ps1 -Hours 0.25 -Label smoke     # prove the harness first
#>
[CmdletBinding()]
param(
    [double]$Hours      = 8,
    [string]$Arm        = 'p4med',
    [string]$Label      = 'soak',
    [string]$AntPort    = 'COM8',
    [string]$LogPort    = 'COM7',
    [string]$PeerPort   = 'COM9',
    [int]$DeviceNumber  = 777,
    [string]$Profile    = 'power',
    [switch]$SkipFlash
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$py   = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
$secs = [int]($Hours * 3600)

$outDir = Join-Path $repo 'build\p4logs'
New-Item -ItemType Directory -Force $outDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$base  = Join-Path $outDir "$stamp-$Arm-$Label"
$console = "$base-console.log"

Write-Host "=== P4 soak: $Arm / $Label, $Hours h ($secs s) ===" -ForegroundColor Cyan
Write-Host "console -> $console"

# The peer has to be up before we start, and it is worth one line of proof:
# an eight-hour contended soak against a peer that was never there is eight
# hours of the UNCONTENDED case, scored as though it were the contended one.
function Get-PeerState {
    try {
        $c = New-Object System.IO.Ports.SerialPort $PeerPort, 115200, 'None', 8, 'One'
        $c.DtrEnable = $true   # without it this port returns nothing at all
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
    } catch { return "<error: $($_.Exception.Message)>" }
}

$peerBefore = Get-PeerState
Write-Host "peer before: $peerBefore" -ForegroundColor `
    $(if ($peerBefore -in @('leader','router')) { 'Green' } else { 'Red' })
if ($peerBefore -notin @('leader','router')) {
    throw "peer is '$peerBefore' - reflash it with scripts\flash_p4_peer.ps1 before soaking"
}

if (-not $SkipFlash) {
    & (Join-Path $PSScriptRoot 'p4_flash_only.ps1') -Dir $Arm
}

Get-Process python -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

# Capture straight to disk. Appending per read keeps memory flat and means a
# crash at any point still leaves everything up to that point on disk.
$capJob = Start-Job -Name soakcap -ArgumentList $LogPort, ($secs + 120), $console -ScriptBlock {
    param($p, $n, $path)
    $c = New-Object System.IO.Ports.SerialPort $p, 115200, 'None', 8, 'One'
    $c.DtrEnable = $true
    $c.RtsEnable = $true
    $c.ReadTimeout = 500
    $c.Open()
    $sw = New-Object System.IO.StreamWriter($path, $true)
    $sw.AutoFlush = $true
    $deadline = (Get-Date).AddSeconds($n)
    try {
        while ((Get-Date) -lt $deadline) {
            $t = $c.ReadExisting()
            if ($t.Length -gt 0) { $sw.Write($t) }
            Start-Sleep -Milliseconds 250
        }
        $sw.Write($c.ReadExisting())
    } finally { $sw.Close(); $c.Close() }
}

# The master, and then the channel - held open for the whole soak, once.
$simJob = Start-Job -Name soaksim -ArgumentList $py, $repo, $Profile, $DeviceNumber, ($secs + 120) -ScriptBlock {
    param($py, $repo, $prof, $dev, $n)
    Set-Location $repo
    & $py -u tools\ant_sim.py --profile $prof --device-number $dev --seconds $n 2>&1
}
Start-Sleep -Seconds 10

$verJob = Start-Job -Name soakver -ArgumentList $py, $repo, $AntPort, $Profile, $DeviceNumber, $secs -ScriptBlock {
    param($py, $repo, $port, $prof, $dev, $n)
    Set-Location $repo
    & $py -u tools\ant_verify.py --port $port --profile $prof --device-number $dev --seconds $n 2>&1
}

Write-Host "soaking... (progress every 10 min)" -ForegroundColor Yellow
$end = (Get-Date).AddSeconds($secs)
while ((Get-Date) -lt $end) {
    Start-Sleep -Seconds 600
    if (Test-Path $console) {
        $last = Get-Content $console -Tail 400 -ErrorAction SilentlyContinue |
                Where-Object { $_ -match 'gate: acq=' } | Select-Object -Last 1
        $left = [int]((New-TimeSpan -End $end).TotalMinutes)
        if ($last -and $last -match 'granted=(\d+).*in_grant=(\d+)/') {
            Write-Host ("  {0,4} min left | granted={1} in_grant={2}" -f $left, $matches[1], $matches[2])
        } else {
            Write-Host ("  {0,4} min left | no gate line yet" -f $left) -ForegroundColor DarkYellow
        }
    }
}

Wait-Job -Job $capJob -Timeout 180 | Out-Null
$verText = (Receive-Job -Job $verJob -ErrorAction SilentlyContinue | Out-String)
[IO.File]::WriteAllText("$base-verify.txt", $verText)
Get-Job | Stop-Job -ErrorAction SilentlyContinue
Get-Job | Remove-Job -Force -ErrorAction SilentlyContinue
Get-Process python -ErrorAction SilentlyContinue | Stop-Process -Force

$peerAfter = Get-PeerState

Write-Host ""
Write-Host "=== scoring ===" -ForegroundColor Cyan
& $py (Join-Path $PSScriptRoot 'p4_soak_score.py') $console
$rc = $LASTEXITCODE

Write-Host ""
Write-Host "peer before/after: $peerBefore / $peerAfter"
($verText -split "`r?`n") | Where-Object { $_ -match 'loss|packets|^PASS|^FAILED' } |
    ForEach-Object { Write-Host "  $($_.Trim())" }
Write-Host "logs: $base-*" -ForegroundColor Green

exit $rc
