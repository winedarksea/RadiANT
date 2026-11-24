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

    Its THIRD backend has neither problem: ANT_DLL loads Silicon Labs'
    USBXpress pair by name and drives them as a byte pipe (open, set baud,
    read, write). Those two DLLs are absent here, so we supply them, backed
    by the COM port the dongle is already on:

      DSI_SiUSBXp_3_1.DLL              the transport   (SI_Open/SI_Read/...)
      DSI_CP210xManufacturing_3_1.dll  the identity    (CP210x_GetDeviceVid)

    ANT_DLL accepts a device on its VENDOR ID ALONE - 0x0FCF or 0x1915, with
    no product-id test anywhere in its binary - so the identity DLL answering
    0x0FCF is sufficient. THE DONGLE ITSELF IS NEVER TOUCHED: no VID/PID
    reprogramming, no driver change, no elevation, and the COM port keeps
    working for every tool in tools/.

    PURELY ADDITIVE: both filenames are absent from a stock Zwift install, so
    nothing of Zwift's is replaced or shadowed. -Uninstall deletes the two
    files and the install is completely undone.

    THE SHIM DRIVES ONLY THE PORT YOU SELECT. It forwards every host write,
    Zwift's ANT+ network key included, so it never guesses: with neither
    -Port nor -Device pinned (RADIANT_ANT_COM / RADIANT_ANT_DEVICE), Zwift
    sees no device. -ListDevices shows the candidates.

    "OURS" IS DECIDED BY THE DLL'S VERSION RESOURCE (ProductName "RadiANT
    Zwift ANT shim", read without loading the file), or, for the one build
    that predates the resource (903718a, installed 2026-11-23), by its exact
    SHA-256. Install replaces only a file that is ours; -Uninstall deletes
    only a file that is ours. Anything else - a genuine Silicon Labs DLL,
    say - is left alone and reported.

    The shim is always built here, from this checkout, with the local MSVC.
    It is not a release artifact (SECURITY.md).

.PARAMETER ZwiftDir
    Zwift's program directory. Default: C:\Program Files (x86)\Zwift.
    Needs no elevation: Zwift self-updates, so BUILTIN\Users has Modify there.

.PARAMETER Port
    Pin RADIANT_ANT_COM (COM1..COM999) for your user: the shim drives that
    port.

.PARAMETER Device
    Pin RADIANT_ANT_DEVICE for your user: a USB serial number, or a device
    instance id, of the port or of a USB device above it. It must name
    exactly one present COM port, so the port number may change. An XDS110
    has two ports under one serial number: give the instance id of the one
    you want (see -ListDevices).

.PARAMETER ListDevices
    List present COM ports with their instance ids and USB serial numbers,
    then stop.

.PARAMETER LogLevel
    Pin RADIANT_SHIM_LOG_LEVEL (0, 1 or 2) for your user, as -Port pins the
    port. Only matters when RADIANT_SHIM_LOG names a log file. 0 = setup,
    identity and errors only (long rides); 1 = also timed reads and writes
    (default); 2 = also the hex of every read and write and SI_CheckRXQueue.
    See tools/zwift_shim/README.md.

.PARAMETER Python
    The Python interpreter to verify the install with
    (tools/ant_zwift_visible.py). Default: $env:RADIANT_PYTHON. With
    neither, the verify command is printed rather than run: no interpreter
    is taken from a directory other local users could write to.

.PARAMETER Uninstall
    Remove the two DLLs, if they are ours, and stop.

.PARAMETER BuildOnly
    Build into tools/zwift_shim/out and do not install.

.EXAMPLE
    .\scripts\zwift_ant_shim.ps1 -Port COM11
    Build, install, pin the port, and print the verify command.

.EXAMPLE
    .\scripts\zwift_ant_shim.ps1 -ListDevices

.EXAMPLE
    .\scripts\zwift_ant_shim.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string]$ZwiftDir = 'C:\Program Files (x86)\Zwift',
    [ValidatePattern('^[Cc][Oo][Mm][0-9]{1,3}$')]
    [string]$Port,
    [ValidatePattern('^[\x21-\x7E]{1,200}$')]
    [string]$Device,
    [switch]$ListDevices,
    [ValidateSet('0', '1', '2')]
    [string]$LogLevel,
    [string]$Python = $env:RADIANT_PYTHON,
    [switch]$Uninstall,
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $repo 'tools\zwift_shim'
$outDir = Join-Path $srcDir 'out'

$files = @('DSI_SiUSBXp_3_1.DLL', 'DSI_CP210xManufacturing_3_1.dll')

# Must match SHIM_PRODUCT_NAME in tools/zwift_shim/shim_version.h.
$productName = 'RadiANT Zwift ANT shim'

# Builds that predate the version resource, recognised by exact SHA-256 so
# they can be upgraded or removed. Only 903718a (merged 92239e9), the build
# installed in Zwift on 2026-11-23, is listed: an older one is replaced by
# -Uninstall after checking it by hand, never by a size or name match.
$legacyShims = @{
    'D847DEABB9C4FEB5149AA2361587001AEC0A804F45CF86BD0A7CDE7D9DCC82CB' = '903718a DSI_SiUSBXp_3_1.DLL'
    '60BA760846A48150F012D1D47C578FAF98B2DAB1E2CCDD7ACBBEA18D3C773599' = '903718a DSI_CP210xManufacturing_3_1.dll'
}

# 'ours' (version resource), 'legacy' (a listed hash) or 'foreign'.
function Get-ShimOwner([string]$path) {
    $vi = (Get-Item -LiteralPath $path).VersionInfo
    if ($vi.ProductName -eq $productName) { return 'ours' }
    $h = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($legacyShims.ContainsKey($h)) { return 'legacy' }
    return 'foreign'
}

function Get-ShimPorts {
    $enum = 'HKLM:\SYSTEM\CurrentControlSet\Enum'
    foreach ($d in Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue) {
        $name = (Get-ItemProperty -LiteralPath "$enum\$($d.InstanceId)\Device Parameters" `
                    -Name PortName -ErrorAction SilentlyContinue).PortName
        if ($name -notmatch '^COM[0-9]{1,3}$') { continue }
        # The serial number the shim matches: the last part of the nearest
        # USB instance id that Windows did not make up (no '&').
        $serial = $null
        $id = $d.InstanceId
        for ($i = 0; $i -lt 16 -and $id; $i++) {
            $last = ($id -split '\\')[-1]
            if ($id -like 'USB\*' -and $last -notmatch '&') { $serial = $last; break }
            $id = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Parent `
                       -ErrorAction SilentlyContinue).Data
        }
        [pscustomobject]@{ Port = $name; Serial = $serial; InstanceId = $d.InstanceId
                           Name = $d.FriendlyName }
    }
}

function Show-ShimPorts {
    $ports = @(Get-ShimPorts | Sort-Object { [int]($_.Port.Substring(3)) })
    if (-not $ports) { Write-Host 'No COM ports present.' -ForegroundColor Yellow; return }
    $ports | Format-Table Port, Serial, InstanceId, Name -AutoSize | Out-String -Width 400 |
        Write-Host
    Write-Host ('Pin one with -Port COMn, or -Device <serial> when that serial ' +
                'names one port only, else -Device <InstanceId>.') -ForegroundColor DarkGray
}

if ($ListDevices) { Show-ShimPorts; return }

if ($Uninstall) {
    $gone = 0
    $kept = 0
    foreach ($f in $files) {
        $p = Join-Path $ZwiftDir $f
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $owner = Get-ShimOwner $p
        if ($owner -eq 'foreign') {
            Write-Host "kept $p : not a RadiANT shim (no '$productName' version resource, unknown hash)" `
                -ForegroundColor Yellow
            $kept++
            continue
        }
        Remove-Item -LiteralPath $p -Force
        Write-Host "removed $p ($owner)" -ForegroundColor Green
        $gone++
    }
    if ($kept) { Write-Host "$kept file(s) left in place; remove them by hand only if you know what they are." -ForegroundColor Yellow }
    if ($gone -eq 0 -and $kept -eq 0) { Write-Host 'Nothing to remove.' -ForegroundColor Yellow }
    elseif ($kept -eq 0) { Write-Host 'Zwift is back to stock.' -ForegroundColor Green }
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

$testDir = Join-Path $outDir 'test'
New-Item -ItemType Directory -Force -Path $outDir, $testDir,
    (Join-Path $srcDir 'obj_a'), (Join-Path $srcDir 'obj_b'),
    (Join-Path $srcDir 'obj_t') | Out-Null

# /W4 /WX: no warning survives a build. /sdl: extra runtime and compile-time
# checks. /guard:cf: Control Flow Guard (the DLL runs inside Zwift).
# out\test\ holds a USBXpress DLL built with SHIM_TEST_PIPES, which also
# opens pipe\radiant_shim_* so test_shim_log.py can run on a named pipe; it
# is never installed.
$bat = Join-Path $srcDir 'build.bat'
@"
@echo off
call "$vcvars" >nul 2>&1
cd /d "$srcDir"
set CFLAGS=/nologo /W4 /WX /O2 /sdl /guard:cf
set LFLAGS=/link /guard:cf /DYNAMICBASE /NXCOMPAT
rc /nologo /DSHIM_XP /fo obj_a\shim_version.res shim_version.rc
if errorlevel 1 exit /b 1
rc /nologo /fo obj_b\shim_version.res shim_version.rc
if errorlevel 1 exit /b 1
cl %CFLAGS% /LD dsi_siusbxp.c shim_util.c shim_port.c obj_a\shim_version.res /Fo:obj_a\ /Fe:out\DSI_SiUSBXp_3_1.DLL advapi32.lib cfgmgr32.lib %LFLAGS% /IMPLIB:obj_a\a.lib
if errorlevel 1 exit /b 1
cl %CFLAGS% /LD dsi_cp210x.c shim_util.c obj_b\shim_version.res /Fo:obj_b\ /Fe:out\DSI_CP210xManufacturing_3_1.dll advapi32.lib %LFLAGS% /IMPLIB:obj_b\b.lib
if errorlevel 1 exit /b 1
cl %CFLAGS% /DSHIM_TEST_PIPES /LD dsi_siusbxp.c shim_util.c shim_port.c obj_a\shim_version.res /Fo:obj_t\ /Fe:out\test\DSI_SiUSBXp_3_1.DLL advapi32.lib cfgmgr32.lib %LFLAGS% /IMPLIB:obj_t\t.lib
if errorlevel 1 exit /b 1
exit /b 0
"@ | Set-Content -Path $bat -Encoding ascii

Write-Host 'building...' -ForegroundColor Cyan
$buildLog = Join-Path $srcDir 'obj_a\build.log'
cmd /c "`"$bat`" > `"$buildLog`" 2>&1"
if ($LASTEXITCODE -ne 0) {
    Get-Content -LiteralPath $buildLog | Write-Host
    throw "build failed (exit $LASTEXITCODE); log: $buildLog"
}
foreach ($f in $files) {
    $p = Join-Path $outDir $f
    if (-not (Test-Path $p)) { throw "build produced no $f" }
    if ((Get-ShimOwner $p) -ne 'ours') { throw "built $f carries no '$productName' version resource" }
    Write-Host ("  {0}  {1:N0} bytes, version {2}" -f $f, (Get-Item $p).Length,
                (Get-Item $p).VersionInfo.ProductVersion) -ForegroundColor Green
}
if ($BuildOnly) { Write-Host "Built into $outDir (not installed)."; return }

# --- install ----------------------------------------------------------------
if (-not (Test-Path $ZwiftDir)) { throw "Zwift directory not found: $ZwiftDir" }
# Check both files before copying either, so a refusal leaves no half-install.
foreach ($f in $files) {
    $target = Join-Path $ZwiftDir $f
    if ((Test-Path -LiteralPath $target) -and (Get-ShimOwner $target) -eq 'foreign') {
        throw ("$target already exists and is not a RadiANT shim (no '$productName' " +
               "version resource, unknown hash). Refusing to overwrite - if that is a " +
               "genuine Silicon Labs DLL, use it instead.")
    }
}
foreach ($f in $files) {
    $target = Join-Path $ZwiftDir $f
    Copy-Item (Join-Path $outDir $f) $target -Force
    Write-Host "installed $target" -ForegroundColor Green
}

if ($Port) {
    [Environment]::SetEnvironmentVariable('RADIANT_ANT_COM', $Port.ToUpper(), 'User')
    Write-Host "pinned RADIANT_ANT_COM=$($Port.ToUpper()) for your user." -ForegroundColor Green
}
if ($Device) {
    [Environment]::SetEnvironmentVariable('RADIANT_ANT_DEVICE', $Device, 'User')
    Write-Host "pinned RADIANT_ANT_DEVICE=$Device for your user." -ForegroundColor Green
}
if ($LogLevel) {
    [Environment]::SetEnvironmentVariable('RADIANT_SHIM_LOG_LEVEL', $LogLevel, 'User')
    Write-Host "pinned RADIANT_SHIM_LOG_LEVEL=$LogLevel for your user." -ForegroundColor Green
}
$selCom = [Environment]::GetEnvironmentVariable('RADIANT_ANT_COM', 'User')
$selDev = [Environment]::GetEnvironmentVariable('RADIANT_ANT_DEVICE', 'User')
if (-not $selCom -and -not $selDev) {
    Write-Host ''
    Write-Host ('No port is selected, so Zwift will see no device. Pin one with ' +
                '-Port or -Device:') -ForegroundColor Yellow
    Show-ShimPorts
} else {
    Write-Host ("selection for your user: RADIANT_ANT_COM={0} RADIANT_ANT_DEVICE={1}" -f
                $selCom, $selDev) -ForegroundColor DarkGray
}

Write-Host ''
Write-Host '--- verifying with Zwift''s own ANT_DLL ---' -ForegroundColor Cyan
$verify = Join-Path $repo 'tools\ant_zwift_visible.py'
if ($Python -and (Test-Path -LiteralPath $Python -PathType Leaf)) {
    # A new process reads the user environment pinned above only after a new
    # login, so hand the selection to this one.
    if ($selCom) { $env:RADIANT_ANT_COM = $selCom }
    if ($selDev) { $env:RADIANT_ANT_DEVICE = $selDev }
    & $Python -I $verify
    if ($LASTEXITCODE -eq 0) {
        Write-Host ''
        Write-Host 'PASS - start Zwift and pair. Undo any time with:' -ForegroundColor Green
        Write-Host '  .\scripts\zwift_ant_shim.ps1 -Uninstall' -ForegroundColor Gray
    } else {
        Write-Host ''
        Write-Host 'Verification did not pass - check the dongle is on the selected port' -ForegroundColor Yellow
        Write-Host 'and that nothing else holds it open (close ant_scan etc.).' -ForegroundColor Yellow
    }
} else {
    if ($Python) { Write-Host "-Python / RADIANT_PYTHON is not a file: $Python" -ForegroundColor Yellow }
    Write-Host 'Verify with an interpreter you trust (pass -Python or set RADIANT_PYTHON to run it here):'
    Write-Host "  <python.exe> -I `"$verify`"" -ForegroundColor Gray
}
