# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Flash the P4 Thread peer (OpenThread CLI + p4_peer.conf) onto the nRF5340 DK
    and confirm from its own console that it has formed a network.

.DESCRIPTION
    THE NETWORK CORE FIRST, THEN THE APPLICATION CORE, and the order is not
    cosmetic. The application core brings the radio up over IPC as soon as it
    starts; flashed the other way round it talks to whatever netcore image was
    there before, which on this bench is usually something unrelated - and the
    result is a board that boots, answers its CLI, and never sees a radio.

    THEN IT CHECKS. `ot state` on the peer is the first question every failed P4
    run asks, and answering it after the sitting is worthless: a dead peer and a
    broken DUT both present as `detached` followed by silence. So this asserts
    `leader` (or at least `router`) before handing the bench over, and refuses
    otherwise.

    ITS CONSOLE VCOM NEEDS DTR ASSERTED. Without it COM9 reads as completely
    silent, which is indistinguishable from a board that did not boot - a
    misreading that has cost this project an investigation already.

    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.

.EXAMPLE
    .\scripts\flash_p4_peer.ps1
    .\scripts\flash_p4_peer.ps1 -CheckOnly
#>
[CmdletBinding()]
param(
    [string]$Serial   = '001050006310',   # the nRF5340 DK
    [string]$Port     = 'COM9',
    [int]$SettleSeconds = 25,
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$app  = Join-Path $repo 'build\p4peer\cli\zephyr\zephyr.hex'
$net  = Join-Path $repo 'build\p4peer\ipc_radio\zephyr\zephyr.hex'

$jlink = 'C:\Program Files\SEGGER\JLink_V966\JLink.exe'
if (-not (Test-Path $jlink)) { throw "no JLink.exe at $jlink" }

$tmp = Join-Path ([IO.Path]::GetTempPath()) 'radiant_p4peer'
New-Item -ItemType Directory -Force $tmp | Out-Null

function Invoke-JLink([string]$device, [string]$tail, [string]$name) {
    # DisableAutoUpdateFW first, ALWAYS - this JLink is newer than these probes'
    # on-board firmware, its upgrade attempt fails at "Emulator did not
    # re-enumerate" every time, and it leaves the probe in bootloader until
    # somebody physically replugs it. That is how a probe got bricked here once.
    $script = Join-Path $tmp "$name.jlink"
    @"
exec DisableAutoUpdateFW
si SWD
speed 4000
device $device
connect
r
h
$tail
q
"@ | Out-File -FilePath $script -Encoding ascii

    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $jlink -NoGui 1 -SelectEmuBySN $Serial -CommanderScript $script > (Join-Path $tmp "$name.log")
        $rc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($rc -ne 0) { throw "JLink '$name' failed (exit $rc); see $tmp\$name.log" }
}

if (-not $CheckOnly) {
    foreach ($p in @($net, $app)) {
        if (-not (Test-Path $p)) {
            throw "no image at $p - build it with the command in p4_peer.conf"
        }
    }
    Write-Host "netcore  <- $net" -ForegroundColor Cyan
    Invoke-JLink 'nRF5340_xxAA_NET' "loadfile $net" 'net'
    Write-Host "appcore  <- $app" -ForegroundColor Cyan
    Invoke-JLink 'nRF5340_xxAA_APP' "loadfile $app" 'app'
    Invoke-JLink 'nRF5340_xxAA_APP' 'g' 'go'
}

Write-Host "waiting $SettleSeconds s for the peer to form a network..." -ForegroundColor Cyan

# $com, and deliberately NOT $port. The parameter above is [string]$Port and
# PowerShell variable names are case-insensitive, so assigning a SerialPort to
# $port coerces the object to its type name - and the failure then surfaces
# three lines later as "the property 'DtrEnable' cannot be found on this
# object", which points at the serial port rather than at the name clash.
$com = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$com.DtrEnable = $true   # not optional: without it this port returns nothing
$com.RtsEnable = $true
$com.NewLine = "`n"
$com.Open()
try {
    Start-Sleep -Seconds $SettleSeconds
    [void]$com.ReadExisting()

    # Asked twice, a second apart. `ot state` on this CLI has been flaky on this
    # bench before, and one blank answer is not evidence the board is dead - but
    # two are worth reporting rather than papering over.
    $answers = @()
    foreach ($i in 1..3) {
        $com.WriteLine('ot state')
        Start-Sleep -Milliseconds 900
        $answers += $com.ReadExisting()
    }
} finally {
    $com.Close()
}

$text = ($answers -join "`n")
Write-Host "--- peer console ---"
Write-Host $text

$state = ($text -split "`r?`n" |
          Where-Object { $_.Trim() -match '^(leader|router|child|detached|disabled)$' } |
          Select-Object -Last 1)

if (-not $state) {
    throw "no ot state from $Port. DTR is asserted here, so the next suspect " +
          "is the other VCOM of the pair - not a dead board."
}
$state = $state.Trim()
Write-Host ""
Write-Host "peer state: $state" -ForegroundColor $(if ($state -in @('leader','router')) { 'Green' } else { 'Red' })
if ($state -notin @('leader', 'router')) {
    throw "the peer is '$state'. A DUT cannot attach to it, thread_coex_load " +
          "will report sent=0, and a P4 loss figure taken against sent=0 is void."
}
