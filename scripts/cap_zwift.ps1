# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
    Capture the USB traffic between a host application (Zwift) and this dongle.

.DESCRIPTION
    Wraps USBPcapCMD so that a capture of a real Zwift pairing session is one
    command rather than a set of hand-copied device numbers. The original
    scratch version of this script was lost; `archive/host-api/README.md`
    records the decode it produced as unregenerable, which is why this one
    lives in the repository.

    The script finds the USBPcap filter device that the ANT dongle sits behind
    (by asking USBPcap for each root hub's device tree and matching the device
    description), then captures only that device's traffic. Capturing a whole
    root hub also works but buries the ANT stream in whatever else shares the
    controller.

    USBPcap is documented as needing Administrator, but on this bench a normal
    user can open the filter device and capture - so the script runs in place
    by default and only warns. Pass -Elevate if the capture file stays empty;
    that relaunches in a second, elevated window writing to the same path.

.PARAMETER Out
    Destination .pcap. Default: a timestamped file in the repo's capture
    directory (created if missing).

.PARAMETER Seconds
    Stop after this many seconds (default 300). Ctrl-C stops it early and still
    leaves a usable file. 0 means run until Ctrl-C with no time limit.

.PARAMETER Interface
    Force a USBPcap filter device, e.g. \\.\USBPcap2. Skips auto-detection.

.PARAMETER DeviceMatch
    Regex matched against the device descriptions USBPcap reports.
    Default 'ANT USB-m', which is what this firmware enumerates as.

.PARAMETER All
    Capture every device on the selected filter device, not just the match.
    Use if the dongle re-enumerates mid-capture (a reset changes its address,
    and an address filter set before the reset will not follow it).

.PARAMETER Kill
    Stop any USBPcapCMD processes left behind and exit. Ctrl-C normally ends
    the capture cleanly; this is for the cases where it did not.

.PARAMETER Elevate
    Relaunch in an elevated window instead of capturing in place. Only needed
    if an unelevated capture produces an empty file.

.EXAMPLE
    .\scripts\cap_zwift.ps1
    Start capturing, run Zwift, pair, then Ctrl-C.

.EXAMPLE
    .\scripts\cap_zwift.ps1 -Seconds 120 -Out C:\tmp\zwift.pcap
    Then: python tools\decode_pcap.py C:\tmp\zwift.pcap

.EXAMPLE
    .\scripts\cap_zwift.ps1 -Interface '\\.\USBPcap2' -All
    Skips auto-detection. USBPcap's device enumeration is the fragile part -
    it can report nothing while capture still works fine - and on this bench
    the dongle is the only device behind USBPcap2, so -All costs nothing.
#>
[CmdletBinding()]
param(
    [string]$Out,
    [int]$Seconds = 300,
    [string]$Interface,
    [string]$DeviceMatch = 'ANT USB-m',
    [switch]$All,
    [switch]$Elevate,
    [switch]$Kill
)

$ErrorActionPreference = 'Stop'

$usbpcap = Join-Path $env:ProgramFiles 'USBPcap\USBPcapCMD.exe'
if (-not (Test-Path $usbpcap)) {
    throw "USBPcapCMD.exe not found at $usbpcap. Install USBPcap (https://desowin.org/usbpcap/)."
}

if ($Kill) {
    $stray = @(Get-Process USBPcapCMD -ErrorAction SilentlyContinue)
    if (-not $stray) { Write-Host 'No USBPcapCMD processes running.' -ForegroundColor Green; return }
    foreach ($p in $stray) {
        try {
            Stop-Process -Id $p.Id -Force -Confirm:$false -ErrorAction Stop
            Write-Host "Stopped USBPcapCMD (PID $($p.Id))." -ForegroundColor Green
        } catch {
            # An instance started from an elevated window cannot be stopped from
            # an unelevated one; taskkill from an Administrator console does it.
            Write-Warning "PID $($p.Id) refused to stop (likely elevated). From an admin console: taskkill /PID $($p.Id) /F"
        }
    }
    Write-Host 'Note: force-stopping a capture can leave USBPcap unable to enumerate devices' -ForegroundColor DarkGray
    Write-Host "until a reboot. Capture still works - keep using -Interface '\\.\USBPcap2' -All." -ForegroundColor DarkGray
    return
}

if (-not $Out) {
    $Out = Join-Path (Split-Path -Parent $PSScriptRoot) `
        ("captures\zwift-{0}.pcap" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$Out = [System.IO.Path]::GetFullPath($Out)
# Unconditionally - the elevated relaunch is handed a path, not a directory to
# invent, and USBPcapCMD's failure to open the output is easy to misread as a
# device problem.
$outDir = Split-Path -Parent $Out
if ($outDir -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($Elevate -and -not $isAdmin) {
    Write-Host "Relaunching as Administrator." -ForegroundColor Yellow
    Write-Host "Capture will write to: $Out"
    $argList = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-NoExit',
        '-File', "`"$PSCommandPath`"",
        '-Out', "`"$Out`"",
        '-Seconds', $Seconds,
        '-DeviceMatch', "`"$DeviceMatch`""
    )
    if ($Interface) { $argList += @('-Interface', "`"$Interface`"") }
    if ($All)       { $argList += '-All' }
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argList
    return
}
if (-not $isAdmin) {
    # Capturing works unelevated - USBPcapCMD just relaunches itself elevated to
    # do it. The catch is that this script can then no longer stop the capture
    # it started, so -Seconds and Ctrl-C both leave it running. -Elevate keeps
    # the capture process ours.
    Write-Host "Not elevated: the capture cannot be stopped by this script. Use -Elevate." -ForegroundColor DarkGray
}

# --- locate the dongle -----------------------------------------------------
# USBPcap's extcap config lists, per filter device, the tree of attached
# devices as `value {arg=99}{value=<n>}{display=<name>}`. Top-level devices
# have no {parent=...}; only those carry an address usable with --devices.
# Every enumerating call goes through here, and every one of them redirects
# stdout to a file. USBPcapCMD writes its extcap output straight to the console:
# capture it through a PowerShell pipe (`$lines = & $usbpcap ...`) and you get
# *zero lines and no error*, while running it bare looks fine because the text
# you see came from USBPcapCMD's own console write, not from PowerShell. That
# silence is what "USBPcap listed no filter devices" really meant all session,
# and guessing an interface around it is how a capture ended up recording a
# flash drive. Redirected to a file it works, unelevated, every time.
# Also note: no `2>$null` anywhere. PowerShell 5.1 wraps a native command's
# redirected stderr in NativeCommandError records, which under
# $ErrorActionPreference='Stop' discards the stdout being parsed.
function Invoke-UsbPcap {
    param([string[]]$Arguments)
    $stdout = [System.IO.Path]::GetTempFileName()
    $stderr = [System.IO.Path]::GetTempFileName()
    try {
        Start-Process -FilePath $usbpcap -ArgumentList $Arguments -NoNewWindow -Wait `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr | Out-Null
        Get-Content $stdout -ErrorAction SilentlyContinue
    } finally {
        Remove-Item $stdout, $stderr -Force -ErrorAction SilentlyContinue
    }
}

function Get-UsbPcapTree {
    param([string]$Iface)
    $lines = Invoke-UsbPcap -Arguments @('--extcap-interface', $Iface, '--extcap-config')
    foreach ($line in $lines) {
        if ($line -match '^value \{arg=99\}\{value=([^}]+)\}\{display=([^}]*)\}') {
            [pscustomobject]@{
                Address  = $Matches[1]
                Display  = $Matches[2]
                IsTopLevel = ($line -notmatch '\{parent=')
            }
        }
    }
}

if (-not $Interface) {
    $ifaces = @()
    foreach ($line in (Invoke-UsbPcap -Arguments @('--extcap-interfaces'))) {
        if ($line -match '^interface \{value=([^}]+)\}') { $ifaces += $Matches[1] }
    }
    if (-not $ifaces) {
        throw ("USBPcap listed no filter devices, which normally means USBPcap " +
               "is not installed or its driver is not running - check that " +
               "Wireshark can see USBPcap interfaces. Do NOT work around this " +
               "by guessing -Interface: an interface that is not the dongle's " +
               "captures happily and yields a file with no ANT traffic in it.")
    }

    foreach ($iface in $ifaces) {
        $hit = Get-UsbPcapTree -Iface $iface |
               Where-Object { $_.IsTopLevel -and $_.Display -match $DeviceMatch } |
               Select-Object -First 1
        if ($hit) {
            $Interface = $iface
            $address   = $hit.Address
            Write-Host "Found '$($hit.Display)' on $iface as device $address" -ForegroundColor Green
            break
        }
    }
    if (-not $Interface) {
        Write-Host 'Devices USBPcap can see:' -ForegroundColor Yellow
        $anyDevice = $false
        foreach ($iface in $ifaces) {
            Get-UsbPcapTree -Iface $iface |
                Where-Object IsTopLevel |
                ForEach-Object {
                    $anyDevice = $true
                    Write-Host ("  {0}  [{1}] {2}" -f $iface, $_.Address, $_.Display)
                }
        }
        if (-not $anyDevice) {
            throw ("USBPcap listed its filter devices but reported nothing " +
                   "behind any of them. That is the console-write problem " +
                   "described above resurfacing - check Invoke-UsbPcap is " +
                   "still redirecting stdout to a file.")
        }
        throw "No device matching /$DeviceMatch/ found. Plug the dongle in, or pass -Interface and -All."
    }
} else {
    $hit = Get-UsbPcapTree -Iface $Interface |
           Where-Object { $_.IsTopLevel -and $_.Display -match $DeviceMatch } |
           Select-Object -First 1
    if ($hit) { $address = $hit.Address }
}

# No --inject-descriptors. It looks harmless - prepend the descriptors of
# already-connected devices - but on this bench it makes USBPcapCMD exit
# immediately with status 0 and a zero-byte file, i.e. a capture that silently
# never runs. Verified 2026-08-18 by A/B against the identical command line.
$capArgs = @('-d', $Interface, '-o', $Out, '--snaplen', '65535')
if ($address -and -not $All) {
    $capArgs += @('--devices', $address)
} else {
    $capArgs += '-A'
    Write-Host 'Capturing all devices on this filter device.' -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Capturing to $Out" -ForegroundColor Cyan
if ($Seconds -gt 0) { Write-Host "Runs for $Seconds s. Ctrl-C stops early and still leaves a usable file." }
else { Write-Host "Press Ctrl-C when the Zwift session is done." }
Write-Host "Start Zwift and pair now."
Write-Host ""

# USBPcapCMD does not stay in the foreground. When it is not already running
# elevated it relaunches itself, and the process started here exits within
# milliseconds while the capture continues in the detached child. Waiting on
# the process we launched therefore returns instantly and the output file does
# not exist yet - which reads as "the capture never ran" while it is in fact
# running and filling the file, and leaves the child behind looking like a
# stray. So follow whichever USBPcapCMD processes are new, not the launcher.
# (Diagnosed 2026-08-18 from a "failed" run whose file was still growing.)
$before = @(Get-Process USBPcapCMD -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty Id)
$null = Start-Process -FilePath $usbpcap -ArgumentList $capArgs -PassThru -NoNewWindow

$capturePids = @()
foreach ($attempt in 1..20) {
    Start-Sleep -Milliseconds 250
    $capturePids = @(Get-Process USBPcapCMD -ErrorAction SilentlyContinue |
                     Where-Object { $before -notcontains $_.Id } |
                     Select-Object -ExpandProperty Id)
    if ($capturePids -and (Test-Path $Out)) { break }
}
if (-not $capturePids) {
    throw ("USBPcapCMD exited without leaving a capture process running. " +
           "If a UAC prompt appeared and was declined, accept it; otherwise " +
           "check that -Interface names a real filter device.")
}

# USBPcapCMD has no run-for-N-seconds option (-s is snapshot length), so a timed
# run means stopping the process. The finally block also runs on Ctrl-C, so a
# cancelled capture is stopped and reported rather than detaching. Stopping
# mid-record can truncate the last packet; tools/decode_pcap.py tolerates that.
$deadline = if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { [datetime]::MaxValue }
$nextTick = (Get-Date).AddSeconds(15)
try {
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Id $capturePids -ErrorAction SilentlyContinue)) {
            Write-Host "Capture process exited on its own." -ForegroundColor Yellow
            break
        }
        if ((Get-Date) -ge $nextTick) {
            $now = Get-Date
            $nextTick = $now.AddSeconds(15)
            $bytes = if (Test-Path $Out) { (Get-Item $Out).Length } else { 0 }
            $left = if ($Seconds -gt 0) { [int]($deadline - $now).TotalSeconds } else { -1 }
            $note = if ($left -ge 0) { "{0,4}s left" -f $left } else { "no limit" }
            Write-Host ("  {0}  {1:N0} bytes captured" -f $note, $bytes) -ForegroundColor DarkGray
        }
        Start-Sleep -Milliseconds 500
    }
} finally {
    $alive = @(Get-Process -Id $capturePids -ErrorAction SilentlyContinue)
    if ($alive) {
        Write-Host "Stopping capture." -ForegroundColor Cyan
        foreach ($p in $alive) {
            try {
                Stop-Process -Id $p.Id -Force -Confirm:$false -ErrorAction Stop
            } catch {
                # Parenthesised: -f binds tighter than +, so formatting the
                # concatenation as a whole needs the parentheses or only the
                # last fragment gets the argument.
                Write-Warning (("Could not stop PID {0} - it self-elevated. " +
                                "From an admin console: taskkill /PID {0} /F") -f $p.Id)
            }
        }
        Start-Sleep -Milliseconds 500   # let it flush and release the file
    }
}

Write-Host ""
$size = if (Test-Path $Out) { (Get-Item $Out).Length } else { -1 }
if ($size -gt 24) {
    # 24 bytes is a bare pcap file header - a capture that ran but saw nothing.
    Write-Host ("Wrote {0} ({1:N0} bytes)" -f $Out, $size) -ForegroundColor Green
    Write-Host "Decode with:"
    Write-Host "  C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe tools\decode_pcap.py `"$Out`" --summary"
    Write-Host "A real capture names ANT messages there. Two header lines and nothing else means" -ForegroundColor DarkGray
    Write-Host "the bytes are not ANT - check --raw; ANT frames start a4." -ForegroundColor DarkGray
} elseif ($size -ge 0) {
    Write-Warning ("Capture ran but saw no traffic ({0} bytes - a bare pcap header)." -f $size)
    Write-Host "The capture itself worked; nothing was talking to the dongle. Usually that means" -ForegroundColor Yellow
    Write-Host "Zwift was not running or had not paired yet. Re-run and pair while it captures." -ForegroundColor Yellow
} else {
    Write-Warning "No capture file was written at $Out."
}

$left = @(Get-Process USBPcapCMD -ErrorAction SilentlyContinue)
if ($left) {
    Write-Host ("{0} USBPcapCMD process(es) still running - clean up with: .\scripts\cap_zwift.ps1 -Kill" -f $left.Count) -ForegroundColor Yellow
}
