# SPDX-License-Identifier: Apache-2.0
#
# Flash an arbitrary build directory to the nRF54L15 DK and let it run. The
# capture half lives in p4_bench.ps1; this is for the case where the host tools
# are the instrument and the console is not being read.
#
# ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [string]$Serial = '001057737173',
    [string]$Device = 'nRF54L15_M33'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$hex = Join-Path $repo "build\$Dir\dongle_thread\zephyr\zephyr.hex"
if (-not (Test-Path $hex)) { throw "no image at $hex" }

$jlink = 'C:\Program Files\SEGGER\JLink_V966\JLink.exe'
$tmp = Join-Path ([IO.Path]::GetTempPath()) 'radiant_p4'
New-Item -ItemType Directory -Force $tmp | Out-Null

# DisableAutoUpdateFW first, always - see p4_bench.ps1 for what losing that line
# costs (a probe stuck in bootloader until it is physically replugged).
@"
exec DisableAutoUpdateFW
si SWD
speed 4000
device $Device
connect
r
h
loadfile $hex
r
g
q
"@ | Out-File -FilePath (Join-Path $tmp 'only.jlink') -Encoding ascii

$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    & $jlink -NoGui 1 -SelectEmuBySN $Serial -CommanderScript (Join-Path $tmp 'only.jlink') > (Join-Path $tmp 'only.log')
    $rc = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $prev
}
if ($rc -ne 0) { throw "JLink failed; see $tmp\only.log" }
Write-Host "flashed $hex and released the core" -ForegroundColor Green
Start-Sleep -Seconds 3
