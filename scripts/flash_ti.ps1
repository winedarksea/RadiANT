# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Flash a CC13x2/CC26x2 board through the onboard XDS110, using OpenOCD.

.DESCRIPTION
    The counterpart of flash_sim_jlink.ps1 for the other vendor's probe -
    [[jlink-manual-commands-trap]] says never to hand-compose a probe script
    for this bench. Every OpenOCD invocation this project makes goes through
    here.

    MEASURED ON A LAUNCHXL-CC26X2R1: three of four routes in are dead ends.
    SWD fails with `Error connecting DP: cannot read IDR` - the CC26x2 debug
    port does not speak SWD at all. 4-pin JTAG through the XDS110's CMSIS-DAP
    personality, and through OpenOCD's native `xds110` driver (what
    board.cmake's runner uses), both scan as all zeroes with TDO stuck low at
    every clock from 100 kHz to 5.5 MHz. TI's own `dbgjtag -f @xds110 -S
    integrity` agrees: SC_ERR_PATH_BROKEN. Two independent implementations
    reaching the same verdict makes this a property of the board: this
    LaunchXL's XDS110-to-target link is 2-pin cJTAG, and OpenOCD has no cJTAG
    path for this adapter.

    So the route in is TI's own debug stack in cJTAG mode (UniFlash's
    `dslite` CLI). OpenOCD stays as the -UseOpenOcd escape hatch, because a
    board in this family with the JTAG isolation block populated differently
    may scan, and because it is the runner `west flash` would use.

    ONE PREREQUISITE EITHER WAY: the XDS110's debug interface,
    USB\VID_0451&PID_BEF3&MI_02, ships with NO driver bound - Device Manager
    shows status Error and a blank class. See the DRIVER section printed on
    failure.

    dbgjtag updated the probe's firmware unasked when run directly - the same
    class of side effect that bricked a J-Link on this bench once (see
    [[jlink-manual-commands-trap]]). Do not run TI's uscif utilities casually;
    use this script.

.PARAMETER HexPath
    Image to flash. Defaults to the newest zephyr.hex under build\ti*.

.PARAMETER DsLite
    Path to UniFlash's dslite.bat - the working path on this bench.

.PARAMETER Ccxml
    The target configuration dslite programs against. UniFlash generates one
    per board; see the message this script prints if it cannot find it.

.PARAMETER UseOpenOcd
    Use OpenOCD instead. An escape hatch, not an alternative: on this board
    it cannot scan the chain at all (see the measured dead ends above).

.PARAMETER OpenOcd
    Path to openocd.exe, for -UseOpenOcd. The xPack distribution is what this
    was tried against.

.PARAMETER Serial
    XDS110 serial number, for when more than one TI probe is attached.

.PARAMETER Speed
    JTAG clock in kHz. 5500 is what board/ti_cc26x2_launchpad.cfg asks for.

.PARAMETER CheckOnly
    Connect, report the target, and do not write anything.

.PARAMETER Xds110Reset
    Path to xds110reset.exe, found automatically under C:\ti otherwise. Run
    after every flash to bring the ANT backchannel back - see
    Reset-Backchannel below for why the order matters.

.PARAMETER NoBackchannelReset
    Skip that reset. A board flashed this way needs a physical USB replug.

    ON THE CC2652P DONGLE THIS DOES MORE THAN ITS NAME SUGGESTS: the probe's
    nRESET line is wired to the TARGET on this board, so skipping the reset
    also skips the dongle's own boot. dslite's -u resumes the core after
    programming but that is not a clean boot. Measured, twice:

      flash -NoBackchannelReset, then probe   -> FAILED, all three checks
      the same image after any DSS reset      -> PASS
      flash WITH the reset, then probe        -> PASS

    Use it only when the dongle's USB is unplugged and the ANT UART is not
    going to be spoken to before the next reset anyway.

.EXAMPLE
    .\scripts\flash_ti.ps1 -CheckOnly
    .\scripts\flash_ti.ps1
    .\scripts\flash_ti.ps1 -HexPath build\ti_null\radiant_dongle_ti\zephyr\zephyr.hex

.NOTES
    Unlike the Feather, this board is NOT a rationed flash - it has an onboard
    XDS110 and, failing that, the same ROM serial bootloader the Sonoff dongle
    uses. Reflash freely.

    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM, so an
    em-dash in this file is a parse error rather than a typo.
#>
[CmdletBinding()]
param(
    [string]$HexPath,
    [string]$DsLite,
    [string]$Ccxml,
    [switch]$UseOpenOcd,
    [string]$OpenOcd = 'C:\tools\xpack-openocd-0.12.0-7\bin\openocd.exe',
    [string]$Serial,
    [int]$Speed = 5500,
    [switch]$CheckOnly,
    [string]$Xds110Reset,
    [switch]$NoBackchannelReset
)

$ErrorActionPreference = 'Stop'

# ---- The backchannel, after a flash -----------------------------------------
#
# THE ONE THING THAT MAKES THIS BOARD USABLE UNATTENDED, so it lives in the
# script rather than in a habit.
#
# Every dslite session leaves COM14 - the XDS110's "Application/User UART",
# which carries the ANT wire protocol - completely silent (zero bytes, not
# garbage). It reproduces on EVERY image including the null backend and a
# bare blink app, with JTAG perfectly healthy, so it looks exactly like a
# firmware hang.
#
# The fix is a probe-side reset pulse, NOT a physical replug (replugging does
# work, but needs a human every time). ORDER IS LOAD-BEARING - measured three
# ways on the same board in one sitting:
#
#   flash, sleep, probe (no reset)          -> FAILED, 0 bytes
#   reset with NO port open, then open      -> FAILED, 0 bytes
#   flash, reset, then probe                -> PASS
#   open the port (DTR+RTS high) FIRST,
#     then reset, then read                 -> PASS, full boot banner
#
# What this function does is the third form; the fourth is what to reach for
# by hand if a port is already dead and reflashing is not wanted (see
# docs/testing.md).
function Reset-Backchannel {
    if ($NoBackchannelReset) {
        Write-Host 'Skipping the backchannel reset (-NoBackchannelReset).'
        Write-Host 'COM14 will very likely be silent until the USB cable is replugged.'
        return
    }

    $reset = $Xds110Reset
    if (-not $reset) {
        $roots = @(
            'C:\ti\ccs_base\common\uscif\xds110\xds110reset.exe'
        ) + (Get-ChildItem 'C:\ti' -Filter 'uniflash*' -Directory -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending |
             ForEach-Object {
                Join-Path $_.FullName 'deskdb\content\TICloudAgent\win\ccs_base\common\uscif\xds110\xds110reset.exe'
             })
        $reset = @($roots | Where-Object { Test-Path $_ })[0]
    }

    if (-not $reset -or -not (Test-Path $reset)) {
        Write-Warning @'
No xds110reset.exe found, so the backchannel was not reset.

It ships with UniFlash and with CCS, at
  <install>\ccs_base\common\uscif\xds110\xds110reset.exe
Pass -Xds110Reset <path> if it is somewhere else.

Without it COM14 will be silent after this flash and the only other way back
is physically replugging the USB cable.
'@
        return
    }

    # Deliberately NOT one of the uscif utilities this bench is warned about:
    # dbgjtag updated the probe firmware without being asked (see the header),
    # xds110reset only pulses nRESET. -a toggle -d 50 are its own defaults,
    # named here so a future reader can see there is a knob if 50 ms is ever
    # marginal.
    Write-Host "Resetting the backchannel: $reset"
    & $reset -a toggle -d 50 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "xds110reset exited with $LASTEXITCODE; COM14 may still be silent."
    }
    # The CDC needs a moment to come back before anything opens it. Three
    # seconds is what the working sequence used; it is not a tuned number.
    Start-Sleep -Seconds 3
}

$driverHelp = @'
DRIVER: the XDS110 debug interface has no driver bound.

OpenOCD reaches the probe through libusb, and libusb on Windows can only open
an interface that has WinUSB (or libusbK) bound to it. The XDS110 presents six
USB interfaces; Windows drives the two serial ports and the HID one by itself
and leaves MI_02, the debug interface, with no driver at all. Device Manager
shows it as "XDS110 (...) Embed with CMSIS-DAP" with status Error and a blank
class.

This is a one-time, elevated, machine-wide change, which is why this script
will not do it behind your back. Run ONE of these from an ADMINISTRATOR
PowerShell:

  # TI's own signed driver package - preferred, because it is signed by TI and
  # is the same driver Code Composer Studio and UniFlash install.
  pnputil /add-driver "<emupack>\ccs_base\emulation\windows\xds110_drivers\*.inf" /install /subdirs

  # Or install UniFlash / the XDS Emulation Software package and let its
  # installer do it:
  #   https://software-dl.ti.com/ccs/esd/documents/xdsdebugprobes/emu_xds_software_package_download.html

Afterwards, confirm with:

  Get-PnpDevice -PresentOnly | Where-Object InstanceId -like "*VID_0451&PID_BEF3&MI_02*"

It must report status OK. Then re-run this script.
'@

# ---- Which tool --------------------------------------------------------------
#
# dslite unless told otherwise, because on this board it is the only one that
# can reach the target at all. See the four measured dead ends in the header.
if (-not $UseOpenOcd) {
    if (-not $DsLite) {
        $candidates = Get-ChildItem 'C:\ti' -Filter 'uniflash*' -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'dslite.bat' } |
            Where-Object { Test-Path $_ }
        $DsLite = @($candidates)[0]
    }

    if (-not $DsLite -or -not (Test-Path $DsLite)) {
        throw @"
No dslite.bat found under C:\ti.

UniFlash is the flash tool for this board. OpenOCD cannot scan its JTAG chain
- that is measured, twice, with two independent implementations; see this
script's header - so this is not a preference.

  1. Download UniFlash for Windows:
       https://www.ti.com/tool/UNIFLASH
  2. Install it from an ADMINISTRATOR PowerShell:
       & '<installer>.exe' --mode unattended
  3. Re-run this script, or pass -DsLite <path to dslite.bat>.

To use OpenOCD anyway - on a different board in this family, or to reproduce
the dead end - pass -UseOpenOcd.
"@
    }

    Write-Host "dslite: $DsLite"
}
elseif (-not (Test-Path $OpenOcd)) {
    throw @"
No openocd.exe at $OpenOcd.

Install the xPack OpenOCD distribution and pass -OpenOcd <path>, or put it
there:

  https://github.com/xpack-dev-tools/openocd-xpack/releases

It is a zip; unpack it and point at bin\openocd.exe. It carries
board/ti_cc26x2_launchpad.cfg, which is the same script
boards/ti/cc26x2r1_launchxl/board.cmake names for `west flash`.

NOTE that on the LAUNCHXL-CC26X2R1 on this bench, OpenOCD cannot scan the
chain at all. See this script's header before spending time on it.
"@
}

if (-not $CheckOnly) {
    if (-not $HexPath) {
        # Not merged.hex: Partition Manager is a Nordic thing and is simply
        # absent on this target, so there is only ever the one image and
        # sysbuild writes no merged.hex at all.
        $buildRoot = Join-Path $PSScriptRoot '..\build'
        $candidate = Get-ChildItem -Path $buildRoot -Filter zephyr.hex -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\ti[^\\]*\\' } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1

        if (-not $candidate) {
            throw @"
No zephyr.hex found under $buildRoot for a TI build. Build first:

  . .\scripts\env.ps1 -NcsVersion v3.4.0
  .\scripts\fetch_hal_ti.ps1
  Push-Location C:\ncs\v3.4.0
  west -z C:\ncs\v3.4.0\zephyr build -s <repo>\apps\dongle_ti -d <repo>\build\ti_null ``
       -b cc26x2r1_launchxl -p always -- ``
       -DANT_RADIO=core -DRADIANT_BACKEND=null ``
       -DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti
  Pop-Location
"@
        }

        $HexPath = $candidate.FullName
        Write-Host "Using newest image: $HexPath"
    }

    if (-not (Test-Path $HexPath)) {
        throw "No image at $HexPath."
    }
    $HexPath = (Resolve-Path $HexPath).Path -replace '\\', '/'
}

if (-not $UseOpenOcd) {
    # ---- dslite ------------------------------------------------------------
    #
    # UniFlash's CLI. It programs against a target configuration (.ccxml) that
    # names the probe, the connection type and the device; the connection type
    # is where cJTAG is selected, which is the whole reason this path exists
    # rather than the OpenOCD one.
    if (-not $Ccxml) {
        $local = Join-Path $PSScriptRoot 'ti\cc26x2r1_launchxl.ccxml'
        if (Test-Path $local) {
            $Ccxml = $local
        }
    }

    if (-not $Ccxml -or -not (Test-Path $Ccxml)) {
        throw @"
No target configuration (.ccxml).

dslite programs against one, and it is where the connection type - cJTAG - is
selected. Generate it once with UniFlash's GUI (New Configuration ->
CC26X2R1 LaunchPad) or its CLI, save it as

  scripts\ti\cc26x2r1_launchxl.ccxml

and it will be found automatically from then on. Pass -Ccxml <path> for one
somewhere else.
"@
    }

    $dsArgs = @('--config', $Ccxml)

    if ($CheckOnly) {
        Write-Host "Connecting only - nothing will be written."
        # -l lists what the configuration resolves to and touches no flash.
        $dsArgs += '--list-cores'
    } else {
        $sizeKb = [math]::Round((Get-Item $HexPath).Length / 1KB, 1)
        Write-Host "Flashing $HexPath ($sizeKb KB) through dslite"
        # -v is verify, and it is not optional here: a CCFG page written wrong
        # is the one mistake on this part that is not cheaply recoverable, and
        # it is 88 bytes at the top of flash that every image writes.
        # -u runs the target afterwards. Not optional either, and for a less
        # obvious reason than verify: dslite leaves the core HALTED when it
        # finishes, so a board that flashed and verified perfectly is silent
        # on its UART, which looks exactly like an image that does not boot.
        $dsArgs += @('-v', '-f', $HexPath, '-u')
    }

    & cmd /c "`"$DsLite`" $($dsArgs -join ' ')"
    $code = $LASTEXITCODE

    if ($code -ne 0) {
        Write-Host ''
        Write-Warning "dslite exited with $code."
        Write-Host ''
        Write-Host $driverHelp
        throw "flash_ti: dslite failed ($code)."
    }

    if ($CheckOnly) {
        Write-Host ''
        Write-Host 'Probe and target are reachable.'
        return
    }

    Reset-Backchannel

    Write-Host ''
    Write-Host 'Flashed.'
    Write-Host '  ANT serial: the XDS110 backchannel COM port, AT 57600 - not 115200.'
    Write-Host '              python tools\ant_probe.py --port COMxx --baud 57600'
    Write-Host '  log:        uart1 on BoosterPack pins DIO22 (TX) / DIO24 (RX), 115200,'
    Write-Host '              which needs a 3V3 USB-serial adapter clipped on. Nothing'
    Write-Host '              is wrong if it is silent; nothing is attached.'
    return
}

# The adapter, then the target. Deliberately NOT `-f board/ti_cc26x2_launchpad.cfg`
# even though that file says exactly this: the serial-number selection has to be
# injected between the interface and the transport, and splitting it here is
# better than sourcing a file and then overriding half of it.
$args = @(
    '-f', 'interface/xds110.cfg'
)
if ($Serial) {
    $args += @('-c', "adapter serial $Serial")
}
$args += @(
    '-c', "adapter speed $Speed",
    '-c', 'transport select jtag',
    '-f', 'target/ti_cc26x2.cfg'
)

if ($CheckOnly) {
    Write-Host "Connecting only - nothing will be written."
    $args += @('-c', 'init; targets; mdw 0x50001318 1; exit')
} else {
    $sizeKb = [math]::Round((Get-Item $HexPath).Length / 1KB, 1)
    Write-Host "Flashing $HexPath ($sizeKb KB) at $Speed kHz"
    # `program ... verify reset exit` is OpenOCD's own one-shot form: halt,
    # erase what the image covers, write, read back, reset into it, quit.
    # Verify is not optional here - a CCFG page written wrong is the one
    # mistake on this part that is not cheaply recoverable, and it is 88 bytes
    # at the top of flash that every image writes.
    $args += @('-c', "program $HexPath verify reset exit")
}

& $OpenOcd @args
$code = $LASTEXITCODE

if ($code -ne 0) {
    Write-Host ''
    Write-Warning "OpenOCD exited with $code."
    Write-Host ''
    Write-Host $driverHelp
    throw "flash_ti: OpenOCD failed ($code)."
}

if ($CheckOnly) {
    Write-Host ''
    Write-Host 'Probe and target are reachable.'
    return
}

Reset-Backchannel

Write-Host ''
Write-Host 'Flashed and reset.'
Write-Host '  ANT serial: the XDS110 backchannel COM port, AT 57600 - not 115200.'
Write-Host '              python tools\ant_probe.py --port COMxx --baud 57600'
Write-Host '  log:        uart1 on BoosterPack pins DIO22 (TX) / DIO24 (RX), 115200,'
Write-Host '              which needs a 3V3 USB-serial adapter clipped on. Nothing'
Write-Host '              is wrong if it is silent; nothing is attached.'
