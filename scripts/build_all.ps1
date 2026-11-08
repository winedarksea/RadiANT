# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Build every target and collect the release artifacts into dist\.

.DESCRIPTION
    Mirrors the matrix in .github\workflows\release.yml, including the checks it
    makes after each build, so a local dist\ matches what CI would publish. The
    checks exist because the failures are silent: Partition Manager does not
    complain when the app's link address disagrees with CONFIG_FLASH_LOAD_OFFSET,
    the transport is defaulted from devicetree so a moved USB node silently
    compiles a different one, and the radio backend is a CMake cache variable
    decided before Kconfig runs, so a typo in -DANT_RADIO falls back to a
    default and ships the wrong backend green.

    THREE MATRICES, NOT ONE: $targets (the dongle, apps/dongle and
    apps/dongle_ti - a release artifact with a bootloader offset, a transport
    and a package format), $nodeTargets (apps/hrm_ble - no bootloader, no
    transport, no artifact) and $treadmillTargets (apps/treadmill - shares the
    node shape but not its Kconfig choices). Kept separate rather than folded
    into $targets with optional fields, because `if ($t.field)` would silently
    skip an assertion a row meant to have.

    The node and treadmill rows build only under -Backend core, since neither
    application has an ANT_RADIO axis - both are built out of radiant with no
    sdk-ant path, which is the demonstration they exist to make.

    See docs\gotchas.md for the promicro/lm20 `radiant,radio-timer` build
    failure this matrix caught.

    The TI rows need -NcsVersion v3.4.0: hal_ti lives at
    C:\ncs\v3.4.0\modules\hal\ti and is ABSENT under v3.2.4, where the row is
    skipped with a message rather than failing.

.PARAMETER NcsVersion
    NCS version to build against. Defaults to v3.4.0, which is what the
    shipping backend tracks and what the TI rows require.

    -Backend sdk_ant NEEDS -NcsVersion v3.2.4 PASSED EXPLICITLY: sdk-ant v2.1.0
    keys ANT_LIB_DIR off CONFIG_SOC_SERIES_NRF52X, which v3.4.0's Zephyr
    renamed to CONFIG_SOC_SERIES_NRF52, so libant.a is never located.

.PARAMETER Backend
    Which implementation of apps/common/ant/ant_radio.h to build against:
    core (the clean-room radiant stack, the default, and the one release
    artifacts are cut from), sdk_ant (Nordic's prebuilt libant.a) or stub.
    Passed through as -DANT_RADIO and asserted afterwards against .config.

    sdk_ant is the A/B comparison reference and nothing else: not built by CI,
    writes nothing to dist\, kept only because it is the other half of every
    gate in tools\ab_gates.toml. See
    docs\decisions\0001-backend-selection-and-release-default.md.

.PARAMETER SdkAntDir
    Path to the sdk-ant checkout, for -Backend sdk_ant. Defaults to
    $env:SDK_ANT_DIR, then to a sibling checkout beside this repository -
    passed explicitly here so the value is visible in the build log rather
    than being whatever the machine happened to have configured.

.PARAMETER RadiantBackend
    Which radiant radio HAL to compile, for -Backend core: nrf (the real
    radio) or null (an inert stub that transmits and receives nothing).
    Defaults to nrf, matching CMakeLists.txt's default. A null-radio image
    enumerates, answers every host command with OK and finds no sensors,
    which is indistinguishable from a dead antenna.

.PARAMETER HalTiDir
    Where the hal_ti Zephyr module is, for the CC26x2 target. Defaults to
    where scripts\fetch_hal_ti.ps1 puts it. NCS's west manifest filters this
    module out of its import, so the TI row is skipped with a message rather
    than failing the whole matrix when it is missing.

.PARAMETER Only
    Build just the targets whose artifact name matches this wildcard. The node
    rows (apps/hrm_ble) have no artifact, so they match on their
    build-directory name instead: -Only "node_*".

.PARAMETER SkipDfu
    Do not package the dongle DFU zip (which needs nrfutil).

.EXAMPLE
    .\scripts\build_all.ps1
    .\scripts\build_all.ps1 -Only "*promicro*"
    # The A/B comparison reference. Note the SDK pin, which is not the default.
    .\scripts\build_all.ps1 -Backend sdk_ant -NcsVersion v3.2.4
#>
[CmdletBinding()]
param(
    [string]$NcsVersion = 'v3.4.0',
    [ValidateSet('sdk_ant', 'core', 'stub')]
    [string]$Backend = 'core',
    [ValidateSet('null', 'nrf')]
    [string]$RadiantBackend = 'nrf',
    [string]$SdkAntDir = '',
    [string]$HalTiDir = '',
    [string]$Only = '*',
    [switch]$SkipDfu
)

$ErrorActionPreference = 'Stop'

# WEST MUST NOT BE CALLED DIRECTLY WITH $ErrorActionPreference = 'Stop': west
# writes ordinary progress to stderr, PowerShell 5.1 wraps a native command's
# stderr in ErrorRecords, and 'Stop' then aborts on the first progress line
# with a NativeCommandError that reads exactly like a build failure while the
# build was fine. `> $log` does not help - it only redirects stdout. So the
# preference drops to 'Continue' across the west call only, and $LASTEXITCODE
# is the sole verdict tested. scripts/build_p4.ps1 does the same for the same
# reason.
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

# The same trick one level down, for radiant's own HAL choice, is computed per
# target rather than once here because a row may pin its own backend
# regardless of -RadiantBackend (see the cc26x2r1_launchxl row). Only the core
# backend has such a choice at all; sdk_ant and stub never look at
# RADIANT_BACKEND.

# Same fields as the CI matrix: 'offset' is where the app must link and
# 'transport' is which of the three src/ transports must be compiled; both are
# asserted after the build. The `release` row's overlay declares the LED node
# and `rgb-led0` alias; its conf turns the module on. Either alone fails
# loudly by design - see apps\dongle\rgb.conf.
$targets = @(
    @{ dir='release';        board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='radiant_dongle.uf2';                   pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf='rgb.conf';    release=$true; overlay='rgb_feather.overlay' }
    @{ dir='dongle';         board='nrf52840dongle/nrf52840';                artifact='radiant_dongle_nrf52840dongle.zip';    pkg='dfu'; offset=0x1000;  transport='USB_LEGACY'; conf=$null;         release=$true  }
    @{ dir='promicro';       board='promicro_nrf52840/nrf52840/uf2';         artifact='radiant_dongle_promicro.uf2';          pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf=$null;         release=$true; assert_config=@('CONFIG_ANT_DONGLE_UF2_REBOOT_REQUEST=y') }
    @{ dir='promicro_synth'; board='promicro_nrf52840/nrf52840/uf2';         artifact='radiant_dongle_promicro_synth.uf2';    pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf='synth.conf';  release=$true; assert_config=@('CONFIG_ANT_DONGLE_UF2_REBOOT_REQUEST=y') }
    @{ dir='feather_next';   board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='radiant_dongle_feather_usbd.uf2';      pkg='uf2'; offset=0x26000; transport='USB_NEXT';   conf='next.conf';   release=$false }
    @{ dir='l15';            board='nrf54l15dk/nrf54l15/cpuapp';             artifact='radiant_dongle_nrf54l15dk.hex';        pkg='hex'; offset=0x0;     transport='UART';       conf=$null;         release=$false }
    @{ dir='lm20';           board='nrf54lm20dk/nrf54lm20a/cpuapp';          artifact='radiant_dongle_nrf54lm20dk.hex';       pkg='hex'; offset=0x0;     transport='USB_NEXT';   conf=$null;         release=$false }
)

# Default-off fragments, gated on core only: CONFIG_RADIANT_SEC depends on
# CONFIG_RADIANT, so a security build under -Backend sdk_ant would compile
# nothing and pass - which looks like coverage but is not.
if ($Backend -eq 'core') {
    $targets += @{ dir='feather_security';   board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='radiant_dongle_feather_security.uf2';   pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf='security.conf';   release=$false }
    $targets += @{ dir='feather_encryption'; board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='radiant_dongle_feather_encryption.uf2'; pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf='encryption.conf'; release=$false }
}

# ---- The second vendor ------------------------------------------------------
#
# A LAUNCHXL-CC26X2R1. Three things differ from every other row:
#
#   `radiant` overrides -RadiantBackend to cc26xx - nrf names a peripheral
#   this part does not have. Only the base row sets it; the coex arms below
#   inherit it from the base row's $t.radiant.
#
#   `baud` is asserted at 57600, not the usual 115200: the CC2652P + CP2102N
#   module impersonates a Dynastream ANT2USB. A mismatch looks like "no
#   response to reset", indistinguishable from a hung image.
#
#   `modules` names hal_ti, which NCS's west manifest filters out of its
#   import (scripts/fetch_hal_ti.ps1 fetches it); the row is skipped rather
#   than failing the matrix when it is absent.
#
# Only under -Backend core: sdk_ant is Nordic-only silicon and a stub build
# asserts nothing about the port.
#
# Four rows, an A/B/C/D read in order - see
# docs/decisions/0015-cc26xx-coexistence-design.md and docs/testing.md:
#
#   ti_launchxl        arm 1  the floor
#   ti_launchxl_patch  arm 2  the CPE multi-protocol patch's PHY cost alone
#   ti_launchxl_gate   arm 3  the scheduler hooks, patch held constant
#   ti_launchxl_coex   arm 4  a real second RF client (forked 802.15.4
#                             driver), arbiter and patch held constant
#
# Arm 4 needs a devicetree overlay too: Zephyr's own cc26x2r1_launchxl.dts
# disables the 802.15.4 node.
#
# ARM 4 BUILDS BUT HAS NEVER RUN AGAINST AN 802.15.4 PEER - this bench has
# none. It stays in the matrix so the build cannot rot, not because a green
# matrix proves coexistence works. Read ti_coex.conf's header before recording
# any number off it.
if ($Backend -eq 'core') {
    if (-not $HalTiDir) { $HalTiDir = "C:\ncs\$NcsVersion\modules\hal\ti" }
    if (Test-Path (Join-Path $HalTiDir 'zephyr\module.yml')) {
        $halTiModule = ($HalTiDir -replace '\\', '/')
        # app: apps/dongle_ti, not apps/dongle - the only board with no USB
        # device peripheral to make an ANT_DONGLE-shaped image on, and the
        # only silicon radiant's cc26xx backend targets. See apps/dongle_ti's
        # own README for the "unverified pending hardware" scope of arm 4.
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

# ---- The node ----------------------------------------------------------------
#
# apps/hrm_ble, the heart-rate node reference design. A separate list rather
# than more $targets rows: offset, transport and pkg have no meaning here (no
# bootloader, no ANT_DONGLE_TRANSPORT symbol, no release artifact), and
# `if ($t.field)` would silently skip an assertion a row meant to have.
#
# Asserted below: the HAL backend (RADIANT_BACKEND falls back to null on an
# unmet `depends on`, and a null-radio node boots and logs "transmitting"
# while putting nothing on the air), the HRM_SENSOR arm by name (so a
# simulator left wired in fails the build instead of shipping), and on BLE
# rows, HRM_BLE_HRS plus the symbols it `select`s/`default`s rather than sets
# directly - those defaults are silently dead if Kconfig parses them after
# the symbol's own definition, and only a .config read-back catches it.
#
# Only under -Backend core: this application has no ANT_RADIO axis at all.
$nodeTargets = @(
    @{ dir='node_l15';        board='nrf54l15dk/nrf54l15/cpuapp';    sensor='SIMULATED'; conf=$null }
    # The other half of radiant/Kconfig's `SOC_COMPATIBLE_NRF52X ||
    # SOC_COMPATIBLE_NRF54LX`. Build-verified only - no nRF52 bench slot for
    # this application; on-air behaviour is predicted from nRF54L15 measurements.
    @{ dir='node_52840dk';    board='nrf52840dk/nrf52840';           sensor='SIMULATED'; conf=$null }
    @{ dir='node_lm20';       board='nrf54lm20dk/nrf54lm20a/cpuapp'; sensor='SIMULATED'; conf=$null }
    @{ dir='node_ble';        board='nrf54l15dk/nrf54l15/cpuapp';    sensor='SIMULATED'; conf='ble.conf';                     ble=$true }
    @{ dir='node_ble_nosup';  board='nrf54l15dk/nrf54l15/cpuapp';    sensor='SIMULATED'; conf='ble.conf;ble_nosuppress.conf'; ble=$true }
    # The seam's second implementation, and the arm with no implementation:
    # a seam with only one implementation is indistinguishable from a direct
    # call, and if the empty case doesn't link, an integrator's first build
    # fails for the wrong reason.
    @{ dir='node_example';    board='nrf54l15dk/nrf54l15/cpuapp';    sensor='EXAMPLE';   conf='sensor_example.conf' }
    @{ dir='node_custom';     board='nrf54l15dk/nrf54l15/cpuapp';    sensor='CUSTOM';    conf='sensor_custom.conf' }
)

# ---- The treadmill -----------------------------------------------------------
#
# apps/treadmill, the fitness-equipment reference design. A third list for the
# same reason $nodeTargets isn't part of $targets, with its own fields:
# TREADMILL_SOURCE (SIMULATED/CUSTOM, not HRM_SENSOR's three arms) and its own
# BLE symbol set (TREADMILL_BLE selects BT_RSCS and BT_SMP, not HRM's set).
#
# Asserted below: the HAL backend and source arm (same rationale as the node
# rows), CONFIG_TREADMILL_ANT_SDM in BOTH directions (fec_only proves the
# coexistence fallback still configures - a fallback nobody compiles is not a
# fallback), and on BLE rows, TREADMILL_BLE plus the four symbols it
# select/default rather than sets directly.
$treadmillTargets = @(
    @{ dir='tread_l15';      board='nrf54l15dk/nrf54l15/cpuapp'; source='SIMULATED'; conf=$null;                  sdm=$true }
    # The other half of radiant/Kconfig's `SOC_COMPATIBLE_NRF52X ||
    # SOC_COMPATIBLE_NRF54LX`. ANT+ only - no nRF52 bench slot for the BLE arm
    # (SDC + MPSL + gate + two GATT services + SMP, untested on that family).
    # See apps/treadmill/boards/nrf52840dk_nrf52840.overlay.
    @{ dir='tread_52840dk';  board='nrf52840dk/nrf52840';        source='SIMULATED'; conf=$null;                  sdm=$true }
    # One ANT+ master instead of two - the coexistence fallback, compiled so
    # it cannot rot. Two masters at 8192 and 8134 counts beat against each
    # other with a period of about 32 s; that mix has never been measured
    # alongside BLE.
    @{ dir='tread_fec_only'; board='nrf54l15dk/nrf54l15/cpuapp'; source='SIMULATED'; conf='fec_only.conf';        sdm=$false }
    @{ dir='tread_ble';      board='nrf54l15dk/nrf54l15/cpuapp'; source='SIMULATED'; conf='ble.conf';             sdm=$true; ble=$true }
    # The heaviest configuration here: two masters, one slave, BLE with SMP
    # and a bonding store - the row that decides whether the RAM budget holds
    # (RAM, not flash, is the constraint on this part; see the Matter spike).
    @{ dir='tread_ble_hr';   board='nrf54l15dk/nrf54l15/cpuapp'; source='SIMULATED'; conf='ble.conf;hr_rx.conf';  sdm=$true; ble=$true; hr=$true }
    # The arm with no source: must LINK (an integrator's first build failing
    # for an unrelated reason is expensive) and report a stopped belt rather
    # than falling back to the simulator.
    @{ dir='tread_custom';   board='nrf54l15dk/nrf54l15/cpuapp'; source='CUSTOM';    conf='source_custom.conf';   sdm=$true }
)

if (-not (Get-Command west -ErrorAction SilentlyContinue)) {
    throw "west is not on PATH. Dot-source the environment first:`n" +
          "  . .\scripts\env.ps1 -NcsVersion $NcsVersion"
}

# Resolve sdk-ant here rather than letting CMake do it silently. Only the
# sdk_ant backend needs it; core and stub never look at it - the independence
# build below proves that rather than asserting it.
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

# `west build` is discovered through the workspace manifest, so west must run
# with cwd inside the workspace even though -s/-d/-z point elsewhere.
#
# Every row builds from apps/dongle except cc26x2r1_launchxl, which names its
# own $t.app (apps/dongle_ti). The sysbuild image folder under each row's -d
# is therefore the app directory's basename ('dongle' or 'dongle_ti') -
# sysbuild takes the image name from APP_DIR's basename, not from project().
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

        # A row may pin its own HAL backend (only TI does - see its row note,
        # -RadiantBackend's default names a peripheral that part lacks).
        $rowRadiant = if ($t.radiant) { $t.radiant } else { $RadiantBackend }

        $extra = @('--', "-DANT_RADIO=$Backend")
        if ($Backend -eq 'core') { $extra += "-DRADIANT_BACKEND=$rowRadiant" }
        if ($antModuleDir) { $extra += "-DANT_MODULE_DIR=$antModuleDir" }
        if ($t.modules) { $extra += "-DEXTRA_ZEPHYR_MODULES=$($t.modules)" }
        if ($t.conf) { $extra += "-DEXTRA_CONF_FILE=$($t.conf)" }
        # Only ti_launchxl_coex sets this. Not folded into `conf`: a fragment
        # and an overlay are read by different tools/stages, and a row that
        # silently dropped its overlay would still build - the wrong image.
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

        # 3. Radio backend. ANT_RADIO is a CMake cache variable chosen before
        # Kconfig runs, so nothing in .config records it unless CMake writes
        # the symbol on purpose. Without this check a -DANT_RADIO typo falls
        # back to a default and ships the wrong backend green.
        if (-not ($cfg | Where-Object { $_ -eq $backendSymbol })) {
            $gotRadio = ($cfg | Where-Object { $_ -match '^CONFIG_ANT_DONGLE_RADIO_\w+=y' })
            throw "$($t.artifact): expected $backendSymbol in .config, found '$gotRadio'"
        }

        # 4. radiant's radio HAL. RADIANT_BACKEND can be passed explicitly and
        # Kconfig can decline to honour it when a `depends on` is unmet; the
        # result is an image that transmits and receives nothing while checks
        # 1-3 all still pass. Such an image enumerates and answers OK to every
        # host command, and reads as a dead antenna, not a build defect.
        if ($Backend -eq 'core') {
            $rowRadiantSymbol = "CONFIG_RADIANT_BACKEND_$($rowRadiant.ToUpper())=y"
            if (-not ($cfg | Where-Object { $_ -eq $rowRadiantSymbol })) {
                $gotHal = ($cfg | Where-Object { $_ -match '^CONFIG_RADIANT_BACKEND_\w+=y' })
                throw "$($t.artifact): expected $rowRadiantSymbol in .config, found '$gotHal'"
            }
        }

        # 5. The ANT UART's baud, only asserted for TI (the one board not at
        # 115200). CONFIG_ANT_DONGLE_UART_BAUD's 57600 comes from a `default
        # ... if SOC_SERIES_CC13X2_CC26X2` - exactly the kind of symbol this
        # project has had renamed out from under it before. If it is renamed,
        # the default silently reverts to 115200 and only this check notices.
        if ($t.baud) {
            $gotBaud = ($cfg | Where-Object { $_ -match '^CONFIG_ANT_DONGLE_UART_BAUD=' }) -replace '.*=', ''
            if ([int]$gotBaud -ne [int]$t.baud) {
                throw "$($t.artifact): ANT UART baud is $gotBaud, expected $($t.baud)"
            }
        }

        # 6. Symbols a row must have, the counterpart of release.yml's
        # assert_config. A whole-line compare against the generated .config,
        # because a symbol whose `depends on` stops being met is dropped by
        # Kconfig with no error and the image still builds. foreach over an
        # absent field runs zero times, which is right for the other rows.
        foreach ($sym in $t.assert_config) {
            if (-not ($cfg | Where-Object { $_ -ceq $sym })) {
                throw "$($t.artifact): expected $sym in .config"
            }
        }

        $halNote = if ($Backend -eq 'core') { ", $rowRadiant HAL" } else { '' }
        Write-Host ("  ok: links at 0x{0:x}, {1} transport, {2} radio{3}" -f $offset, $got, $Backend, $halNote)

        # Only the core backend produces artifacts anyone is handed (see
        # docs/decisions/0001). An sdk_ant run is a comparison build and
        # deliberately leaves dist\ alone, so a bench A/B cannot overwrite
        # images about to ship.
        if ($Backend -ne 'core') {
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

    # Build independence, for backends that claim it. ANT_MODULE_DIR points at
    # a directory that cannot exist, so any surviving sdk-ant reference
    # resolves through it and the build stops - success *proves* core/stub are
    # free of sdk-ant rather than asserting it in a comment that decays.
    #
    # Only means anything if the override reaches the image: under sysbuild,
    # -DANT_MODULE_DIR lands in the sysbuild cache and CMakeLists.txt reads it
    # back explicitly. Without that it silently resolves $env:SDK_ANT_DIR and
    # proves nothing. Debugging: build\independence\dongle\CMakeCache.txt must
    # record ANT_MODULE_DIR as the nonexistent path.
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

    # ---- The node ------------------------------------------------------------
    #
    # NO -DANT_RADIO HERE: apps/hrm_ble has no ANT_RADIO axis. The flag would
    # be accepted and silently unused, which is the wrong habit for a script
    # whose job is asserting that flags took effect.
    if ($Backend -eq 'core') {
        $nodeApp = Join-Path $repo 'apps\hrm_ble'

        foreach ($t in $nodeTargets) {
            if ($t.dir -notlike $Only) { continue }

            $out = Join-Path $repo "build\$($t.dir)"
            Write-Host "`n=== hrm_ble $($t.dir)  [$($t.board)]" -ForegroundColor Cyan

            $extra = @('--', "-DRADIANT_BACKEND=$RadiantBackend")
            # IMAGE-SCOPED: the prefix is the app directory's basename, which
            # is how sysbuild names an image. A wrong prefix names an image
            # that doesn't exist and is accepted in silence - CMake's only
            # complaint is an end-of-run "manually-specified variables were
            # not used" warning, and the image builds green with no BLE in it.
            # Measured: two builds, with the flag and without, produced
            # byte-identical .config files. A plain -DEXTRA_CONF_FILE= is no
            # better (lands in the sysbuild cache, never reaches the image),
            # nor is a bare -DCONFIG_x - which is why even the sensor choice
            # travels as a fragment.
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

            # 1. radiant's radio HAL: a null-radio node boots, logs
            # "transmitting" and puts nothing on the air, while every other
            # check still passes.
            $want = "CONFIG_RADIANT_BACKEND_$($RadiantBackend.ToUpper())=y"
            if (-not ($cfg | Where-Object { $_ -eq $want })) {
                $got = ($cfg | Where-Object { $_ -match '^CONFIG_RADIANT_BACKEND_\w+=y' })
                throw "hrm_ble $($t.dir): expected $want in .config, found '$got'"
            }

            # 2. The heart-rate source, so a simulator left wired in fails the
            # build instead of shipping.
            $wantSensor = "CONFIG_HRM_SENSOR_$($t.sensor)=y"
            if (-not ($cfg | Where-Object { $_ -eq $wantSensor })) {
                $got = ($cfg | Where-Object { $_ -match '^CONFIG_HRM_SENSOR_(SIMULATED|EXAMPLE|CUSTOM)=y' })
                throw "hrm_ble $($t.dir): expected $wantSensor in .config, found '$got'"
            }

            # 3. The BLE interlock. HRM_BLE_HRS is set by the fragment; the
            # other two come from a `select`/`default` on it in
            # apps/hrm_ble/Kconfig - a `default` parsed after the symbol's own
            # definition is silently dead. Without the gate, radiant/Kconfig's
            # `depends on !BT || ..._GATE_MPSL` drops the nrf backend to null;
            # with the gate but session count 0, every timeslot session_open()
            # fails while the image still runs.
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

    # ---- The treadmill --------------------------------------------------------
    #
    # NO -DANT_RADIO HERE either: apps/treadmill has no ANT_RADIO axis.
    if ($Backend -eq 'core') {
        $treadmillApp = Join-Path $repo 'apps\treadmill'

        foreach ($t in $treadmillTargets) {
            if ($t.dir -notlike $Only) { continue }

            $out = Join-Path $repo "build\$($t.dir)"
            Write-Host "`n=== treadmill $($t.dir)  [$($t.board)]" -ForegroundColor Cyan

            $extra = @('--', "-DRADIANT_BACKEND=$RadiantBackend")
            # IMAGE-SCOPED, same rule as apps/hrm_ble above: a wrong prefix
            # names an image that doesn't exist and is accepted in silence.
            if ($t.conf) { $extra += "-Dtreadmill_EXTRA_CONF_FILE=$($t.conf)" }

            $log = Join-Path $env:TEMP "build_$($t.dir).log"
            $rc = Invoke-West @('-z', $zephyr, 'build', '-s', $treadmillApp,
                                '-d', $out, '-b', $t.board, '-p', 'always') $extra $log
            if ($rc -ne 0) {
                Get-Content $log -Tail 30
                throw "build failed for treadmill $($t.dir); full log at $log"
            }

            # 'treadmill', the basename of the application directory - the
            # same rule that decides the -D prefix above.
            $cfg = Get-Content (Join-Path $out 'treadmill\zephyr\.config')

            # 1. radiant's radio HAL, same failure mode as the node loop above.
            $want = "CONFIG_RADIANT_BACKEND_$($RadiantBackend.ToUpper())=y"
            if (-not ($cfg | Where-Object { $_ -eq $want })) {
                $got = ($cfg | Where-Object { $_ -match '^CONFIG_RADIANT_BACKEND_\w+=y' })
                throw "treadmill $($t.dir): expected $want in .config, found '$got'"
            }

            # 2. The treadmill source, so a simulator left wired in fails the
            # build instead of shipping.
            $wantSource = "CONFIG_TREADMILL_SOURCE_$($t.source)=y"
            if (-not ($cfg | Where-Object { $_ -eq $wantSource })) {
                $got = ($cfg | Where-Object { $_ -match '^CONFIG_TREADMILL_SOURCE_(SIMULATED|CUSTOM)=y' })
                throw "treadmill $($t.dir): expected $wantSource in .config, found '$got'"
            }

            # 3. The second ANT+ master, asserted in BOTH directions: a `y`
            # row proves the SDM channel compiles, fec_only proves the
            # fallback still configures. Kconfig writes an absent bool as
            # `# CONFIG_X is not set`, so the negative case matches that exact
            # line rather than the absence of `=y`.
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

            # 4. The BLE interlock. TREADMILL_BLE is set by the fragment; the
            # other four are not, and that is the point of asserting them.
            # Without the MPSL gate, radiant/Kconfig's `depends on !BT ||
            # ..._GATE_MPSL` drops the nrf backend to null; with the gate but
            # session count 0, every timeslot session_open() fails while the
            # image still runs.
            #
            # BT_SMP: FTMS Table 4.1 gives the control point security
            # permission "Encryption", so a build without SMP would advertise
            # a control point no client can ever write.
            #
            # BT_LL_SOFTDEVICE: TREADMILL_BLE does not `select` it, so this
            # application relies on the SDC being the default link layer for
            # CONFIG_BT=y on nRF54L15 (verified true from .config, but latent).
            # The MPSL gate this application arbitrates through assumes the
            # SDC; a board or Kconfig change that moved to the open-source LL
            # would break arbitration with a perfectly green build.
            if ($t.ble) {
                foreach ($sym in @('CONFIG_TREADMILL_BLE=y',
                                   'CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL=y',
                                   'CONFIG_MPSL_TIMESLOT_SESSION_COUNT=1',
                                   'CONFIG_BT_RSCS=y',
                                   'CONFIG_BT_SMP=y',
                                   'CONFIG_BT_LL_SOFTDEVICE=y')) {
                    if (-not ($cfg | Where-Object { $_ -eq $sym })) {
                        throw "treadmill $($t.dir): expected $sym in .config"
                    }
                }

                # 4b. The arbitration policy is a `choice`, so it has an arm
                # set whether or not anyone meant to. EXCLUSIVE is the default
                # every bench/CI result is taken against; NONE restores
                # last-writer-wins, a legitimate integrator choice but not one
                # to ship by accident.
                if (-not ($cfg | Where-Object { $_ -eq 'CONFIG_TREADMILL_CTRL_POLICY_EXCLUSIVE=y' })) {
                    throw "treadmill $($t.dir): expected " +
                          "CONFIG_TREADMILL_CTRL_POLICY_EXCLUSIVE=y in .config - " +
                          "without it two controllers write the same setpoint " +
                          "with no signal to either"
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

if ($Backend -ne 'core') {
    Write-Host "`n=== $Backend backend built; no artifacts collected ===" -ForegroundColor Cyan
    Write-Host "Only -Backend core writes dist\. sdk_ant is the A/B comparison"
    Write-Host "reference, so a bench run of it cannot overwrite a release image."
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
