# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Build the three P4 coexistence images, and refuse to hand back one that
    silently fell back to the null backend.

.DESCRIPTION
    The three arms of the P4 measurement differ only in their EXTRA_CONF_FILE:

        p4ctrl  gate.conf                 arbiter present, no second stack
        p4med   thread.conf               OpenThread MED - always on, worst case
        p4sed   thread.conf;thread_sed.conf   the intended role

    BOTH -DANT_RADIO=core AND -DRADIANT_BACKEND=nrf ARE PASSED EXPLICITLY, and
    that stays true even though nrf is now the CMake default for every
    application. This project has been bitten three times here: RADIANT_BACKEND
    used to default to the inert radio, and - the reason an explicit flag still
    is not enough on its own - once a Zephyr SoC symbol rename made
    `-DRADIANT_BACKEND=nrf` itself build the null backend on v3.4.0 while the
    command line looked perfectly correct. A null
    backend does not fail - it answers every serial command, opens its channel,
    and reports zero packets, which is indistinguishable at the console from
    every real failure this phase is chasing.

    So the .config is READ BACK after every build and the script throws if
    CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL is not y. Checking the command
    line is not checking the build.

    ASCII only, deliberately: Windows PowerShell 5.1 reads .ps1 files as ANSI
    unless they carry a BOM.

.EXAMPLE
    .\scripts\build_p4.ps1
    .\scripts\build_p4.ps1 -Only p4sed -Pristine
#>
[CmdletBinding()]
param(
    [ValidateSet('p4ctrl', 'p4med', 'p4sed', 'all')]
    [string]$Only = 'all',
    [switch]$Pristine,
    [string]$NcsVersion = 'v3.4.0',
    [string]$NcsRoot = 'C:\ncs',
    [string]$Board = 'nrf54l15dk/nrf54l15/cpuapp',

    # Extra -D arguments, appended AFTER the three mandatory ones so they can
    # override a default but never the backend selection - the read-back below
    # still refuses the image if they did. Used to build an A/B pair that differs
    # in exactly one Kconfig symbol.
    # Use -Pristine with it: a changed -D on an existing build directory is
    # exactly the class of silent stale-config failure this script exists for.
    [string[]]$ExtraCmake = @()
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$dongleThreadApp = Join-Path $repo 'apps\dongle_thread'

$arms = @(
    @{ Name = 'p4ctrl'; Conf = 'gate.conf' }
    @{ Name = 'p4med';  Conf = 'thread.conf' }
    @{ Name = 'p4sed';  Conf = 'thread.conf;thread_sed.conf' }
)
if ($Only -ne 'all') {
    $arms = $arms | Where-Object { $_.Name -eq $Only }
}

. (Join-Path $PSScriptRoot 'env.ps1') -NcsVersion $NcsVersion -NcsRoot $NcsRoot

$zephyr = Join-Path (Join-Path $NcsRoot $NcsVersion) 'zephyr'
$failed = @()

foreach ($arm in $arms) {
    $dir = Join-Path $repo "build\$($arm.Name)"
    $log = Join-Path $repo "build\$($arm.Name)-build.log"

    Write-Host ""
    Write-Host "=== $($arm.Name)  EXTRA_CONF_FILE=$($arm.Conf) ===" -ForegroundColor Cyan

    $buildArgs = @('-z', $zephyr, 'build', '-s', $dongleThreadApp, '-d', $dir, '-b', $Board)
    if ($Pristine) { $buildArgs += @('-p', 'always') }
    # CONFIG_RADIANT_SWEEP_DEBUG IS PART OF THE MEASUREMENT, NOT A DEBUG
    # AID, and leaving it off is why the first smoke run produced a board that
    # booted perfectly and reported nothing for 45 s.
    #
    # It is the only source of two things this sitting has to record: the 1 Hz
    # gate dump - where grant_end_calls, late_disarm, overstayed and
    # invalid_return come from, which is the whole scoring rule for the soak -
    # and the sweep set rate, which is what [gates.coexistence]'s
    # min_sweep_rate_ratio reads. Without it an arm is unscoreable after the
    # fact and the run cannot be repeated, because the board has moved on.
    #
    # Passed on the command line rather than added to the three .conf files
    # because it is a property of THIS sitting, not of the images: a shipping
    # thread.conf build should not carry a once-a-second log line.
    $buildArgs += @('--', '-DANT_RADIO=core', '-DRADIANT_BACKEND=nrf',
                    '-DCONFIG_RADIANT_SWEEP_DEBUG=y',
                    "-DEXTRA_CONF_FILE=$($arm.Conf)")
    if ($ExtraCmake.Count) { $buildArgs += $ExtraCmake }

    # ErrorActionPreference is dropped to Continue across the west call and
    # nowhere else. west writes its ordinary progress to stderr, and Windows
    # PowerShell 5.1 wraps a native command's stderr in ErrorRecords - so with
    # 'Stop' in force the FIRST progress line aborts the script and reads
    # exactly like a build failure. $LASTEXITCODE is the only trustworthy
    # verdict here, which is why it is what is tested.
    Push-Location (Join-Path $NcsRoot $NcsVersion)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & west @buildArgs > $log 2>&1
        $rc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
        Pop-Location
    }

    if ($rc -ne 0) {
        Write-Host "  BUILD FAILED (exit $rc) - see $log" -ForegroundColor Red
        $failed += $arm.Name
        continue
    }

    # THE READ-BACK. See the .DESCRIPTION: this is the check, not the -D flags.
    #
    # Two layouts, because sysbuild is not always in play: with it the image
    # lands under <build>\dongle_thread\zephyr (the image name is APP_DIR's own
    # basename, not project()), without it directly under <build>\zephyr. Both
    # are tried rather than one assumed, for the reason apps/dongle's own CI
    # already learned at .github/workflows/release.yml - a hard-coded path that
    # stops existing turns this check into "file not found", and a check that
    # cannot run is not a check that passed.
    $cfg = $null
    foreach ($candidate in @((Join-Path $dir 'dongle_thread\zephyr\.config'),
                             (Join-Path $dir 'zephyr\.config'))) {
        if (Test-Path $candidate) { $cfg = $candidate; break }
    }
    if (-not $cfg) {
        Write-Host "  NO .config UNDER $dir (tried both the sysbuild and the plain layout)" -ForegroundColor Red
        $failed += $arm.Name
        continue
    }

    $want = @{
        'CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL' = 'y'
        'CONFIG_RADIANT_BACKEND_NRF'           = 'y'
        'CONFIG_RADIANT_SWEEP_DEBUG'           = 'y'
    }
    $bad = $false
    foreach ($k in $want.Keys) {
        $line = Select-String -Path $cfg -Pattern "^$k=" -SimpleMatch:$false |
                Select-Object -First 1
        $got = if ($line) { ($line.Line -split '=', 2)[1] } else { '<absent>' }
        if ($got -ne $want[$k]) {
            Write-Host "  $k = $got  (wanted $($want[$k]))" -ForegroundColor Red
            $bad = $true
        } else {
            Write-Host "  $k = $got" -ForegroundColor Green
        }
    }
    # Named separately because its absence is the exact silent failure above.
    $null_be = Select-String -Path $cfg -Pattern '^CONFIG_RADIANT_BACKEND_NULL=y'
    if ($null_be) {
        Write-Host "  CONFIG_RADIANT_BACKEND_NULL=y - THIS IMAGE HAS NO RADIO" -ForegroundColor Red
        $bad = $true
    }
    if ($bad) { $failed += $arm.Name; continue }

    $elf = Join-Path (Split-Path $cfg -Parent) 'zephyr.elf'
    Write-Host "  ok: $elf" -ForegroundColor Green
}

Write-Host ""
if ($failed.Count) {
    throw "Unusable: $($failed -join ', '). Do not take a bench reading from these."
}
Write-Host "All requested P4 images built and backend-checked." -ForegroundColor Green
