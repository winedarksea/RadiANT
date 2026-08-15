# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Capture a board's log VCOM to a file for a fixed number of seconds.

.DESCRIPTION
    A minimal, correct console reader, extracted because every ad-hoc one
    written on this bench has got the same thing wrong.

    DtrEnable AND RtsEnable, before Open(). System.IO.Ports.SerialPort
    defaults DtrEnable to $false, and J-Link's CDC then hands over one
    128-byte chunk and goes silent - which looks precisely like a board that
    hung immediately after boot, and has been debugged as one more than once.
    On the nRF54L15 DK the log is COM7 and the ANT byte stream is COM8; on the
    nRF5340 DK the log is COM9. Both need this.

    ReadExisting() in a poll loop, not ReadLine(): ReadLine() blocks until a
    terminator arrives, so a board that stops logging hangs the capture past
    its own deadline rather than returning what it did say.

    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.

.EXAMPLE
    .\scripts\capture_log.ps1 -Port COM7 -Seconds 130 -Out bench-logs\sweep.log
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [int]$Seconds = 60,
    [int]$Baud = 115200,
    [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = 'Stop'

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.DtrEnable = $true
$sp.RtsEnable = $true
$sp.ReadTimeout = 250
$sp.Open()

$sw = [Diagnostics.Stopwatch]::StartNew()
$sb = New-Object System.Text.StringBuilder
try {
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk) { [void]$sb.Append($chunk) }
        } catch [TimeoutException] {
        }
        Start-Sleep -Milliseconds 100
    }
} finally {
    $sp.Close()
    $sp.Dispose()
}

Set-Content -Path $Out -Value $sb.ToString() -Encoding ascii
$lines = (Get-Content $Out | Measure-Object -Line).Lines
Write-Host "captured $lines lines from $Port over $Seconds s -> $Out"
