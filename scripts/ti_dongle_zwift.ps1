# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
    Make the CC2652P (TI) dongle visible to Zwift, in one elevated pass.

.DESCRIPTION
    The CC2652P has no USB peripheral. Its only path to the host is a CP2102N
    UART bridge enumerating as 10C4:EA60, and Zwift's ANT_DLL only accepts
    vendor IDs 0x0FCF and 0x1915 - so the bridge has to be given an ANT vendor
    ID before Zwift will look at it twice. See docs/ti-dongle-usb-identity.md
    for every measurement behind that sentence.

    Three steps, and each verifies before the next:

      1. Bind libusb0 to the bridge. Everything needed is already installed and
         signed here (Dynastream's ant_libusb.inf). This is what gives raw
         control-transfer access to the configuration block; the VCP driver
         gives none. Only Device Manager's "untick Show compatible hardware"
         path can bind a driver to a hardware ID its INF does not name, so this
         does it the same way programmatically - pnputil will not, and devcon
         is not installed.
      2. Rewrite the stored VID/PID with tools/cp2102n_ids.py, which refuses to
         write unless it has BOTH matched the stored IDs to the live
         descriptors AND reproduced the block's Fletcher checksum. A bad write
         leaves a part that will not enumerate, and this board has no USB way
         back - only the JTAG rig.
      3. Ask Zwift's own ANT_DLL whether it can now see it.

    PID 0x1008 is chosen deliberately: oem79.inf already binds libusb0 to it,
    so after the rewrite the device has a legitimately matching driver and the
    forced binding from step 1 is no longer doing any work. It also leaves
    0x1009 free, so this board can never be confused with the nRF52840 sticks.

    REVERSIBLE. -Revert puts the identity back to 10C4:EA60 and removes the
    device node so Windows rebinds silabser and COM15 returns.

.PARAMETER Revert
    Restore 10C4:EA60 and hand the device back to the Silicon Labs VCP driver.

.PARAMETER VendorId
    Vendor ID to store. Default 0x0FCF. ANT_DLL accepts only 0x0FCF or 0x1915.

.PARAMETER ProductId
    Product ID to store. Default 0x1008. ANT_DLL ignores the PID entirely.

    Not named -Pid: $Pid is a read-only PowerShell automatic variable (this
    process's own id), so a parameter of that name fails at invocation with
    "Cannot overwrite variable Pid because it is read-only or constant."

.PARAMETER WhatIf
    Do everything except the write - binds, reads, verifies and reports.

.EXAMPLE
    .\scripts\ti_dongle_zwift.ps1
    Accept the one UAC prompt. Runs the whole sequence and reports.

.EXAMPLE
    .\scripts\ti_dongle_zwift.ps1 -Revert
#>
[CmdletBinding()]
param(
    [switch]$Revert,
    [int]$VendorId = 0x0FCF,
    [int]$ProductId = 0x1008,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Find-Python {
    foreach ($c in @(
        'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe',
        (Get-Command python.exe -ErrorAction SilentlyContinue).Source,
        (Get-Command python3.exe -ErrorAction SilentlyContinue).Source)) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    throw "No Python found. The NCS toolchain's interpreter is the one this repo uses."
}

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host ""
    Write-Host "Binding a driver needs Administrator." -ForegroundColor Yellow
    Write-Host "A UAC prompt is about to appear - accept it to continue." -ForegroundColor Yellow
    Write-Host "Nothing has been changed yet; declining leaves the bench exactly as it is." -ForegroundColor DarkGray
    Write-Host ""
    $argList = @('-NoProfile','-ExecutionPolicy','Bypass','-NoExit','-File',"`"$PSCommandPath`"")
    if ($Revert) { $argList += '-Revert' }
    if ($WhatIf) { $argList += '-WhatIf' }
    $argList += @('-VendorId', $VendorId, '-ProductId', $ProductId)
    try {
        Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argList -ErrorAction Stop
        Write-Host "Elevated window launched. Watch it for the result." -ForegroundColor Green
    } catch {
        Write-Host "Elevation declined - nothing was changed." -ForegroundColor Yellow
        Write-Host "You can do step 1 by hand instead, which needs no command line:" -ForegroundColor Gray
        Write-Host "  Device Manager -> Silicon Labs CP210x USB to UART Bridge (COMn)" -ForegroundColor Gray
        Write-Host "  -> Update driver -> Browse -> Let me pick from a list" -ForegroundColor Gray
        Write-Host "  -> untick 'Show compatible hardware'" -ForegroundColor Gray
        Write-Host "  -> 'ANT LibUSB Drivers' / 'ANT USB-m'" -ForegroundColor Gray
        Write-Host "then re-run this script (it skips the bind once libusb0 is on)." -ForegroundColor Gray
    }
    return
}

$py = Find-Python
$idsTool = Join-Path $repo 'tools\cp2102n_ids.py'
$visTool = Join-Path $repo 'tools\ant_zwift_visible.py'

function Get-Bridge {
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match 'VID_10C4&PID_EA60|VID_0FCF&PID_100[489]' } |
        Select-Object -First 1
}

function Get-Service-Of($dev) {
    if (-not $dev) { return '<absent>' }
    (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_Service' `
        -ErrorAction SilentlyContinue).Data
}

Write-Host "=== TI dongle -> Zwift ===" -ForegroundColor Cyan
$dev = Get-Bridge
if (-not $dev) { throw "No CP2102N bridge found (looked for 10C4:EA60 and 0FCF:1004/1008/1009)." }
$svc = Get-Service-Of $dev
Write-Host ("device : {0}" -f $dev.FriendlyName)
Write-Host ("id     : {0}" -f $dev.InstanceId)
Write-Host ("driver : {0}" -f $svc)

if ($Revert) {
    Write-Host ""
    Write-Host "--- reverting identity to 10C4:EA60 ---" -ForegroundColor Cyan
    & $py $idsTool --set-vid 0x10C4 --set-pid 0xEA60 --commit
    Start-Sleep -Seconds 3
    $dev = Get-Bridge
    if ($dev) {
        Write-Host "removing the device node so Windows rebinds the best match..."
        & pnputil.exe /remove-device $dev.InstanceId 2>&1 | Out-Null
    }
    & pnputil.exe /scan-devices 2>&1 | Out-Null
    Start-Sleep -Seconds 3
    $dev = Get-Bridge
    Write-Host ("driver now: {0}" -f (Get-Service-Of $dev)) -ForegroundColor Green
    Write-Host "COM15 (or whichever port) should be back. Check with:"
    Write-Host "  python tools\ant_probe.py --port COMn --baud 57600"
    return
}

# --- step 1: get raw access -------------------------------------------------
if ($svc -eq 'libusb0') {
    Write-Host ""
    Write-Host "step 1: libusb0 already bound - skipping." -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "--- step 1: bind libusb0 to the bridge ---" -ForegroundColor Cyan
    $binder = Join-Path $PSScriptRoot 'bind_forced_driver.py'
    if (-not (Test-Path $binder)) { throw "Missing $binder" }
    $hwid = 'USB\VID_10C4&PID_EA60'
    & $py $binder $hwid 'C:\Windows\INF\oem79.inf'
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "Could not force the binding programmatically." -ForegroundColor Yellow
        Write-Host "Do it in Device Manager instead - it is the same operation:" -ForegroundColor Yellow
        Write-Host "  Silicon Labs CP210x ... -> Update driver -> Browse -> Let me pick"
        Write-Host "  -> untick 'Show compatible hardware' -> 'ANT LibUSB Drivers' / 'ANT USB-m'"
        Write-Host "then re-run this script."
        return
    }
    Start-Sleep -Seconds 3
}

# --- step 2: rewrite the stored identity ------------------------------------
Write-Host ""
Write-Host "--- step 2: read, verify, and rewrite the stored VID/PID ---" -ForegroundColor Cyan
$vidArg = '0x{0:X4}' -f $VendorId
$pidArg = '0x{0:X4}' -f $ProductId
if ($WhatIf) {
    & $py $idsTool --set-vid $vidArg --set-pid $pidArg      # dry run: no --commit
    Write-Host "(-WhatIf: nothing written)" -ForegroundColor Yellow
    return
}
& $py $idsTool --set-vid $vidArg --set-pid $pidArg --commit
if ($LASTEXITCODE -ne 0) {
    Write-Host "Refused or failed - nothing was written. See the message above." -ForegroundColor Yellow
    return
}
Start-Sleep -Seconds 4
& pnputil.exe /scan-devices 2>&1 | Out-Null
Start-Sleep -Seconds 3
$dev = Get-Bridge
Write-Host ("now: {0}  driver {1}" -f $dev.InstanceId, (Get-Service-Of $dev))

# --- step 3: the only verdict that counts -----------------------------------
Write-Host ""
Write-Host "--- step 3: does Zwift's own ANT_DLL see it? ---" -ForegroundColor Cyan
& $py $visTool
$verdict = $LASTEXITCODE
Write-Host ""
if ($verdict -eq 0) {
    Write-Host "PASS - Zwift will find this dongle. Start Zwift and pair." -ForegroundColor Green
} else {
    Write-Host "Not visible yet. If ANT_DLL enumerated it but could not open it," -ForegroundColor Yellow
    Write-Host "that is the expected raw-bulk failure: the CP2102N's endpoints are" -ForegroundColor Yellow
    Write-Host "0x02/0x82 and ANT_DLL drives 0x01/0x81, and ANT_DLL has no" -ForegroundColor Yellow
    Write-Host "usb_control_msg with which to enable the chip's UART. The USBXpress" -ForegroundColor Yellow
    Write-Host "route in docs/ti-dongle-usb-identity.md is what remains." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "To put everything back:  .\scripts\ti_dongle_zwift.ps1 -Revert" -ForegroundColor Gray
}
