# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
    Build and install the USBXpress shim that makes the CC2652P dongle visible
    to Zwift.

.DESCRIPTION
    The CC2652P has no USB peripheral. It reaches the host as a CP2102N UART
    bridge on a COM port, and Zwift's ANT_DLL cannot use that directly: its
    raw-bulk backend drives endpoints 0x01/0x81 where the chip offers
    0x02/0x82, and it has no usb_control_msg at all, so it can never send the
    IFC_ENABLE a CP210x needs.

    Its THIRD backend has neither problem. ANT_DLL loads Silicon Labs'
    USBXpress pair by name and drives them as a byte pipe - open, set baud,
    read, write. Those two DLLs are simply absent here, so we supply them,
    backed by the COM port the dongle is already on.

      DSI_SiUSBXp_3_1.DLL              the transport   (SI_Open/SI_Read/...)
      DSI_CP210xManufacturing_3_1.dll  the identity    (CP210x_GetDeviceVid)

    ANT_DLL accepts a device on its VENDOR ID ALONE - 0x0FCF or 0x1915, with no
    product-id test anywhere in its binary - so the identity DLL answering
    0x0FCF is sufficient. THE DONGLE ITSELF IS NEVER TOUCHED: no VID/PID
    reprogramming, no driver change, no elevation, and COM15 keeps working for
    every tool in tools/.

    PURELY ADDITIVE. Both filenames are absent from a stock Zwift install, so
    nothing of Zwift's is replaced or shadowed - ANT_DLL simply gains a
    transport it did not have. -Uninstall deletes the two files and the install
    is completely undone.

    Verified end to end: ANT_Init(0, 57600, 0, 0) -> TRUE, ANT_ResetSystem ->
    TRUE with a 7-byte 0xA4 frame on the wire and the dongle's replies read
    back.

.PARAMETER ZwiftDir
    Zwift's program directory. Default: C:\Program Files (x86)\Zwift.
    Needs no elevation: Zwift self-updates, so BUILTIN\Users has Modify there.

.PARAMETER Port
    Force a COM port (e.g. COM15). By default the shim finds the CP2102N's port
    from the registry at run time, so a renumbered port keeps working.

.PARAMETER Uninstall
    Remove the two DLLs and stop.

.PARAMETER BuildOnly
    Build into tools/zwift_shim/out and do not install.

.EXAMPLE
    .\scripts\zwift_ant_shim.ps1
    Build, install, and verify with tools/ant_zwift_visible.py.

.EXAMPLE
    .\scripts\zwift_ant_shim.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string]$ZwiftDir = 'C:\Program Files (x86)\Zwift',
    [string]$Port,
    [switch]$Uninstall,
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $repo 'tools\zwift_shim'
$outDir = Join-Path $srcDir 'out'

$files = @('DSI_SiUSBXp_3_1.DLL', 'DSI_CP210xManufacturing_3_1.dll')

if ($Uninstall) {
    $gone = 0
    foreach ($f in $files) {
        $p = Join-Path $ZwiftDir $f
        if (Test-Path $p) {
            Remove-Item $p -Force
            Write-Host "removed $p" -ForegroundColor Green
            $gone++
        }
    }
    if ($gone -eq 0) { Write-Host 'Nothing to remove.' -ForegroundColor Yellow }
    else { Write-Host 'Zwift is back to stock.' -ForegroundColor Green }
    return
}

# --- find a compiler --------------------------------------------------------
$vcvars = $null
foreach ($root in @("${env:ProgramFiles}\Microsoft Visual Studio",
                    "${env:ProgramFiles(x86)}\Microsoft Visual Studio")) {
    if (-not (Test-Path $root)) { continue }
    $hit = Get-ChildItem $root -Filter 'vcvars64.bat' -Recurse -File -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($hit) { $vcvars = $hit.FullName; break }
}
if (-not $vcvars) {
    throw ("No MSVC vcvars64.bat found. These are Windows DLLs and need the " +
           "MSVC toolchain; the Zephyr ARM toolchain cannot build them.")
}
Write-Host "compiler: $vcvars" -ForegroundColor DarkGray

New-Item -ItemType Directory -Force -Path $outDir, (Join-Path $srcDir 'obj_a'),
                                        (Join-Path $srcDir 'obj_b') | Out-Null

$bat = Join-Path $srcDir 'build.bat'
@"
@echo off
call "$vcvars" >nul 2>&1
cd /d "$srcDir"
set CFLAGS=/nologo /W3 /O2 /D_CRT_SECURE_NO_WARNINGS
cl %CFLAGS% /LD dsi_siusbxp.c shim_util.c /Fo:obj_a\ /Fe:out\DSI_SiUSBXp_3_1.DLL advapi32.lib /link /IMPLIB:obj_a\a.lib
if errorlevel 1 exit /b 1
cl %CFLAGS% /LD dsi_cp210x.c shim_util.c /Fo:obj_b\ /Fe:out\DSI_CP210xManufacturing_3_1.dll advapi32.lib /link /IMPLIB:obj_b\b.lib
if errorlevel 1 exit /b 1
exit /b 0
"@ | Set-Content -Path $bat -Encoding ascii

Write-Host 'building...' -ForegroundColor Cyan
cmd /c "`"$bat`"" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
foreach ($f in $files) {
    $p = Join-Path $outDir $f
    if (-not (Test-Path $p)) { throw "build produced no $f" }
    Write-Host ("  {0}  {1:N0} bytes" -f $f, (Get-Item $p).Length) -ForegroundColor Green
}
if ($BuildOnly) { Write-Host "Built into $outDir (not installed)."; return }

# --- install ----------------------------------------------------------------
if (-not (Test-Path $ZwiftDir)) { throw "Zwift directory not found: $ZwiftDir" }
foreach ($f in $files) {
    $target = Join-Path $ZwiftDir $f
    # Refuse to clobber a real Silicon Labs DLL if one ever appears: this shim
    # is only correct as an ADDITION, never as a replacement.
    if (Test-Path $target) {
        $existing = Get-Item $target
        $ours = Get-Item (Join-Path $outDir $f)
        if ($existing.Length -ne $ours.Length) {
            throw ("$target already exists and is not ours ({0:N0} bytes). " -f $existing.Length) +
                  "Refusing to overwrite - if that is a genuine Silicon Labs DLL, use it instead."
        }
    }
    Copy-Item (Join-Path $outDir $f) $target -Force
    Write-Host "installed $target" -ForegroundColor Green
}

if ($Port) {
    [Environment]::SetEnvironmentVariable('RADIANT_ANT_COM', $Port, 'User')
    Write-Host "pinned RADIANT_ANT_COM=$Port for your user." -ForegroundColor Green
} else {
    Write-Host 'COM port will be discovered from the registry at run time.' -ForegroundColor DarkGray
}

Write-Host ''
Write-Host '--- verifying with Zwift''s own ANT_DLL ---' -ForegroundColor Cyan
$py = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
if (-not (Test-Path $py)) { $py = (Get-Command python.exe -ErrorAction SilentlyContinue).Source }
if ($py) {
    & $py (Join-Path $repo 'tools\ant_zwift_visible.py')
    if ($LASTEXITCODE -eq 0) {
        Write-Host ''
        Write-Host 'PASS - start Zwift and pair. Undo any time with:' -ForegroundColor Green
        Write-Host '  .\scripts\zwift_ant_shim.ps1 -Uninstall' -ForegroundColor Gray
    } else {
        Write-Host ''
        Write-Host 'Verification did not pass - check the dongle is on its COM port' -ForegroundColor Yellow
        Write-Host 'and that nothing else holds it open (close ant_scan etc.).' -ForegroundColor Yellow
    }
} else {
    Write-Host 'No Python found; verify manually with tools\ant_zwift_visible.py'
}
