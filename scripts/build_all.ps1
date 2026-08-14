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
    clean-room radiant_core stack) or stub. Passed through as -DANT_RADIO and
    asserted afterwards against .config.

.PARAMETER SdkAntDir
    Path to the sdk-ant checkout, for -Backend sdk_ant. Defaults to
    $env:SDK_ANT_DIR, then to a sibling checkout beside this repository - the
    same order CMakeLists.txt resolves in. It is passed explicitly on the
    command line rather than left to that resolution so the value is visible
    in the build log and in a failure message, instead of being whatever the
    machine happened to have configured.

.PARAMETER RadiantBackend
    Which radiant_core radio HAL to compile, for -Backend core: nrf (the real
    radio) or null (an inert stub that transmits and receives nothing).

    It defaults to nrf here even though CMakeLists.txt defaults it to null,
    and the difference is deliberate. CMake's default is right for a compile
    check; it is wrong for a script whose whole job is producing images
    somebody flashes. A null-radio image enumerates, answers every host
    command with OK and finds no sensors - indistinguishable from a dead
    antenna - and it cost a Feather flash and a whole Zwift session to
    identify once already, because none of the three checks below looked at
    it. Now one does.

.PARAMETER HalTiDir
    Where the hal_ti Zephyr module is, for the CC26x2 target. Defaults to
    where scripts\fetch_hal_ti.ps1 puts it. NCS's west manifest filters this
    module out of its import, so it is never present unless that script has
    been run; the TI row is skipped with a message rather than failing the
    whole matrix when it is missing, because every other target builds fine
    without it.

.PARAMETER Only
    Build just the targets whose artifact name matches this wildcard.

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

$repo = Split-Path $PSScriptRoot -Parent
$zephyr = "C:\ncs\$NcsVersion\zephyr"
$dist = Join-Path $repo 'dist'

# CONFIG_ANT_DONGLE_RADIO_SDK_ANT / _CORE / _STUB. The Kconfig choice arm is
# the backend name upper-cased, which is not a coincidence - CMakeLists.txt
# generates the symbol from ANT_RADIO the same way.
$backendSymbol = "CONFIG_ANT_DONGLE_RADIO_$($Backend.ToUpper())=y"

# The same trick one level down - for radiant_core's own HAL choice - is now
# computed per target rather than once here, because a row may pin its own
# backend regardless of -RadiantBackend. See the cc26x2r1_launchxl row. Only
# the core backend has such a choice at all; sdk_ant and stub never look at
# RADIANT_BACKEND.

# Same fields as the CI matrix. 'offset' is where the application must link and
# 'transport' is which of the three src/ transports must end up compiled; both
# are asserted after the build.
$targets = @(
    @{ dir='release';        board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='ant_dongle.uf2';                   pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf=$null;         release=$true  }
    @{ dir='dongle';         board='nrf52840dongle/nrf52840';                artifact='ant_dongle_nrf52840dongle.zip';    pkg='dfu'; offset=0x1000;  transport='USB_LEGACY'; conf=$null;         release=$true  }
    @{ dir='promicro';       board='promicro_nrf52840/nrf52840/uf2';         artifact='ant_dongle_promicro.uf2';          pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf=$null;         release=$true  }
    @{ dir='promicro_synth'; board='promicro_nrf52840/nrf52840/uf2';         artifact='ant_dongle_promicro_synth.uf2';    pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf='synth.conf';  release=$true  }
    @{ dir='feather_next';   board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='ant_dongle_feather_usbd.uf2';      pkg='uf2'; offset=0x26000; transport='USB_NEXT';   conf='next.conf';   release=$false }
    @{ dir='l15';            board='nrf54l15dk/nrf54l15/cpuapp';             artifact='ant_dongle_nrf54l15dk.hex';        pkg='hex'; offset=0x0;     transport='UART';       conf=$null;         release=$false }
    @{ dir='lm20';           board='nrf54lm20dk/nrf54lm20a/cpuapp';          artifact='ant_dongle_nrf54lm20dk.hex';       pkg='hex'; offset=0x0;     transport='USB_NEXT';   conf=$null;         release=$false }
)

# The default-off configuration fragments, which only build under a backend
# that can reach them.
#
# encryption.conf's own comment says it exists "so a default-off path does not
# quietly stop compiling", and it had never appeared in this list or in either
# CI matrix - so the thing it was written to prevent was never prevented for
# it. security.conf gets its entry in the same change that creates it.
#
# CONFIG_RADIANT_SEC depends on CONFIG_RADIANT_CORE, so a security build under
# -Backend sdk_ant would compile nothing and pass, which is worse than not
# building it: it would look like coverage. Hence the gate on $Backend rather
# than an unconditional row.
if ($Backend -eq 'core') {
    $targets += @{ dir='feather_security'; board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='ant_dongle_feather_security.uf2'; pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf='security.conf'; release=$false }
}
if ($Backend -eq 'sdk_ant') {
    $targets += @{ dir='feather_encryption'; board='adafruit_feather_nrf52840/nrf52840/uf2'; artifact='ant_dongle_feather_encryption.uf2'; pkg='uf2'; offset=0x26000; transport='USB_LEGACY'; conf='encryption.conf'; release=$false }
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
# THREE ROWS, NOT ONE: the base build (arm 1, today's floor) plus the two
# coexistence arms that do not need the 802.15.4 driver fork yet - ti_patch.conf
# (arm 2, the CPE patch alone) and ti_gate.conf (arm 3, the scheduler hooks).
# Arm 4 (ti_coex.conf) is not a row here until the coex154/ fork exists - see
# docs/decisions/0015-cc26xx-coexistence-design.md's "Status" section; adding
# it before then would build a coexistence arm with nothing to arbitrate
# against, which is arm 3 measured twice under a different name.
if ($Backend -eq 'core') {
    if (-not $HalTiDir) { $HalTiDir = "C:\ncs\$NcsVersion\modules\hal\ti" }
    if (Test-Path (Join-Path $HalTiDir 'zephyr\module.yml')) {
        $halTiModule = ($HalTiDir -replace '\\', '/')
        $targets += @{ dir='ti_launchxl';       board='cc26x2r1_launchxl'; artifact='ant_dongle_cc26x2r1_launchxl.hex';       pkg='hex'; offset=0x0; transport='UART'; conf=$null;           release=$false; radiant='cc26xx'; baud=57600; modules=$halTiModule }
        $targets += @{ dir='ti_launchxl_patch'; board='cc26x2r1_launchxl'; artifact='ant_dongle_cc26x2r1_launchxl_patch.hex'; pkg='hex'; offset=0x0; transport='UART'; conf='ti_patch.conf'; release=$false; radiant='cc26xx'; baud=57600; modules=$halTiModule }
        $targets += @{ dir='ti_launchxl_gate';  board='cc26x2r1_launchxl'; artifact='ant_dongle_cc26x2r1_launchxl_gate.hex';  pkg='hex'; offset=0x0; transport='UART'; conf='ti_gate.conf';  release=$false; radiant='cc26xx'; baud=57600; modules=$halTiModule }
    } else {
        Write-Host "cc26x2r1_launchxl: skipped, no hal_ti module at $HalTiDir" -ForegroundColor DarkYellow
        Write-Host "  Run scripts\fetch_hal_ti.ps1 (NCS's west manifest will never fetch it)."
    }
}

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
Push-Location "C:\ncs\$NcsVersion"
try {
    $built = @()

    foreach ($t in $targets) {
        if ($t.artifact -notlike $Only) { continue }

        $out = Join-Path $repo "build\$($t.dir)"
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

        # Do not redirect stderr: PowerShell 5.1 wraps a native command's
        # stderr in ErrorRecords and reports failure even on exit code 0.
        $log = Join-Path $env:TEMP "build_$($t.dir).log"
        west -z $zephyr build -s $repo -d $out -b $t.board -p always @extra > $log
        if ($LASTEXITCODE -ne 0) {
            Get-Content $log -Tail 30
            throw "build failed for $($t.artifact); full log at $log"
        }

        $cfgPath = Join-Path $out 'ant_dongle\zephyr\.config'
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

        # 4. radiant_core's radio HAL. RADIANT_BACKEND defaults to null in
        # CMakeLists.txt, so a -Backend core build that simply never mentions
        # it compiles a radio that transmits and receives nothing - and checks
        # 1 to 3 all pass, because the link address, the transport and the
        # ANT_DONGLE_RADIO choice are all still exactly right. The resulting
        # image enumerates and answers every host command with OK. The only
        # symptom is that no sensor is ever found, which reads as a hardware
        # fault and is why this check is worth its lines.
        if ($Backend -eq 'core') {
            $rowRadiantSymbol = "CONFIG_RADIANT_CORE_BACKEND_$($rowRadiant.ToUpper())=y"
            if (-not ($cfg | Where-Object { $_ -eq $rowRadiantSymbol })) {
                $gotHal = ($cfg | Where-Object { $_ -match '^CONFIG_RADIANT_CORE_BACKEND_\w+=y' })
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
        # for radiant_core; moving them is a recorded decision in
        # docs/decisions/0001, not a side effect of running this script with a
        # different flag.
        if ($Backend -ne 'sdk_ant') {
            $built += $t
            continue
        }

        $zdir = Join-Path $out 'ant_dongle\zephyr'
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
    # debugging, check build\independence\ant_dongle\CMakeCache.txt: it must
    # record ANT_MODULE_DIR as the nonexistent path, not as a real one.
    if ($Backend -ne 'sdk_ant' -and $Only -eq '*') {
        $board = 'adafruit_feather_nrf52840/nrf52840/uf2'
        $out = Join-Path $repo 'build\independence'
        $ghost = 'C:/nonexistent-sdk-ant'
        Write-Host "`n=== build independence  [$board]  [$Backend, ANT_MODULE_DIR=$ghost]" -ForegroundColor Cyan

        $log = Join-Path $env:TEMP 'build_independence.log'
        $indep = @("-DANT_RADIO=$Backend", "-DANT_MODULE_DIR=$ghost")
        if ($Backend -eq 'core') { $indep += "-DRADIANT_BACKEND=$RadiantBackend" }
        west -z $zephyr build -s $repo -d $out -b $board -p always -- @indep > $log
        if ($LASTEXITCODE -ne 0) {
            Get-Content $log -Tail 30
            throw "the $Backend backend does not build without sdk-ant present; full log at $log"
        }

        $cache = Get-Content (Join-Path $out 'ant_dongle\CMakeCache.txt')
        if (-not ($cache | Where-Object { $_ -match "^ANT_MODULE_DIR:[^=]*=$([regex]::Escape($ghost))$" })) {
            throw "the independence build did not receive ANT_MODULE_DIR=$ghost; it proved nothing"
        }

        $cfg = Get-Content (Join-Path $out 'ant_dongle\zephyr\.config')
        if (-not ($cfg | Where-Object { $_ -eq $backendSymbol })) {
            throw "the independence build did not select $Backend"
        }

        Write-Host "  ok: $Backend builds with no sdk-ant reachable"
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
