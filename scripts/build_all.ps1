# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Build every target and collect the release artifacts into dist\.

.DESCRIPTION
    Mirrors the matrix in .github\workflows\build.yml, including the checks it
    makes after each build, so a local dist\ matches what CI would publish
    rather than approximately resembling it.

    Those checks exist because the failures are silent. Partition Manager
    decides where the application links and does not complain when that
    disagrees with CONFIG_FLASH_LOAD_OFFSET - the image just installs cleanly
    and boots into nothing. The transport is defaulted from devicetree, so a
    board whose USB node moved would quietly compile a different one and still
    succeed. And the radio backend is a CMake cache variable, decided before
    Kconfig runs, so a typo in -DANT_RADIO would fall back to a default and
    ship the wrong backend green.

    THREE MATRICES, NOT ONE. $targets is the dongle (apps/dongle,
    apps/dongle_ti): a release artifact with a bootloader offset, a transport
    and a package format. $nodeTargets is apps/hrm_ble, the heart-rate node -
    no bootloader, no transport symbol and no artifact. $treadmillTargets is
    apps/treadmill, the fitness-equipment node, which shares the node shape and
    NONE of the node's assertions: it has no HRM_SENSOR choice, it has a
    TREADMILL_SOURCE choice instead, and its BLE row asserts a different set of
    symbols.

    A THIRD LIST RATHER THAN OPTIONAL FIELDS ON $nodeTargets, and it is the
    same argument the comment above $nodeTargets already makes about folding
    into $targets: `if ($t.sensor)` silently skips an assertion a row meant to
    have, and every assertion in this file exists because the failure it
    catches is silent. A separate list with its own smaller set of assertions
    cannot skip anything by accident.

    The node and treadmill rows build only under -Backend core, because neither
    application has an ANT_RADIO axis at all: both are built out of radiant and
    radiant/src/profiles/ with no sdk-ant path, which is the demonstration they
    exist to make. A default (-Backend sdk_ant) run therefore does not build
    them; use -Backend core.

    EVERY ROW NOW COMPLETES UNDER -Backend core -RadiantBackend nrf. Two of them
    never could until the overlays below were added, and the history is worth
    keeping because the failure was so misleading:

      promicro / promicro_synth   apps\dongle\boards\promicro_nrf52840_
                                  nrf52840_uf2.overlay set no
                                  `chosen { radiant,radio-timer }`. It now
                                  names timer4, as the Feather overlay does.
      lm20                        nrf54lm20dk had a .conf and no .overlay at
                                  all. It now has one naming timer20, as the
                                  nRF54L15 DK overlay does.

    Both died in radiant\src\radiant_radio_nrf.c at its `#error
    "radiant_radio_nrf needs a timer..."` guard. READ THE FIRST ERROR, NOT THE
    CASCADE, if this ever recurs on a new board: what follows is a wall of
    DT_CHOSEN_radiant_radio_timer_..._undeclared plus unrelated log_msg.h /
    cbprintf_internal.h errors that read like a broken toolchain or a logging
    fault, and are neither.

    Why it went unnoticed for so long: -RadiantBackend null built every row, and
    null wants no TIMER. That default is now gone from every application (see
    cmake\radiant_backend.cmake), so a board missing the node fails loudly on the
    first build instead of silently producing an image with no radio.

    The TI rows need -NcsVersion v3.4.0: hal_ti lives at
    C:\ncs\v3.4.0\modules\hal\ti and is ABSENT under v3.2.4, where the row is
    skipped with a message rather than failing.

    ASCII only, deliberately: Windows PowerShell 5.1 reads .ps1 files as ANSI
    unless they carry a BOM, so non-ASCII characters here become parse errors.

.PARAMETER NcsVersion
    NCS version to build against. Must be the one sdk-ant pairs with. v3.2.4
    is a hard constraint of sdk-ant v2.1.0, not a stale pin: v2.1.0 keys
    ANT_LIB_DIR off CONFIG_SOC_SERIES_NRF52X, and the Zephyr in v3.4.0 renamed
    that symbol to CONFIG_SOC_SERIES_NRF52, so libant.a is never located and
    ninja reports it missing with no rule to make it.

.PARAMETER Backend
    Which implementation of src/ant_radio.h to build against: sdk_ant (the
    proven path and the only one release artifacts come from), core (the
    clean-room radiant stack) or stub. Passed through as -DANT_RADIO and
    asserted afterwards against .config.

.PARAMETER SdkAntDir
    Path to the sdk-ant checkout, for -Backend sdk_ant. Defaults to
    $env:SDK_ANT_DIR, then to a sibling checkout beside this repository - the
    same order CMakeLists.txt resolves in. It is passed explicitly on the
    command line rather than left to that resolution so the value is visible
    in the build log and in a failure message, instead of being whatever the
    machine happened to have configured.

.PARAMETER RadiantBackend
    Which radiant radio HAL to compile, for -Backend core: nrf (the real
    radio) or null (an inert stub that transmits and receives nothing).

    It defaults to nrf here, and CMakeLists.txt now defaults to nrf too, so
    the two finally agree. They used to disagree - CMake defaulted to null on
    a compile-check argument - and that disagreement was worth removing: a
    null-radio image enumerates, answers every host command with OK and finds
    no sensors, which is indistinguishable from a dead antenna and cost a
    Feather flash and a whole Zwift session to identify once already, because
    none of the three checks below looked at it. Now one does, and the default
    cannot produce that image in the first place.

.PARAMETER HalTiDir
    Where the hal_ti Zephyr module is, for the CC26x2 target. Defaults to
    where scripts\fetch_hal_ti.ps1 puts it. NCS's west manifest filters this
    module out of its import, so it is never present unless that script has
    been run; the TI row is skipped with a message rather than failing the
    whole matrix when it is missing, because every other target builds fine
    without it.

.PARAMETER Only
    Build just the targets whose artifact name matches this wildcard. The node
    rows (apps/hrm_ble) have no artifact - nothing there is a release image - so
    they match on their build-directory name instead: -Only "node_*".

.PARAMETER SkipDfu
    Do not package the dongle DFU zip (which needs nrfutil).

.EXAMPLE
    .\scripts\build_all.ps1
    .\scripts\build_all.ps1 -Only "*promicro*"
    .\scripts\build_all.ps1 -Backend core
#>
[CmdletBinding()]
param(
    [string]$NcsVersion = 'v3.2.4',
    [ValidateSet('sdk_ant', 'core', 'stub')]
    [string]$Backend = 'sdk_ant',
    [ValidateSet('null', 'nrf')]
    [string]$RadiantBackend = 'nrf',
    [string]$SdkAntDir = '',
    [string]$HalTiDir = '',
    [string]$Only = '*',
    [switch]$SkipDfu
)

$ErrorActionPreference = 'Stop'

# WEST MUST NOT BE CALLED DIRECTLY FROM A SCRIPT WITH 'Stop' IN FORCE.
#
# west writes its ordinary progress to stderr - "Loading Zephyr module(s)
# (Zephyr base): sysbuild_default" and so on - and Windows PowerShell 5.1 wraps
# a native command's stderr in ErrorRecords. With $ErrorActionPreference =
# 'Stop' the FIRST progress line therefore aborts the script, and it aborts it
# with a NativeCommandError that reads exactly like a build failure while the
# build was in fact fine. `> $log` does not help: it redirects stdout, and
# stderr is the stream causing it.
#
# So the preference is dropped to 'Continue' across the west call and nowhere
# else, and $LASTEXITCODE is the only verdict tested. scripts/build_p4.ps1 does
# the same thing for the same reason and its comment says so at length; this
# wrapper exists so the two loops below cannot get it right in one place and
# wrong in the other, which is exactly what had happened - the node loop threw
# on its first row while the target loop above it did not.
function Invoke-West([string[]]$BaseArgs, [string[]]$ExtraArgs, [string]$LogPath) {
    $all = @($BaseArgs)
    if ($ExtraArgs) { $all += $ExtraArgs }

    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & west @all > $LogPath
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
}

$repo = Split-Path $PSScriptRoot -Parent
$zephyr = "C:\ncs\$NcsVersion\zephyr"
$dist = Join-Path $repo 'dist'

# CONFIG_ANT_DONGLE_RADIO_SDK_ANT / _CORE / _STUB. The Kconfig choice arm is
# the backend name upper-cased, which is not a coincidence - CMakeLists.txt
# generates the symbol from ANT_RADIO the same way.
$backendSymbol = "CONFIG_ANT_DONGLE_RADIO_$($Backend.ToUpper())=y"

# The same trick one level down - for radiant's own HAL choice - is now
# computed per target rather than once here, because a row may pin its own
# backend regardless of -RadiantBackend. See the cc26x2r1_launchxl row. Only
# the core backend has such a choice at all; sdk_ant and stub never look at
# RADIANT_BACKEND.

# Same fields as the CI matrix. 'offset' is where the application must link and
# 'transport' is which of the three src/ transports must end up compiled; both
# are asserted after the build.
$targets = @(
    @{ dir='release';        board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='radiant_dongle.uf2';                   pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf=$null;         release=$true  }
    @{ dir='dongle';         board='nrf52840dongle/nrf52840';                artifact='radiant_dongle_nrf52840dongle.zip';    pkg='dfu'; offset=0x1000;  transport='USB_LEGACY'; conf=$null;         release=$true  }
    @{ dir='promicro';       board='promicro_nrf52840/nrf52840/uf2';         artifact='radiant_dongle_promicro.uf2';          pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf=$null;         release=$true  }
    @{ dir='promicro_synth'; board='promicro_nrf52840/nrf52840/uf2';         artifact='radiant_dongle_promicro_synth.uf2';    pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf='synth.conf';  release=$true  }
    @{ dir='feather_next';   board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='radiant_dongle_feather_usbd.uf2';      pkg='uf2'; offset=0x26000; transport='USB_NEXT';   conf='next.conf';   release=$false }
    # The activity-rate NeoPixel. The only dongle row carrying BOTH a conf and
    # an overlay, and it needs both: the overlay declares the LED node and the
    # `rgb-led0` alias the module looks up, and the conf turns the module on.
    # Either one alone fails loudly by design - see apps/dongle/rgb.conf.
    @{ dir='feather_rgb';    board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='radiant_dongle_feather_rgb.uf2';       pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf='rgb.conf';    release=$false; overlay='rgb_feather.overlay' }
    @{ dir='l15';            board='nrf54l15dk/nrf54l15/cpuapp';             artifact='radiant_dongle_nrf54l15dk.hex';        pkg='hex'; offset=0x0;     transport='UART';       conf=$null;         release=$false }
    @{ dir='lm20';           board='nrf54lm20dk/nrf54lm20a/cpuapp';          artifact='radiant_dongle_nrf54lm20dk.hex';       pkg='hex'; offset=0x0;     transport='USB_NEXT';   conf=$null;         release=$false }
)

# The default-off configuration fragments, which only build under a backend
# that can reach them.
#
# encryption.conf's own comment says it exists "so a default-off path does not
# quietly stop compiling", and it had never appeared in this list or in either
# CI matrix - so the thing it was written to prevent was never prevented for
# it. security.conf gets its entry in the same change that creates it.
#
# CONFIG_RADIANT_SEC depends on CONFIG_RADIANT, so a security build under
# -Backend sdk_ant would compile nothing and pass, which is worse than not
# building it: it would look like coverage. Hence the gate on $Backend rather
# than an unconditional row.
if ($Backend -eq 'core') {
    $targets += @{ dir='feather_security'; board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='radiant_dongle_feather_security.uf2'; pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf='security.conf'; release=$false }
}
if ($Backend -eq 'sdk_ant') {
    $targets += @{ dir='feather_encryption'; board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='radiant_dongle_feather_encryption.uf2'; pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf='encryption.conf'; release=$false }
}

# ── The second vendor ───────────────────────────────────────────────────────
#
# A LAUNCHXL-CC26X2R1, and the row is here so that the four assertions below
# cover a non-Nordic target too. Three things about it differ from every other
# row and each one is a thing the assertions are for:
#
#   `radiant` overrides -RadiantBackend. nrf is not merely wrong on this part,
#   it is unbuildable - cc26xx is what belongs here now that the port exists
#   (P2 onward; see docs/decisions/0014 and 0015), and assertion 4 then checks
#   that cc26xx is what Kconfig landed on rather than what CMake was asked
#   for. Only the base row still needs this override; the coex arms below
#   inherit it from the base row's own $t.radiant.
#
#   `baud` is asserted at all. Every other target runs its ANT stream at
#   115200 and tools/ant_probe.py defaults to it; this one runs at 57600
#   because the CC2652P + CP2102N module it stands in for impersonates a
#   Dynastream ANT2USB, which is a 57600 device. A mismatch is invisible
#   except as "no response to reset", which is what a hung image looks like.
#
#   `modules` names hal_ti. NCS's west manifest filters that module out of its
#   import - see scripts/fetch_hal_ti.ps1 - so it is absent until someone
#   fetches it, and the row is skipped rather than failing the matrix.
#
# Only under -Backend core: sdk_ant is Nordic-only silicon by construction and
# a stub build here would assert nothing about the port.
#
# FOUR ROWS, NOT ONE, and they are an A/B/C/D that only means anything read in
# order - see docs/decisions/0015-cc26xx-coexistence-design.md and
# docs/testing.md for the table:
#
#   ti_launchxl        arm 1  the floor
#   ti_launchxl_patch  arm 2  the CPE multi-protocol patch's PHY cost alone
#   ti_launchxl_gate   arm 3  the scheduler hooks, patch held constant
#   ti_launchxl_coex   arm 4  a real second RF client (the forked 802.15.4
#                             driver), arbiter and patch held constant
#
# Arm 4 is the only row in this file that needs a devicetree overlay as well as
# a conf fragment: Zephyr's own cc26x2r1_launchxl.dts disables the 802.15.4
# node. `overlay` exists for it and is $null everywhere else.
#
# ARM 4 BUILDS BUT HAS NEVER RUN AGAINST AN 802.15.4 PEER - this bench has
# none. It belongs in the matrix because it is a build that must not rot, not
# because a green matrix says coexistence works. Read ti_coex.conf's header
# before recording any number off it.
if ($Backend -eq 'core') {
    if (-not $HalTiDir) { $HalTiDir = "C:\ncs\$NcsVersion\modules\hal\ti" }
    if (Test-Path (Join-Path $HalTiDir 'zephyr\module.yml')) {
        $halTiModule = ($HalTiDir -replace '\\', '/')
        # app: apps/dongle_ti, not apps/dongle - the only rows in this matrix
        # that build from a different application, since this is the only
        # board with no USB device peripheral to make an ANT_DONGLE-shaped
        # image on and the only silicon radiant's cc26xx backend targets. See
        # apps/dongle_ti's own README for the "unverified pending hardware"
        # scope of the coexistence arm (the fourth row).
        $dongleTiApp = Join-Path $repo 'apps\dongle_ti'
        $targets += @{ dir='ti_launchxl';       board='cc26x2r1_launchxl'; artifact='radiant_dongle_cc26x2r1_launchxl.hex';       pkg='hex'; offset=0x0; transport='UART'; conf=$null;           release=$false; radiant='cc26xx'; baud=57600; modules=$halTiModule; app=$dongleTiApp }
        $targets += @{ dir='ti_launchxl_patch'; board='cc26x2r1_launchxl'; artifact='radiant_dongle_cc26x2r1_launchxl_patch.hex'; pkg='hex'; offset=0x0; transport='UART'; conf='ti_patch.conf'; release=$false; radiant='cc26xx'; baud=57600; modules=$halTiModule; app=$dongleTiApp }
        $targets += @{ dir='ti_launchxl_gate';  board='cc26x2r1_launchxl'; artifact='radiant_dongle_cc26x2r1_launchxl_gate.hex';  pkg='hex'; offset=0x0; transport='UART'; conf='ti_gate.conf';  release=$false; radiant='cc26xx'; baud=57600; modules=$halTiModule; app=$dongleTiApp }
        $targets += @{ dir='ti_launchxl_coex';  board='cc26x2r1_launchxl'; artifact='radiant_dongle_cc26x2r1_launchxl_coex.hex';  pkg='hex'; offset=0x0; transport='UART'; conf='ti_coex.conf';  release=$false; radiant='cc26xx'; baud=57600; modules=$halTiModule; overlay='ti_coex.overlay'; app=$dongleTiApp }
    } else {
        Write-Host "cc26x2r1_launchxl: skipped, no hal_ti module at $HalTiDir" -ForegroundColor DarkYellow
        Write-Host "  Run scripts\fetch_hal_ti.ps1 (NCS's west manifest will never fetch it)."
    }
}

# ── The node ────────────────────────────────────────────────────────────────
#
# apps/hrm_ble, the heart-rate node and manufacturer reference design. It is not
# a dongle and it never was; until this list existed it appeared in neither this
# script nor .github/workflows/build.yml, so NOTHING in the repository compiled
# it and a rename could break it invisibly.
#
# A SEPARATE LIST RATHER THAN MORE $targets ROWS, and the reason is that three
# of $targets' fields have no meaning here and the five assertions below are
# built out of them:
#
#   offset     there is no bootloader and no Partition Manager on this
#              application, so `app` links wherever the board's dts says. A row
#              claiming an offset would be asserting a number nothing decides.
#   transport  CONFIG_ANT_DONGLE_TRANSPORT_* does not EXIST in a node's Kconfig
#              tree. Assertion 2 reads `-ne $t.transport` against an empty
#              string, so a node row folded into $targets would fail on a symbol
#              that is correctly absent.
#   pkg        nothing here is a release artifact. Only -Backend sdk_ant writes
#              dist\, and this application has no sdk_ant build at all - it is
#              built out of radiant and radiant/src/profiles/, which is the
#              entire point of it.
#
# The alternative considered was making those fields optional and skipping the
# assertions when absent. Rejected: `if ($t.offset)` silently skips assertion 1
# for a row that meant to have one, and the five assertions in this file exist
# precisely because the failures they catch are silent. A second list with its
# own, smaller set of assertions cannot skip anything by accident.
#
# WHAT IS ASSERTED HERE INSTEAD, and each one is a failure that has actually
# happened in this tree:
#
#   the HAL backend    RADIANT_BACKEND is a CMake cache variable resolved before
#                      Kconfig runs, so an unmet `depends on` falls back to the
#                      null backend and the image boots, logs "transmitting" and
#                      puts nothing on the air.
#   the sensor source  exactly one arm of the HRM_SENSOR choice, asserted by
#                      name. This is what makes "I forgot to unhook the
#                      simulator" a red build rather than a shipped strap
#                      reporting 72 bpm from a simulator on a real chest.
#   the BLE symbols    on the BLE row only: HRM_BLE_HRS plus the two symbols it
#                      `select`s/`default`s rather than sets by hand. A Kconfig
#                      `default` written after the symbol's own definition is
#                      silently dead, and only a .config read-back tells the
#                      difference.
#
# Only under -Backend core: this application has no ANT_RADIO axis at all, so
# building it under -Backend sdk_ant or stub would compile the same image and
# assert nothing extra.
$nodeTargets = @(
    @{ dir='node_l15';        board='nrf54l15dk/nrf54l15/cpuapp';    sensor='SIMULATED'; conf=$null }
    # The other half of radiant/Kconfig's `SOC_COMPATIBLE_NRF52X ||
    # SOC_COMPATIBLE_NRF54LX`. Build-verified only - there is no nRF52 bench slot
    # for this application, and the part's on-air behaviour is predicted from the
    # nRF54L15 measurements rather than confirmed.
    @{ dir='node_52840dk';    board='nrf52840dk/nrf52840';           sensor='SIMULATED'; conf=$null }
    @{ dir='node_lm20';       board='nrf54lm20dk/nrf54lm20a/cpuapp'; sensor='SIMULATED'; conf=$null }
    @{ dir='node_ble';        board='nrf54l15dk/nrf54l15/cpuapp';    sensor='SIMULATED'; conf='ble.conf';                     ble=$true }
    @{ dir='node_ble_nosup';  board='nrf54l15dk/nrf54l15/cpuapp';    sensor='SIMULATED'; conf='ble.conf;ble_nosuppress.conf'; ble=$true }
    # The seam's second implementation, and the arm with no implementation at
    # all. A seam with one implementation is indistinguishable from a direct
    # call with extra prototypes; a seam whose empty case does not link is a
    # seam an integrator's first build fails on for the wrong reason.
    @{ dir='node_example';    board='nrf54l15dk/nrf54l15/cpuapp';    sensor='EXAMPLE';   conf='sensor_example.conf' }
    @{ dir='node_custom';     board='nrf54l15dk/nrf54l15/cpuapp';    sensor='CUSTOM';    conf='sensor_custom.conf' }
)

# ── The treadmill ───────────────────────────────────────────────────────────
#
# apps/treadmill, the fitness-equipment node and this tree's second
# manufacturer reference design. A THIRD list rather than more $nodeTargets
# rows, for the same reason $nodeTargets is not part of $targets: the fields
# are not the same fields.
#
#   sensor  there is no HRM_SENSOR choice here. The equivalent is
#           TREADMILL_SOURCE, whose arms are SIMULATED and CUSTOM and not
#           SIMULATED/EXAMPLE/CUSTOM, so assertion 2 of the node loop would
#           either pass vacuously or fail on a symbol that is correctly absent.
#   ble     the symbols to assert are different ones. TREADMILL_BLE `select`s
#           BT_RSCS and BT_SMP as well as the MPSL gate, and the SMP half is
#           the one that is genuinely new in this tree - apps/hrm_ble has no
#           pairing at all.
#
# WHAT IS ASSERTED, and each is a failure that has actually happened here:
#
#   the HAL backend  as on the node rows. RADIANT_BACKEND is a CMake cache
#                    variable resolved before Kconfig runs, so an unmet
#                    `depends on` falls back to the null backend and the image
#                    boots, logs "transmitting" and puts nothing on the air.
#                    This has bitten twice.
#   the source arm   exactly one arm of the TREADMILL_SOURCE choice, by name.
#                    What makes "I forgot to unhook the simulator" a red build
#                    rather than a shipped treadmill reporting a synthetic
#                    runner while somebody stands on it.
#   the second ANT+  CONFIG_TREADMILL_ANT_SDM, asserted BOTH ways. The fec_only
#   master           row exists so the coexistence fallback cannot rot, and a
#                    fallback nobody compiles is not a fallback.
#   the BLE symbols  on the BLE rows: TREADMILL_BLE plus the four symbols it
#                    `select`s or `default`s rather than sets by hand. A
#                    Kconfig `default` written after the symbol's own
#                    definition is silently dead, and only a .config read-back
#                    tells the difference.
$treadmillTargets = @(
    @{ dir='tread_l15';      board='nrf54l15dk/nrf54l15/cpuapp'; source='SIMULATED'; conf=$null;                  sdm=$true }
    # The other half of radiant/Kconfig's `SOC_COMPATIBLE_NRF52X ||
    # SOC_COMPATIBLE_NRF54LX`. ANT+ only: the BLE arm on nRF52 would be the
    # first time SDC, MPSL, the gate, two hand-written GATT services and SMP
    # were compiled together on that family, and there is no nRF52 bench slot
    # for this application. See apps/treadmill/boards/nrf52840dk_nrf52840.overlay.
    @{ dir='tread_52840dk';  board='nrf52840dk/nrf52840';        source='SIMULATED'; conf=$null;                  sdm=$true }
    # ONE ANT+ MASTER INSTEAD OF TWO - the coexistence fallback, compiled so it
    # cannot rot. Two masters at 8192 and 8134 counts beat against each other
    # with a period of about 32 s, and nothing in this project has measured
    # that mix beside BLE.
    @{ dir='tread_fec_only'; board='nrf54l15dk/nrf54l15/cpuapp'; source='SIMULATED'; conf='fec_only.conf';        sdm=$false }
    @{ dir='tread_ble';      board='nrf54l15dk/nrf54l15/cpuapp'; source='SIMULATED'; conf='ble.conf';             sdm=$true; ble=$true }
    # The heaviest configuration this application has: two masters, one slave
    # and BLE with SMP and a bonding store. It is the row that decides whether
    # the RAM budget holds, which the Matter spike already established matters
    # more than flash on this part.
    @{ dir='tread_ble_hr';   board='nrf54l15dk/nrf54l15/cpuapp'; source='SIMULATED'; conf='ble.conf;hr_rx.conf';  sdm=$true; ble=$true; hr=$true }
    # The arm with no source in it. It must LINK - an integrator's first build
    # failing for a reason unrelated to their driver is an expensive way to
    # start - and then report a stopped belt rather than falling back to the
    # simulator.
    @{ dir='tread_custom';   board='nrf54l15dk/nrf54l15/cpuapp'; source='CUSTOM';    conf='source_custom.conf';   sdm=$true }
)

if (-not (Get-Command west -ErrorAction SilentlyContinue)) {
    throw "west is not on PATH. Dot-source the environment first:`n" +
          "  . .\scripts\env.ps1 -NcsVersion $NcsVersion"
}

# Resolve sdk-ant here rather than letting CMake do it silently. Only the
# sdk_ant backend needs it at all; core and stub never look at it, which is
# the entire point of the seam and is what the independence build below
# proves rather than asserts.
$antModuleDir = $null
if ($Backend -eq 'sdk_ant') {
    if ($SdkAntDir) {
        $antModuleDir = $SdkAntDir
    } elseif ($env:SDK_ANT_DIR) {
        $antModuleDir = $env:SDK_ANT_DIR
    } else {
        $antModuleDir = Join-Path (Split-Path $repo -Parent) 'sdk-ant'
    }
    $antModuleDir = $antModuleDir -replace '\\', '/'
    if (-not (Test-Path (Join-Path $antModuleDir 'zephyr/module.yml'))) {
        throw "no sdk-ant module at '$antModuleDir' (no zephyr/module.yml there).`n" +
              "Set `$env:SDK_ANT_DIR, pass -SdkAntDir <path>, or build a backend`n" +
              "that does not need it: -Backend core, -Backend stub."
    }
    Write-Host "sdk-ant: $antModuleDir" -ForegroundColor DarkGray
}

New-Item -ItemType Directory -Force -Path $dist | Out-Null

# `west build` is an extension command discovered through the workspace
# manifest, so west must run with its cwd inside the workspace even though
# -s/-d/-z all point elsewhere.
#
# Every row builds from apps/dongle except the cc26x2r1_launchxl ones, which
# name their own $t.app (apps/dongle_ti) - see that row's own comment. The
# sysbuild image folder under each row's own -d is therefore the built app's
# directory basename ('dongle' or 'dongle_ti'), not a fixed name - sysbuild
# takes the default image name from APP_DIR's own basename, not from
# project().
Push-Location "C:\ncs\$NcsVersion"
try {
    $built = @()
    $dongleApp = Join-Path $repo 'apps\dongle'

    foreach ($t in $targets) {
        if ($t.artifact -notlike $Only) { continue }

        $out = Join-Path $repo "build\$($t.dir)"
        $rowApp = if ($t.app) { $t.app } else { $dongleApp }
        $imageName = Split-Path $rowApp -Leaf
        Write-Host "`n=== $($t.artifact)  [$($t.board)]  [$Backend]" -ForegroundColor Cyan

        # A row may pin its own HAL backend. Only the TI one does, and it does
        # because -RadiantBackend's default (nrf) names a peripheral that part
        # does not have - see the row's own note.
        $rowRadiant = if ($t.radiant) { $t.radiant } else { $RadiantBackend }

        $extra = @('--', "-DANT_RADIO=$Backend")
        if ($Backend -eq 'core') { $extra += "-DRADIANT_BACKEND=$rowRadiant" }
        if ($antModuleDir) { $extra += "-DANT_MODULE_DIR=$antModuleDir" }
        if ($t.modules) { $extra += "-DEXTRA_ZEPHYR_MODULES=$($t.modules)" }
        if ($t.conf) { $extra += "-DEXTRA_CONF_FILE=$($t.conf)" }
        # Only ti_launchxl_coex sets this today. Not folded into `conf`: a
        # fragment and an overlay are read by different tools at different
        # stages, and a row that silently dropped its overlay would still
        # configure, still build, and still be the wrong image (see
        # ti_coex.overlay's header for what that failure looks like).
        if ($t.overlay) { $extra += "-DEXTRA_DTC_OVERLAY_FILE=$($t.overlay)" }

        # Do not redirect stderr: PowerShell 5.1 wraps a native command's
        # stderr in ErrorRecords and reports failure even on exit code 0.
        $log = Join-Path $env:TEMP "build_$($t.dir).log"
        $rc = Invoke-West @('-z', $zephyr, 'build', '-s', $rowApp, '-d', $out,
                            '-b', $t.board, '-p', 'always') $extra $log
        if ($rc -ne 0) {
            Get-Content $log -Tail 30
            throw "build failed for $($t.artifact); full log at $log"
        }

        $cfgPath = Join-Path $out "$imageName\zephyr\.config"
        $cfg = Get-Content $cfgPath

        # 1. Link address. PM's `app` and the load offset must agree.
        $offLine = ($cfg | Where-Object { $_ -match '^CONFIG_FLASH_LOAD_OFFSET=' }) -replace '.*=', ''
        $offset = [Convert]::ToInt64($offLine, ($(if ($offLine -match '^0x') { 16 } else { 10 })))
        if ($offset -ne $t.offset) {
            throw ("$($t.artifact): links at 0x{0:x} but 0x{1:x} was expected" -f $offset, $t.offset)
        }

        $pm = Join-Path $out 'partitions.yml'
        if (Test-Path $pm) {
            $inApp = $false
            foreach ($line in Get-Content $pm) {
                if ($line -match '^app:') { $inApp = $true; continue }
                if ($inApp -and $line -match '^\s+address:\s*(0x[0-9a-fA-F]+|\d+)') {
                    $pmAddr = [Convert]::ToInt64($Matches[1], ($(if ($Matches[1] -match '^0x') { 16 } else { 10 })))
                    if ($pmAddr -ne $t.offset) {
                        throw ("$($t.artifact): Partition Manager put app at 0x{0:x}, load offset is 0x{1:x}" -f $pmAddr, $t.offset)
                    }
                    break
                }
                if ($inApp -and $line -notmatch '^\s') { break }
            }
        }

        # 2. Transport. Defaulted from devicetree, so assert what got compiled.
        $got = ($cfg | Where-Object { $_ -match '^CONFIG_ANT_DONGLE_TRANSPORT_\w+=y' }) -replace 'CONFIG_ANT_DONGLE_TRANSPORT_', '' -replace '=y', ''
        if ($got -ne $t.transport) {
            throw "$($t.artifact): compiled the $got transport, expected $($t.transport)"
        }

        # 3. Radio backend. ANT_RADIO is a CMake cache variable, chosen before
        # Kconfig exists, so nothing in .config would record which backend was
        # compiled unless CMake wrote the symbol on purpose - which it does,
        # precisely so this line can exist. Without it a -DANT_RADIO typo
        # falls back to a default and ships the wrong backend green.
        if (-not ($cfg | Where-Object { $_ -eq $backendSymbol })) {
            $gotRadio = ($cfg | Where-Object { $_ -match '^CONFIG_ANT_DONGLE_RADIO_\w+=y' })
            throw "$($t.artifact): expected $backendSymbol in .config, found '$gotRadio'"
        }

        # 4. radiant's radio HAL. This check long predates the default being
        # fixed, and it stays: RADIANT_BACKEND is still a value that can be
        # passed explicitly, still a value Kconfig can decline to honour when a
        # `depends on` is unmet, and the consequence is still an image that
        # transmits and receives nothing while checks 1 to 3 all pass - the
        # link address, the transport and the ANT_DONGLE_RADIO choice are all
        # still exactly right on a null-radio build. Such an image enumerates
        # and answers every host command with OK; the only symptom is that no
        # sensor is ever found, which reads as a hardware fault. That is why
        # this check is worth its lines even now that no application defaults
        # to null.
        if ($Backend -eq 'core') {
            $rowRadiantSymbol = "CONFIG_RADIANT_BACKEND_$($rowRadiant.ToUpper())=y"
            if (-not ($cfg | Where-Object { $_ -eq $rowRadiantSymbol })) {
                $gotHal = ($cfg | Where-Object { $_ -match '^CONFIG_RADIANT_BACKEND_\w+=y' })
                throw "$($t.artifact): expected $rowRadiantSymbol in .config, found '$gotHal'"
            }
        }

        # 5. The ANT UART's baud, where a row states one.
        #
        # New with the TI target and only asserted there, because it is the
        # only board that does not run at 115200. The rate lives in two places
        # by design - the devicetree configures the UART, this symbol records
        # it, and src/ant_uart_transport.c BUILD_ASSERTs they agree - so this
        # check is not about them disagreeing. It is about the number reaching
        # .config at all: CONFIG_ANT_DONGLE_UART_BAUD's 57600 comes from a
        # `default ... if SOC_SERIES_CC13X2_CC26X2`, and that is precisely the
        # kind of symbol whose rename this project has been bitten by three
        # times. If it is renamed, the default silently reverts to 115200 -
        # and this line, not a bench session, is what says so.
        if ($t.baud) {
            $gotBaud = ($cfg | Where-Object { $_ -match '^CONFIG_ANT_DONGLE_UART_BAUD=' }) -replace '.*=', ''
            if ([int]$gotBaud -ne [int]$t.baud) {
                throw "$($t.artifact): ANT UART baud is $gotBaud, expected $($t.baud)"
            }
        }

        $halNote = if ($Backend -eq 'core') { ", $rowRadiant HAL" } else { '' }
        Write-Host ("  ok: links at 0x{0:x}, {1} transport, {2} radio{3}" -f $offset, $got, $Backend, $halNote)

        # Only the sdk_ant backend produces artifacts anyone is handed. Release
        # images stay on that backend until the Tier 3 Zwift acceptance passes
        # for radiant; moving them is a recorded decision in
        # docs/decisions/0001, not a side effect of running this script with a
        # different flag.
        if ($Backend -ne 'sdk_ant') {
            $built += $t
            continue
        }

        $zdir = Join-Path $out "$imageName\zephyr"
        switch ($t.pkg) {
            'uf2' { Copy-Item (Join-Path $zdir 'zephyr.uf2') (Join-Path $dist $t.artifact) -Force }
            'hex' { Copy-Item (Join-Path $zdir 'zephyr.hex') (Join-Path $dist $t.artifact) -Force }
            'dfu' {
                if ($SkipDfu) {
                    Write-Host "  (skipping DFU packaging)"
                } else {
                    & (Join-Path $PSScriptRoot 'package_dfu.ps1') `
                        -HexPath (Join-Path $zdir 'zephyr.hex') `
                        -OutPath (Join-Path $dist $t.artifact) | Out-Null
                }
            }
        }

        $built += $t
    }

    # 4. Build independence, for the backends that claim it.
    #
    # This one is not an assertion about the tree; it is a build that either
    # happens or does not. ANT_MODULE_DIR points at a directory that cannot
    # exist, so any surviving reference to an sdk-ant header, module or
    # library resolves through it and the build stops. Success therefore
    # *proves* the core and stub backends are free of sdk-ant, rather than
    # asserting it in a comment that decays the first time someone adds an
    # include. It costs one extra build of one board.
    #
    # It only means anything because the override actually reaches the
    # application image: under sysbuild, -DANT_MODULE_DIR lands in the
    # sysbuild cache, and CMakeLists.txt reads it back out of the sysbuild
    # cache file explicitly. Without that it would silently resolve
    # $env:SDK_ANT_DIR, build green against the real checkout, and prove
    # nothing at all - which is worse than not running it. If this ever needs
    # debugging, check build\independence\dongle\CMakeCache.txt: it
    # must record ANT_MODULE_DIR as the nonexistent path, not as a real one.
    if ($Backend -ne 'sdk_ant' -and $Only -eq '*') {
        $board = 'adafruit_feather_nrf52840/nrf52840/uf2'
        $out = Join-Path $repo 'build\independence'
        $ghost = 'C:/nonexistent-sdk-ant'
        Write-Host "`n=== build independence  [$board]  [$Backend, ANT_MODULE_DIR=$ghost]" -ForegroundColor Cyan

        $log = Join-Path $env:TEMP 'build_independence.log'
        $indep = @("-DANT_RADIO=$Backend", "-DANT_MODULE_DIR=$ghost")
        if ($Backend -eq 'core') { $indep += "-DRADIANT_BACKEND=$RadiantBackend" }
        $rc = Invoke-West @('-z', $zephyr, 'build', '-s', $dongleApp, '-d', $out,
                            '-b', $board, '-p', 'always', '--') $indep $log
        if ($rc -ne 0) {
            Get-Content $log -Tail 30
            throw "the $Backend backend does not build without sdk-ant present; full log at $log"
        }

        $cache = Get-Content (Join-Path $out 'dongle\CMakeCache.txt')
        if (-not ($cache | Where-Object { $_ -match "^ANT_MODULE_DIR:[^=]*=$([regex]::Escape($ghost))$" })) {
            throw "the independence build did not receive ANT_MODULE_DIR=$ghost; it proved nothing"
        }

        $cfg = Get-Content (Join-Path $out 'dongle\zephyr\.config')
        if (-not ($cfg | Where-Object { $_ -eq $backendSymbol })) {
            throw "the independence build did not select $Backend"
        }

        Write-Host "  ok: $Backend builds with no sdk-ant reachable"
    }

    # ── The node ────────────────────────────────────────────────────────────
    #
    # See $nodeTargets above for why this is a second loop with its own
    # assertions rather than more rows in the first one.
    #
    # NO -DANT_RADIO HERE. apps/hrm_ble has no ANT_RADIO axis: it is built out
    # of radiant and radiant/src/profiles/ and has no sdk-ant path at all, which
    # is the demonstration the application exists to make. Passing the flag
    # would be accepted and unused, and an unused flag in a script whose job is
    # asserting that flags took effect is the wrong habit to teach.
    if ($Backend -eq 'core') {
        $nodeApp = Join-Path $repo 'apps\hrm_ble'

        foreach ($t in $nodeTargets) {
            if ($t.dir -notlike $Only) { continue }

            $out = Join-Path $repo "build\$($t.dir)"
            Write-Host "`n=== hrm_ble $($t.dir)  [$($t.board)]" -ForegroundColor Cyan

            $extra = @('--', "-DRADIANT_BACKEND=$RadiantBackend")
            # IMAGE-SCOPED, and the prefix is the basename of the application
            # directory because that is how sysbuild names an image. While this
            # directory was strap/, `-Dstrap_EXTRA_CONF_FILE=` was correct;
            # after the rename the same flag named an image that does not exist
            # and was accepted in silence - CMake says nothing beyond its
            # end-of-run "manually-specified variables were not used" warning,
            # and the BLE image builds green with no BLE in it. Measured: two
            # builds, with the flag and without, produced byte-identical
            # .config files.
            #
            # A plain -DEXTRA_CONF_FILE= is no better - it lands in the sysbuild
            # cache and never reaches the image - and neither is a bare
            # -DCONFIG_x for the same reason, which is why even the sensor
            # choice travels as a fragment.
            if ($t.conf) { $extra += "-Dhrm_ble_EXTRA_CONF_FILE=$($t.conf)" }

            $log = Join-Path $env:TEMP "build_$($t.dir).log"
            $rc = Invoke-West @('-z', $zephyr, 'build', '-s', $nodeApp,
                                '-d', $out, '-b', $t.board, '-p', 'always') $extra $log
            if ($rc -ne 0) {
                Get-Content $log -Tail 30
                throw "build failed for hrm_ble $($t.dir); full log at $log"
            }

            # 'hrm_ble', the basename of the application directory - the same
            # rule that decides the -D prefix above.
            $cfg = Get-Content (Join-Path $out 'hrm_ble\zephyr\.config')

            # 1. radiant's radio HAL, for the reason assertion 4 above gives:
            # a null-radio node boots, logs "transmitting" and puts nothing on
            # the air, and every other check still passes.
            $want = "CONFIG_RADIANT_BACKEND_$($RadiantBackend.ToUpper())=y"
            if (-not ($cfg | Where-Object { $_ -eq $want })) {
                $got = ($cfg | Where-Object { $_ -match '^CONFIG_RADIANT_BACKEND_\w+=y' })
                throw "hrm_ble $($t.dir): expected $want in .config, found '$got'"
            }

            # 2. The heart-rate source. The HRM_SENSOR choice is what makes "I
            # forgot to unhook the simulator" unrepresentable, and this is what
            # makes the choice checkable from outside the build.
            $wantSensor = "CONFIG_HRM_SENSOR_$($t.sensor)=y"
            if (-not ($cfg | Where-Object { $_ -eq $wantSensor })) {
                $got = ($cfg | Where-Object { $_ -match '^CONFIG_HRM_SENSOR_(SIMULATED|EXAMPLE|CUSTOM)=y' })
                throw "hrm_ble $($t.dir): expected $wantSensor in .config, found '$got'"
            }

            # 3. The BLE interlock, on the rows that ask for BLE.
            #
            # HRM_BLE_HRS is set by the fragment; the other two are NOT, and
            # that is the point of asserting them. They come from a `select` and
            # a `default` on HRM_BLE_HRS in apps/hrm_ble/Kconfig, and a Kconfig
            # `default` parsed after the symbol's own definition is silently
            # dead. Without the gate, radiant/Kconfig's `depends on !BT ||
            # ..._GATE_MPSL` drops the nrf backend to null; with the gate but a
            # session count of 0, every timeslot session_open() fails while the
            # image still runs - a node that refuses every arm rather than a
            # build error.
            if ($t.ble) {
                foreach ($sym in @('CONFIG_HRM_BLE_HRS=y',
                                   'CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL=y',
                                   'CONFIG_MPSL_TIMESLOT_SESSION_COUNT=1')) {
                    if (-not ($cfg | Where-Object { $_ -eq $sym })) {
                        throw "hrm_ble $($t.dir): expected $sym in .config"
                    }
                }
            }

            $bleNote = if ($t.ble) { ', BLE HRS + MPSL gate' } else { '' }
            Write-Host ("  ok: {0} HAL, {1} sensor{2}" -f $RadiantBackend, $t.sensor, $bleNote)
        }
    }

    # ── The treadmill ───────────────────────────────────────────────────────
    #
    # See $treadmillTargets above for why this is a THIRD loop rather than
    # optional fields on the node rows.
    #
    # NO -DANT_RADIO HERE either, for the same reason: apps/treadmill has no
    # ANT_RADIO axis and no sdk-ant path at all.
    if ($Backend -eq 'core') {
        $treadmillApp = Join-Path $repo 'apps\treadmill'

        foreach ($t in $treadmillTargets) {
            if ($t.dir -notlike $Only) { continue }

            $out = Join-Path $repo "build\$($t.dir)"
            Write-Host "`n=== treadmill $($t.dir)  [$($t.board)]" -ForegroundColor Cyan

            $extra = @('--', "-DRADIANT_BACKEND=$RadiantBackend")
            # IMAGE-SCOPED, and the prefix is the basename of the application
            # directory because that is how sysbuild names an image. A wrong
            # prefix names an image that does not exist and is accepted in
            # silence - apps/hrm_ble shipped exactly that no-op for a while and
            # measured it: two builds, with the flag and without, produced
            # byte-identical .config files.
            if ($t.conf) { $extra += "-Dtreadmill_EXTRA_CONF_FILE=$($t.conf)" }

            $log = Join-Path $env:TEMP "build_$($t.dir).log"
            $rc = Invoke-West @('-z', $zephyr, 'build', '-s', $treadmillApp,
                                '-d', $out, '-b', $t.board, '-p', 'always') $extra $log
            if ($rc -ne 0) {
                Get-Content $log -Tail 30
                throw "build failed for treadmill $($t.dir); full log at $log"
            }

            # 'treadmill', the basename of the application directory - the same
            # rule that decides the -D prefix above.
            $cfg = Get-Content (Join-Path $out 'treadmill\zephyr\.config')

            # 1. radiant's radio HAL. A null-radio node boots, logs
            # "transmitting" and puts nothing on the air, and every other check
            # still passes.
            $want = "CONFIG_RADIANT_BACKEND_$($RadiantBackend.ToUpper())=y"
            if (-not ($cfg | Where-Object { $_ -eq $want })) {
                $got = ($cfg | Where-Object { $_ -match '^CONFIG_RADIANT_BACKEND_\w+=y' })
                throw "treadmill $($t.dir): expected $want in .config, found '$got'"
            }

            # 2. The treadmill source. The TREADMILL_SOURCE choice is what makes
            # "I forgot to unhook the simulator" unrepresentable.
            $wantSource = "CONFIG_TREADMILL_SOURCE_$($t.source)=y"
            if (-not ($cfg | Where-Object { $_ -eq $wantSource })) {
                $got = ($cfg | Where-Object { $_ -match '^CONFIG_TREADMILL_SOURCE_(SIMULATED|CUSTOM)=y' })
                throw "treadmill $($t.dir): expected $wantSource in .config, found '$got'"
            }

            # 3. The second ANT+ master, asserted in BOTH directions.
            #
            # A `y` row proves the SDM channel is compiled; the fec_only row
            # proves the fallback still configures. Kconfig writes an absent
            # bool as `# CONFIG_X is not set`, which is a different claim from
            # the symbol being omitted entirely, so the negative case is
            # matched on that exact line rather than on the absence of `=y`.
            if ($t.sdm) {
                if (-not ($cfg | Where-Object { $_ -eq 'CONFIG_TREADMILL_ANT_SDM=y' })) {
                    throw "treadmill $($t.dir): expected CONFIG_TREADMILL_ANT_SDM=y in .config"
                }
            } else {
                if (-not ($cfg | Where-Object { $_ -eq '# CONFIG_TREADMILL_ANT_SDM is not set' })) {
                    throw "treadmill $($t.dir): expected '# CONFIG_TREADMILL_ANT_SDM is not set' in .config - " +
                          "the coexistence fallback must actually turn the second master off"
                }
            }

            # 4. The BLE interlock, on the rows that ask for BLE.
            #
            # TREADMILL_BLE is set by the fragment; the other four are NOT, and
            # that is the point of asserting them. Without the MPSL gate,
            # radiant/Kconfig's `depends on !BT || ..._GATE_MPSL` drops the nrf
            # backend to null; with the gate but a session count of 0, every
            # timeslot session_open() fails while the image still runs.
            #
            # BT_SMP is the one that is new in this tree. FTMS Table 4.1 gives
            # the control point security permission "Encryption", so a build
            # without SMP would advertise a control point no client can ever
            # write - which looks exactly like a machine that ignores commands.
            if ($t.ble) {
                foreach ($sym in @('CONFIG_TREADMILL_BLE=y',
                                   'CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL=y',
                                   'CONFIG_MPSL_TIMESLOT_SESSION_COUNT=1',
                                   'CONFIG_BT_RSCS=y',
                                   'CONFIG_BT_SMP=y')) {
                    if (-not ($cfg | Where-Object { $_ -eq $sym })) {
                        throw "treadmill $($t.dir): expected $sym in .config"
                    }
                }
            }

            # 5. The heart-rate slave, on the row that asks for it.
            if ($t.hr) {
                if (-not ($cfg | Where-Object { $_ -eq 'CONFIG_TREADMILL_HR_RX=y' })) {
                    throw "treadmill $($t.dir): expected CONFIG_TREADMILL_HR_RX=y in .config"
                }
            }

            $notes = @()
            if ($t.sdm) { $notes += 'FE-C + SDM' } else { $notes += 'FE-C only' }
            if ($t.ble) { $notes += 'FTMS + RSC + SMP + MPSL gate' }
            if ($t.hr)  { $notes += 'ANT+ HR slave' }
            Write-Host ("  ok: {0} HAL, {1} source, {2}" -f $RadiantBackend, $t.source, ($notes -join ', '))
        }
    }
} finally {
    Pop-Location
}

if ($Backend -ne 'sdk_ant') {
    Write-Host "`n=== $Backend backend built; no artifacts collected ===" -ForegroundColor Cyan
    Write-Host "Only -Backend sdk_ant writes dist\, so a core or stub run cannot"
    Write-Host "overwrite a release image with something that has never been ridden."
    return
}

Write-Host "`n=== dist\ ===" -ForegroundColor Cyan
Get-ChildItem $dist | Sort-Object Name |
    Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize

$ship = $built | Where-Object { $_.release } | ForEach-Object { $_.artifact }
if ($ship) {
    Write-Host "Release artifacts: $($ship -join ', ')"
    Write-Host "The rest are test/dev images and are not attached to releases."
}
