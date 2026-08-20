# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Read a TI board's Zephyr RTT log over the XDS110, with no serial port.

.DESCRIPTION
    The console for boards that have none. On the CC2652P + CP2102N dongle the
    single wired UART carries the ANT wire protocol and nothing else may appear
    in it, and the second UART's pads are inside a sealed shell - so the log
    goes to an RTT ring buffer in RAM and this reads it back through the same
    JTAG rig that flashed the image.

    NO USB CONNECTION TO THE DONGLE IS NEEDED. That is the property that makes
    bring-up on this board possible at all: flash, read the boot banner, decide
    what is wrong, all over four wires.

    Two lookups happen here rather than in the DSS script, because both are
    exact and free from the host side and both have a failure mode that is
    silent from inside the target:

      * The RTT control block address, read out of the ELF that was flashed.
        Searching RAM for the "SEGGER RTT" signature also works, and is what
        SEGGER's tools do, but it can find a STALE block from a previous image
        and report an old boot as a new one.
      * java.exe, which dss.bat needs on PATH and which UniFlash ships its own
        copy of. Without it dss.bat fails with a bare "java is not recognized",
        which reads like a broken install rather than a PATH.

.PARAMETER Elf
    The zephyr.elf that was flashed. Defaults to the newest one under build\ti*.
    It must be the SAME image that is running - an address from a different
    build points at the wrong RAM and the script reports "not an initialised
    RTT control block", which is the correct answer to the wrong question.

.PARAMETER Ccxml
    Target configuration. Defaults to the dongle's.

.PARAMETER Reset
    Reset the board and capture a fresh boot instead of reading what is already
    in the buffer. Costs a reboot; gains a banner that is definitely from this
    power-up.

.EXAMPLE
    .\scripts\ti\rtt_dump.ps1
    .\scripts\ti\rtt_dump.ps1 -Reset

.NOTES
    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding()]
param(
    [string]$Elf,
    [string]$Ccxml,
    [string]$Dss,
    [switch]$Reset
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')

if (-not $Ccxml) { $Ccxml = Join-Path $PSScriptRoot 'cc2652p_dongle.ccxml' }
if (-not (Test-Path $Ccxml)) { throw "No target configuration at $Ccxml." }

if (-not $Elf) {
    $candidate = Get-ChildItem -Path (Join-Path $repo 'build') -Filter zephyr.elf -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\ti[^\\]*\\' } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $candidate) {
        throw "No zephyr.elf found under build\ti*. Build first, or pass -Elf."
    }
    $Elf = $candidate.FullName
}
if (-not (Test-Path $Elf)) { throw "No ELF at $Elf." }

# nm comes from the Zephyr SDK, which scripts\env.ps1 puts on PATH. Not
# assumed: a bare `nm` here would silently be the wrong architecture's if one
# happened to exist.
$nm = (Get-Command arm-zephyr-eabi-nm -ErrorAction SilentlyContinue)
if (-not $nm) {
    throw @"
arm-zephyr-eabi-nm is not on PATH. Run

  . .\scripts\env.ps1 -NcsVersion v3.4.0

first - the RTT control block's address is read out of the ELF rather than
searched for in RAM, and that lookup needs the toolchain.
"@
}

$sym = & $nm.Source $Elf | Select-String -Pattern '\b_SEGGER_RTT$' | Select-Object -First 1
if (-not $sym) {
    throw @"
No _SEGGER_RTT symbol in $Elf.

That image was built without CONFIG_USE_SEGGER_RTT, so it has no RTT buffer to
read and this script has nothing to do. On a board with no usable UART that
also means the image has no console at all - see
apps\dongle_ti\boards\cc2652p_dongle.conf for the three symbols it needs.
"@
}
$addr = '0x' + ($sym.Line -split '\s+')[0]

# UniFlash's ccs_base is NOT at <install>\ccs_base - it is buried under
# deskdb\content\TICloudAgent\win, the same place dslite.bat reaches into. A
# full CCS install does put it at the shallow path, so both are searched rather
# than one being assumed.
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

# UniFlash carries its own JRE at <install>\jre\bin and dss.bat does not put it
# on PATH itself; without it dss.bat dies with a bare "java is not recognized",
# which reads like a broken install rather than a missing PATH entry.
$jre = @(Get-ChildItem 'C:\ti' -Filter 'java.exe' -Recurse -ErrorAction SilentlyContinue |
         Select-Object -First 1)
if ($jre.Count -gt 0) { $env:PATH = "$(Split-Path $jre[0].FullName -Parent);$env:PATH" }

$env:CCXML = $Ccxml
$env:RTT_CB = $addr
if ($Reset) { $env:RTT_RESET = '1' } else { Remove-Item Env:\RTT_RESET -ErrorAction SilentlyContinue }

Write-Host "ELF:    $Elf"
Write-Host "RTT CB: $addr"
Write-Host ''

& cmd /c "`"$Dss`" `"$(Join-Path $PSScriptRoot 'dss_rtt.js')`""
exit $LASTEXITCODE
