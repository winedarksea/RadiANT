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
    see WinRT types without three things at once: Windows.winmd (from the
    Windows Kits UnionMetadata directory - the projected
    Windows.Devices.Bluetooth.* types); System.Runtime.WindowsRuntime.dll
    (the AsTask()/awaiter glue that makes IAsyncOperation<T> awaitable -
    without it every `await` on a WinRT call is a compile error about a
    missing GetAwaiter); and the Facade assemblies, because the winmd's type
    references are to the contract assemblies rather than to mscorlib. Miss
    any one and the errors point at the source file rather than the command
    line.

.PARAMETER Mode
    scan    - active scan, one line per device seen. NOTE: -Name is IGNORED in
              this mode; every device is listed. A scan that "found nothing" is
              therefore only ever evidence about the DURATION.
    connect - find a peripheral by advertised name, connect, subscribe to Heart
              Rate Measurement notifications, and HOLD the connection.
              HEART-RATE SPECIFIC: it throws "no Heart Rate service (0x180D)"
              and drops the link against anything that is not a strap. Use
              `hold` for a treadmill.
    hold    - connect by name, subscribe to FTMS Treadmill Data (0x2ACD) if the
              peripheral has it, and hold, reporting connection status every
              30 s. This is the apps/treadmill coexistence instrument: the
              measurement that section 7.6 of the treadmill reference design
              asks for is FE-C loss while a client is CONNECTED, and `connect`
              could not hold one against an FTMS machine.
    ftms-conformance
            - on-the-wire FTMS server checks (plan Phase 5 item 3): discovery,
              Feature/ranges/data parsing, Control Point unencrypted vs
              encrypted, CCCD and procedure-in-progress ATT errors, control
              permission, Reset (status to the initiator, elapsed time,
              control released), denied motion (treadmill), disconnect
              mid-command. One PASS/FAIL/INCONCLUSIVE/SKIP line per case;
              exit 1 on any FAIL (or INCONCLUSIVE with -Strict). The case list
              and what each detects is in tools\ble_central_ftms.cs.
              PAIRS THE HOST: it unpairs Windows from the device first (for
              the unencrypted case) and then pairs Just Works, so the
              device's pairing window must be open. -SkipUnencrypted keeps an
              existing bond instead.
    reconnect-soak
            - -Cycles cycles of connect -> subscribe (-Notify) -> first
              notification -> disconnect. Prints connect, data, teardown and
              a re-advertise bracket per cycle, min/median/max, and the plan's
              pass criteria (re-advertise <= 2 s, data <= 10 s). How Windows
              is made to really disconnect, and the limits of the re-advertise
              figure, are in tools\ble_central_soak.cs.
    device-log
            - offline: the -DeviceLog analysis of a console capture from an
              earlier soak (device-side disconnect -> re-advertise). No radio.
    selftest
            - runs the radio-free helpers (statistics, verdicts, FTMS record
              parsers, option parsing). Uses no Bluetooth; safe while the
              radio is busy.

.PARAMETER Name
    Substring of the advertised name to connect to (connect/hold only; see the
    note on scan above). Default "RadiANT HR", which is what
    apps\hrm_ble\ble.conf sets CONFIG_BT_DEVICE_NAME to - apps/treadmill
    advertises "RadiANT Treadmill" and needs -Name to say so. (That fragment
    was strap_ble.conf before the strap/ -> hrm_ble/ rename; no file of that
    name exists in the repo any more.)

.PARAMETER Seconds
    Scan duration, or how long to hold the connection.

.PARAMETER Machine
    ftms-conformance: treadmill (Treadmill Data 0x2ACD, denied-motion case) or
    bike (Indoor Bike Data 0x2AD2, Set Target Power/Resistance case). Also
    picks the default -Name when -Name is not given: "RadiANT Treadmill" or
    "RadiANT Trainer".

.PARAMETER Cycles
    reconnect-soak: number of cycles (default 50).

.PARAMETER Notify
    reconnect-soak: what to subscribe to. hrs (0x180D/0x2A37, default),
    treadmill (0x1826/0x2ACD), bike (0x1826/0x2AD2), or SSSS:CCCC in hex.

.PARAMETER DeviceLog
    reconnect-soak: a capture of the device's Zephyr console taken during the
    soak. Its "BLE disconnected" -> "BLE advertising (restart)" times are the
    device-side re-advertise figure, and when given they decide the
    re-advertise verdict. The Windows figure is dominated by Windows holding
    the link ~9 s after release (measured 2026-11-27), not by the device.

.PARAMETER GapSeconds
    reconnect-soak: scanner calibration window before cycle 1 (default 15;
    0 skips it, and the scanner gap is then unmeasured).

.PARAMETER WatchWhileConnecting
    reconnect-soak: keep the active scan running during each connect attempt
    (the original behaviour). Default is to pause it, since a scan and a
    connection initiation share the controller.

.PARAMETER Json
    ftms-conformance / reconnect-soak: also write the results as JSON here.

.PARAMETER DryRun
    Build if needed, print the ble_central.exe command line this invocation
    would run, and exit 0 without touching the radio.

.PARAMETER OutDir
    Where ble_central.exe is built. Default %TEMP%\radiant_ble_central. A
    second checkout building into the default would replace (or, while it
    runs, fail to replace) the first one's executable.

.EXAMPLE
    .\scripts\ble_central.ps1 -Mode scan -Seconds 10
    .\scripts\ble_central.ps1 -Mode connect -Name "RadiANT HR" -Seconds 60
    .\scripts\ble_central.ps1 -Mode ftms-conformance -Machine treadmill -Json ftms.json
    .\scripts\ble_central.ps1 -Mode reconnect-soak -Name "RadiANT HR" -Cycles 50
    .\scripts\ble_central.ps1 -Mode reconnect-soak -Name "RadiANT Treadmill" -Notify treadmill
    .\scripts\ble_central.ps1 -Mode selftest

.NOTES
    connect mode is the P9 instrument. P9's suppression rule only applies while
    a BLE central is CONNECTED, so verifying it means holding a real connection
    open while the ANT+ side is watched from somewhere else - the dongle, or the
    strap's own VCOM log. Start this first, then start the ANT+ observation.
#>
[CmdletBinding()]
param(
    [ValidateSet('scan', 'connect', 'hold', 'ftms-conformance', 'reconnect-soak', 'selftest', 'device-log')]
    [string]$Mode = 'scan',
    [string]$Name = 'RadiANT HR',
    [int]$Seconds = 10,
    [ValidateSet('treadmill', 'bike')]
    [string]$Machine = 'treadmill',
    [ValidateRange(1, 100000)]
    [int]$Cycles = 50,
    [string]$Notify = 'hrs',
    [string]$Json,
    [ValidateRange(1, 3600)]
    [int]$ScanSeconds = 90,
    [switch]$SkipUnencrypted,
    [switch]$UnpairAtEnd,
    [switch]$Uncached,
    [switch]$NoProbe,
    [string]$DeviceLog,
    [ValidateRange(0, 600)]
    [int]$GapSeconds = 15,
    [switch]$WatchWhileConnecting,
    [switch]$Strict,
    [string]$OutDir = (Join-Path $env:TEMP 'radiant_ble_central'),
    [switch]$DryRun,
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'

# Every tools\ble_central*.cs is one program: ble_central.cs holds Main and
# the original modes, the siblings are partial-class files for the newer ones.
$toolsDir = Join-Path $PSScriptRoot '..\tools'
$srcs = @(Get-ChildItem -Path $toolsDir -Filter 'ble_central*.cs' -ErrorAction SilentlyContinue |
          ForEach-Object { $_.FullName })
if (-not ($srcs | Where-Object { (Split-Path $_ -Leaf) -eq 'ble_central.cs' })) {
    throw "No tools\ble_central.cs under $toolsDir."
}

$outDir = $OutDir
if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
$exe = Join-Path $outDir 'ble_central.exe'

$newestSrc = $srcs | ForEach-Object { (Get-Item $_).LastWriteTime } |
             Sort-Object -Descending | Select-Object -First 1
$needBuild = $Rebuild -or (-not (Test-Path $exe)) -or
             ($newestSrc -gt (Get-Item $exe -ErrorAction SilentlyContinue).LastWriteTime)

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

    # Reference Assemblies\...\Facades is the textbook location, but a machine
    # with only the runtime installed (no Visual Studio, no targeting pack -
    # this bench) does not have it at all, and the failure is not "file not
    # found": csc reports CS0012 pointing at Windows.winmd, claiming the winmd
    # itself needs a reference to System.Runtime. The GAC copies are the same
    # assemblies and are what the runtime binds to anyway, so they are
    # searched as an equal alternative, not a degraded fallback.
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
    & $csc /nologo /target:exe /platform:x64 /out:$exe @refs $srcs
    if ($LASTEXITCODE -ne 0) {
        throw "csc.exe failed with $LASTEXITCODE."
    }
}

if ($Mode -eq 'scan') {
    $a = @('scan', $Seconds)
} elseif ($Mode -eq 'hold') {
    $a = @('hold', $Name, $Seconds)
} elseif ($Mode -eq 'selftest') {
    $a = @('selftest')
} elseif ($Mode -eq 'device-log') {
    if (-not $DeviceLog) { throw '-Mode device-log needs -DeviceLog <console capture>.' }
    $a = @('device-log', $DeviceLog)
} elseif ($Mode -eq 'ftms-conformance') {
    if (-not $PSBoundParameters.ContainsKey('Name')) {
        # 'RadiANT HR' is the strap; an FTMS run against it can only fail.
        $Name = if ($Machine -eq 'bike') { 'RadiANT Trainer' } else { 'RadiANT Treadmill' }
    }
    $a = @('ftms-conformance', $Name, '--machine', $Machine, '--scan-seconds', $ScanSeconds)
    if ($Json) { $a += @('--json', $Json) }
    if ($SkipUnencrypted) { $a += '--skip-unencrypted' }
    if ($UnpairAtEnd) { $a += '--unpair-at-end' }
    if ($Strict) { $a += '--strict' }
} elseif ($Mode -eq 'reconnect-soak') {
    $a = @('reconnect-soak', $Name, '--cycles', $Cycles, '--notify', $Notify, '--scan-seconds', $ScanSeconds)
    if ($Json) { $a += @('--json', $Json) }
    if ($Uncached) { $a += '--uncached' }
    if ($NoProbe) { $a += '--no-probe' }
    if ($DeviceLog) { $a += @('--device-log', $DeviceLog) }
    if ($PSBoundParameters.ContainsKey('GapSeconds')) { $a += @('--gap-seconds', $GapSeconds) }
    if ($WatchWhileConnecting) { $a += '--watch-while-connecting' }
    if ($Strict) { $a += '--strict' }
} else {
    $a = @('connect', $Name, $Seconds)
}

if ($DryRun) {
    # The exact command, one argument per line so an embedded space is
    # visible. Builds (if needed) but touches no radio.
    Write-Host "would run: $exe"
    foreach ($x in $a) { Write-Host "  [$x]" }
    exit 0
}
& $exe @a
exit $LASTEXITCODE
