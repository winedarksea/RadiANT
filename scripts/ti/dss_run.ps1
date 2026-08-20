# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Run any of the scripts\ti\dss_*.js probes against a TI board over the XDS110.

.DESCRIPTION
    The two lookups every DSS script needs, in one place: where UniFlash hides
    dss.bat, and where it hides the JRE that dss.bat needs on PATH. Both were
    originally duplicated into rtt_dump.ps1, and both have failure modes that
    read as a broken install rather than a missing path.

    UniFlash's ccs_base is NOT at <install>\ccs_base - it is buried under
    deskdb\content\TICloudAgent\win, the same place dslite.bat reaches into. A
    full CCS install does put it at the shallow path, so both are searched.

.PARAMETER Script
    The .js to run. A bare name is resolved inside scripts\ti, so `dss_uart.js`
    and a full path both work.

.PARAMETER Ccxml
    Target configuration. Defaults to the dongle's.

.EXAMPLE
    .\scripts\ti\dss_run.ps1 dss_uart.js
    .\scripts\ti\dss_run.ps1 dss_pinmap.js -Ccxml scripts\ti\cc26x2r1_launchxl.ccxml

.NOTES
    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Script,
    [string]$Ccxml,
    [string]$Dss
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Script)) {
    $inTree = Join-Path $PSScriptRoot $Script
    if (Test-Path $inTree) { $Script = $inTree } else { throw "No DSS script at $Script." }
}
$Script = (Resolve-Path $Script).Path

if (-not $Ccxml) { $Ccxml = Join-Path $PSScriptRoot 'cc2652p_dongle.ccxml' }
if (-not (Test-Path $Ccxml)) { throw "No target configuration at $Ccxml." }
$Ccxml = (Resolve-Path $Ccxml).Path

if (-not $Dss) {
    $roots = Get-ChildItem 'C:\ti' -Filter 'uniflash*' -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    $candidates = @()
    foreach ($r in $roots) {
        $candidates += Join-Path $r.FullName 'deskdb\content\TICloudAgent\win\ccs_base\scripting\bin\dss.bat'
        $candidates += Join-Path $r.FullName 'ccs_base\scripting\bin\dss.bat'
    }
    $candidates += 'C:\ti\ccs_base\scripting\bin\dss.bat'
    $Dss = @($candidates | Where-Object { Test-Path $_ })[0]
}
if (-not $Dss -or -not (Test-Path $Dss)) {
    throw @"
No dss.bat found under C:\ti.

It ships inside UniFlash, but NOT at <install>\ccs_base\scripting\bin - that is
the CCS layout. UniFlash's copy is at

  <install>\deskdb\content\TICloudAgent\win\ccs_base\scripting\bin\dss.bat

Pass -Dss <path> for one somewhere else.
"@
}

$jre = @(Get-ChildItem 'C:\ti' -Filter 'java.exe' -Recurse -ErrorAction SilentlyContinue |
         Select-Object -First 1)
if ($jre.Count -gt 0) { $env:PATH = "$(Split-Path $jre[0].FullName -Parent);$env:PATH" }

$env:CCXML = $Ccxml

Write-Host "DSS:    $Dss"
Write-Host "script: $Script"
Write-Host "ccxml:  $Ccxml"
Write-Host ''

& cmd /c "`"$Dss`" `"$Script`""
exit $LASTEXITCODE
