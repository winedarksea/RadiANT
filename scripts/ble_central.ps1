# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Drive the Windows host BLE radio as a central, to observe or connect to the
    strap's SIG Heart Rate Service.

.DESCRIPTION
    Builds tools\ble_central.cs with the .NET Framework compiler that ships with
    Windows and runs it. There is no SDK to install: csc.exe is in the Windows
    directory and the WinRT metadata comes with the Windows 10 SDK that the
    Nordic tooling already pulled in.

    THE REFERENCE SET IS THE ENTIRE DIFFICULTY. A .NET Framework build cannot
    see WinRT types without three things at once:
      - Windows.winmd, from the Windows Kits UnionMetadata directory, which is
        where the projected Windows.Devices.Bluetooth.* types live;
      - System.Runtime.WindowsRuntime.dll, which supplies the AsTask()/awaiter
        glue that makes IAsyncOperation<T> awaitable - without it every `await`
        on a WinRT call is a compile error about a missing GetAwaiter;
      - the Facade assemblies, because the winmd's type references are to the
        contract assemblies rather than to mscorlib.
    Miss any one and the errors point at the source file rather than at the
    command line, which is why they are listed here with names attached.

    ASCII only, and no `&&`: Windows PowerShell 5.1 reads .ps1 as ANSI without a
    BOM and has no pipeline chain operators.

.PARAMETER Mode
    scan    - active scan, one line per device seen.
    connect - find a peripheral by advertised name, connect, subscribe to Heart
              Rate Measurement notifications, and HOLD the connection.

.PARAMETER Name
    Substring of the advertised name to connect to. Default "RadiANT HR", which
    is what apps\hrm_ble\ble.conf sets CONFIG_BT_DEVICE_NAME to. (That fragment
    was strap_ble.conf before the strap/ -> hrm_ble/ rename; no file of that
    name exists in the repo any more.)

.PARAMETER Seconds
    Scan duration, or how long to hold the connection.

.EXAMPLE
    .\scripts\ble_central.ps1 -Mode scan -Seconds 10
    .\scripts\ble_central.ps1 -Mode connect -Name "RadiANT HR" -Seconds 60

.NOTES
    connect mode is the P9 instrument. P9's suppression rule only applies while
    a BLE central is CONNECTED, so verifying it means holding a real connection
    open while the ANT+ side is watched from somewhere else - the dongle, or the
    strap's own VCOM log. Start this first, then start the ANT+ observation.
#>
[CmdletBinding()]
param(
    [ValidateSet('scan', 'connect')]
    [string]$Mode = 'scan',
    [string]$Name = 'RadiANT HR',
    [int]$Seconds = 10,
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'

$src = Join-Path $PSScriptRoot '..\tools\ble_central.cs'
if (-not (Test-Path $src)) {
    throw "No source at $src."
}
$src = (Resolve-Path $src).Path

$outDir = Join-Path $env:TEMP 'radiant_ble_central'
if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
$exe = Join-Path $outDir 'ble_central.exe'

$needBuild = $Rebuild -or (-not (Test-Path $exe)) -or
             ((Get-Item $src).LastWriteTime -gt (Get-Item $exe -ErrorAction SilentlyContinue).LastWriteTime)

if ($needBuild) {
    $csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    if (-not (Test-Path $csc)) {
        throw "No csc.exe at $csc. This needs the .NET Framework 4.x compiler that ships with Windows."
    }

    # Newest SDK metadata version wins - the Bluetooth LE surface used here has
    # been stable since 10.0.15063, so any installed version works and picking
    # the newest avoids hardcoding one this machine may not have.
    $unionRoot = 'C:\Program Files (x86)\Windows Kits\10\UnionMetadata'
    $winmd = Get-ChildItem $unionRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'Windows.winmd' } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1

    if (-not $winmd) {
        throw "No Windows.winmd under $unionRoot. Install the Windows 10/11 SDK."
    }

    # Resolve each support assembly by name. Reference Assemblies\...\Facades is
    # the textbook location and is what every write-up of this assumes, but a
    # machine with only the runtime installed (no Visual Studio, no targeting
    # pack - i.e. this bench) does not have that directory AT ALL, and the
    # failure is not "file not found": csc reports CS0012 pointing at
    # Windows.winmd, claiming the winmd itself needs a reference to
    # System.Runtime. The GAC copies are the same assemblies and are what the
    # runtime binds to anyway, so they are searched as an equal alternative
    # rather than a degraded fallback.
    $gacRoot = 'C:\Windows\Microsoft.NET\assembly\GAC_MSIL'
    $facadeDir = 'C:\Program Files (x86)\Reference Assemblies\Microsoft\Framework\.NETFramework\v4.5\Facades'

    function Resolve-SupportAssembly([string]$asmName) {
        $inFacade = Join-Path $facadeDir "$asmName.dll"
        if (Test-Path $inFacade) {
            return $inFacade
        }
        $hit = Get-ChildItem (Join-Path $gacRoot $asmName) -Recurse -Filter "$asmName.dll" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($hit) {
            return $hit.FullName
        }
        return $null
    }

    # System.Runtime is the one CS0012 names; the two WindowsRuntime assemblies
    # are what make an IAsyncOperation<T> awaitable and what projects IBuffer.
    $required = @('System.Runtime', 'System.Runtime.WindowsRuntime',
                  'System.Runtime.InteropServices.WindowsRuntime')

    $refs = @("/reference:$winmd")
    foreach ($asm in $required) {
        $path = Resolve-SupportAssembly $asm
        if (-not $path) {
            throw "Could not find $asm.dll in either $facadeDir or the GAC. WinRT interop from .NET Framework is not available on this machine."
        }
        $refs += "/reference:$path"
    }

    Write-Host "Building ble_central.exe"
    Write-Host "  winmd: $winmd"
    & $csc /nologo /target:exe /platform:x64 /out:$exe @refs $src
    if ($LASTEXITCODE -ne 0) {
        throw "csc.exe failed with $LASTEXITCODE."
    }
}

if ($Mode -eq 'scan') {
    & $exe scan $Seconds
} else {
    & $exe connect $Name $Seconds
}
exit $LASTEXITCODE
