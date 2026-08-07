<#
.SYNOPSIS
    Clear Windows' cached USB descriptor verdicts for the ANT dongle.

.DESCRIPTION
    Windows queries a device's MS OS descriptors exactly once and caches the
    outcome under

        HKLM\SYSTEM\CurrentControlSet\Control\usbflags\<vid><pid><bcdDevice>

    in the OSVC value. Per Microsoft's documentation, if that first query fails
    Windows records "no MS OS descriptors" and never asks again - so a single
    bad firmware build poisons the VID/PID and every later, correct build looks
    equally broken. SkipBOSDescriptorQuery, if some earlier experiment set it,
    suppresses the USB 2.1 BOS query that carries the MS OS 2.0 platform
    capability, with the same effect.

    This removes those cached verdicts and un-enrolls any currently attached
    instance of the device so the next plug-in re-enumerates from scratch. The
    alternative that needs no elevation is to bump bcdDevice - see
    CONFIG_ANT_DONGLE_BCD_DEVICE in Kconfig.

    Requires an elevated PowerShell. Supports -WhatIf.

    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.

.PARAMETER Vid
    Vendor ID, hex, no prefix. 0FCF = Dynastream.

.PARAMETER ProductId
    Product ID, hex, no prefix. 1008 = ANTUSB2.

.EXAMPLE
    .\scripts\reset_usb_cache.ps1 -WhatIf
    .\scripts\reset_usb_cache.ps1
#>
[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [string]$Vid = '0FCF',
    [string]$ProductId = '1008'
)

$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal] `
        [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)

if (-not $isAdmin) {
    throw "Run this from an elevated PowerShell (Run as Administrator). The usbflags key is not writable otherwise."
}

$usbFlags = 'HKLM:\SYSTEM\CurrentControlSet\Control\usbflags'
$prefix = "$Vid$ProductId"

# --- 1. Cached MS OS / BOS descriptor verdicts -----------------------------

$keys = @()
if (Test-Path $usbFlags) {
    $keys = @(Get-ChildItem $usbFlags | Where-Object { $_.PSChildName -like "$prefix*" })
}

if (-not $keys) {
    Write-Host "No usbflags entries matching $prefix* - nothing cached."
} else {
    foreach ($key in $keys) {
        $props = Get-ItemProperty -Path $key.PSPath
        $names = @($props.PSObject.Properties |
                Where-Object { $_.Name -notlike 'PS*' } |
                ForEach-Object { $_.Name })
        if ($names) { $detail = $names -join ', ' } else { $detail = '(no values)' }

        if ($PSCmdlet.ShouldProcess("$usbFlags\$($key.PSChildName) [$detail]", 'Remove registry key')) {
            Remove-Item -Path $key.PSPath -Recurse -Force
            Write-Host "Removed $($key.PSChildName) [$detail]"
        }
    }
}

# --- 2. Any attached instance of the device --------------------------------

$devices = @(Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match "VID_$Vid&PID_$ProductId" })

if (-not $devices) {
    Write-Host "No attached VID_$Vid&PID_$ProductId device to un-enroll."
} else {
    foreach ($device in $devices) {
        if ($PSCmdlet.ShouldProcess($device.InstanceId, 'pnputil /remove-device')) {
            # /remove-device is the documented way to drop a device node and its
            # driver binding; the next enumeration then starts from scratch.
            & pnputil.exe /remove-device $device.InstanceId
            Write-Host "Removed device node $($device.InstanceId) (status was $($device.Status))"
        }
    }
}

Write-Host ""
Write-Host "Done. Unplug the Feather, wait a couple of seconds, and plug it back in."
Write-Host "Then confirm binding with:"
Write-Host "  Get-PnpDevice | Where-Object InstanceId -match 'VID_$Vid' | Format-List FriendlyName,Class,Status,InstanceId"
