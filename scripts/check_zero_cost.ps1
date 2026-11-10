# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Prove a plain dongle build is identical to a base ref: the local
    release.yml zero-cost job, for every plain board.

.DESCRIPTION
    Builds each row from HEAD and from a base ref (a detached worktree under
    build\zc_base) and compares allocated section sizes (`size -A`) and the
    defined-symbol set (`nm --defined-only`), the same two checks release.yml's
    zero-cost job makes. Exit 0 only if every row matched.

    Identical sections and symbols are the proof that the change costs a plain
    build nothing: there is no difference for the radio's timing to live in.

    THE TI ROWS ARE NOT OPTIONAL. build_all.ps1 skips them with a message when
    hal_ti is missing; here a missing hal_ti FAILS the run, so a machine without
    it cannot report "identical" having checked nothing. TI is build-only on
    this bench: this script is the only proof TI code was not changed, and it
    says nothing about TI on air.

    Each row's .config is read back: the expected radio HAL must be selected,
    the MPSL gate must be OFF on the nRF rows, and neither of ADR 0018's
    bench symbols may be compiled in. Those read-backs are what would otherwise
    turn a broken row into a passing comparison.

.PARAMETER BaseRef
    What to compare against. Default: the merge base of HEAD with master.

.PARAMETER Only
    Wildcard over row names (feather, dongle, promicro, net5340, l15, lm20, ti,
    ti_gate).

.PARAMETER SkipBuild
    Compare the build directories already on disk. For iterating on this script.

.PARAMETER KeepBase
    Leave build\zc_base in place afterwards.

.EXAMPLE
    . .\scripts\env.ps1 -NcsVersion v3.4.0
    .\scripts\check_zero_cost.ps1 -BaseRef 42475c6
#>
[CmdletBinding()]
param(
    [string]$BaseRef = '',
    [string]$NcsVersion = 'v3.4.0',
    [string]$Only = '*',
    [switch]$SkipBuild,
    [switch]$KeepBase
)

$ErrorActionPreference = 'Stop'

# Any uncaught error is a failed check, including when this runs from another
# script: exit 1 so $LASTEXITCODE tells the caller.
trap { Write-Host "FAILED: $_" -ForegroundColor Red; exit 1 }

function Invoke-Native([string]$Exe, [string[]]$NativeArgs) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Exe @NativeArgs 2>&1 | ForEach-Object { "$_" }
    } finally {
        $ErrorActionPreference = $prev
    }
}

$repo = Split-Path $PSScriptRoot -Parent
$zephyr = "C:\ncs\$NcsVersion\zephyr"
$workspace = "C:\ncs\$NcsVersion"
$halTi = "C:\ncs\$NcsVersion\modules\hal\ti"

# Same rows as release.yml's matrix plus the local TI rows: every board a plain
# apps/dongle image ships or is built for, plus the two TI arms in build_all.ps1.
$rows = @(
    @{ name='feather';  board='adafruit_feather_nrf52840/nrf52840/uf2'; app='apps\dongle';    radiant='nrf';    conf=$null }
    @{ name='dongle';   board='nrf52840dongle/nrf52840';                app='apps\dongle';    radiant='nrf';    conf=$null }
    @{ name='promicro'; board='promicro_nrf52840/nrf52840/uf2';         app='apps\dongle';    radiant='nrf';    conf=$null }
    @{ name='net5340';  board='nrf5340dk/nrf5340/cpunet';               app='apps\dongle';    radiant='nrf';    conf=$null }
    @{ name='l15';      board='nrf54l15dk/nrf54l15/cpuapp';             app='apps\dongle';    radiant='nrf';    conf=$null }
    @{ name='lm20';     board='nrf54lm20dk/nrf54lm20a/cpuapp';          app='apps\dongle';    radiant='nrf';    conf=$null }
    @{ name='ti';       board='cc26x2r1_launchxl';                      app='apps\dongle_ti'; radiant='cc26xx'; conf=$null;           ti=$true }
    @{ name='ti_gate';  board='cc26x2r1_launchxl';                      app='apps\dongle_ti'; radiant='cc26xx'; conf='ti_gate.conf';  ti=$true }
)

$rows = @($rows | Where-Object { $_.name -like $Only })
if (-not $rows) { throw "no row matches -Only '$Only'" }

if ($rows | Where-Object { $_.ti }) {
    # A SKIP WOULD BE A PASS HERE: the TI rows have no other proof of no change.
    if (-not (Test-Path (Join-Path $halTi 'zephyr\module.yml'))) {
        throw "hal_ti module is not at $halTi - the TI rows cannot be built. " +
              "Run scripts\fetch_hal_ti.ps1; this check does not skip them."
    }
}
$halTiCmake = $halTi -replace '\\', '/'

if (-not (Get-Command west -ErrorAction SilentlyContinue)) {
    throw "west is not on PATH. Dot-source scripts\env.ps1 -NcsVersion $NcsVersion first."
}

function Get-Binutil([string]$Name) {
    $found = Get-Command "arm-zephyr-eabi-$Name.exe" -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    $candidates = @(Get-ChildItem "C:\ncs\toolchains" -Recurse -Filter "arm-zephyr-eabi-$Name.exe" -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($candidates) { return $candidates[0].FullName }
    throw "cannot find arm-zephyr-eabi-$Name.exe; run from an env.ps1 session"
}

$sizeTool = Get-Binutil 'size'
$nmTool = Get-Binutil 'nm'
$buildRoot = Join-Path $repo 'build\zc'
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

$baseTree = Join-Path $repo 'build\zc_base'
$haveBase = $false
if (-not $SkipBuild) {
    if (-not $BaseRef) {
        $BaseRef = @(Invoke-Native 'git' @('-C', $repo, 'merge-base', 'HEAD', 'master'))[-1].Trim()
        if ($LASTEXITCODE -ne 0) { throw "git merge-base failed" }
    }
    $baseSha = @(Invoke-Native 'git' @('-C', $repo, 'rev-parse', "$BaseRef^{commit}"))[-1].Trim()
    if ($LASTEXITCODE -ne 0) { throw "base ref '$BaseRef' does not resolve" }
    Write-Host "base: $BaseRef ($baseSha)" -ForegroundColor Cyan

    if (Test-Path $baseTree) {
        $head = @(Invoke-Native 'git' @('-C', $baseTree, 'rev-parse', 'HEAD'))[-1].Trim()
        if ($head -ne $baseSha) {
            throw "$baseTree is at $head, not $baseSha. Remove it (git worktree remove) first."
        }
        $status = Invoke-Native 'git' @('-C', $baseTree, 'status', '--porcelain')
        if ($status) { throw "$baseTree is not clean; refusing to build from it." }
    } else {
        Invoke-Native 'git' @('-C', $repo, 'worktree', 'add', '--detach', $baseTree, $baseSha) | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git worktree add failed for $baseTree" }
    }
    $haveBase = $true
}

function Invoke-RowBuild([hashtable]$Row, [string]$TreeRoot, [string]$Out) {
    $app = Join-Path $TreeRoot $Row.app
    $extra = @('--', '-DANT_RADIO=core', "-DRADIANT_BACKEND=$($Row.radiant)")
    if ($Row.ti) { $extra += "-DEXTRA_ZEPHYR_MODULES=$halTiCmake" }
    if ($Row.conf) { $extra += "-DEXTRA_CONF_FILE=$($Row.conf)" }

    $log = "$Out.log"
    New-Item -ItemType Directory -Force -Path (Split-Path $Out -Parent) | Out-Null
    Push-Location $workspace
    try {
        $westArgs = @('-z', $zephyr, 'build', '-s', $app, '-d', $Out, '-b', $Row.board, '-p', 'always') + $extra
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        & west @westArgs *> $log
        $ErrorActionPreference = $prev
        if ($LASTEXITCODE -ne 0) { throw "build of $Out failed; log at $log" }
    } finally {
        Pop-Location
    }
}

function Get-RowElf([string]$Out, [hashtable]$Row) {
    $image = Split-Path $Row.app -Leaf
    $elf = Join-Path $Out "$image\zephyr\zephyr.elf"
    if (-not (Test-Path $elf)) { throw "no ELF at $elf" }
    return $elf
}

# Name-to-size pairs, allocated sections only: the comparison in release.yml.
# Non-zero load address is the filter (.debug_*, .comment and .ARM.attributes
# sit at 0); a file name in .debug_line would otherwise make every run differ.
function Get-Sections([string]$Elf) {
    $out = @{}
    foreach ($line in (Invoke-Native $sizeTool @('-A', $Elf))) {
        if ($line -match '^(\S+)\s+(\d+)\s+(\d+)\s*$' -and $Matches[3] -ne '0' -and $Matches[1] -ne 'Total') {
            $out[$Matches[1]] = [int64]$Matches[2]
        }
    }
    return $out
}

function Get-Symbols([string]$Elf) {
    $names = New-Object System.Collections.Generic.HashSet[string]
    foreach ($line in (Invoke-Native $nmTool @('--defined-only', $Elf))) {
        $parts = -split $line
        if ($parts.Count -ge 2) { [void]$names.Add($parts[-1]) }
    }
    return $names
}

function Assert-Config([hashtable]$Row, [string]$Out, [string]$Tree) {
    $cfgPath = Join-Path $Out "$(Split-Path $Row.app -Leaf)\zephyr\.config"
    $cfg = Get-Content $cfgPath
    $want = "CONFIG_RADIANT_BACKEND_$($Row.radiant.ToUpper())=y"
    if (-not ($cfg | Where-Object { $_ -ceq $want })) {
        throw "$($Row.name) ($Tree): expected $want in .config"
    }
    if ($cfg | Where-Object { $_ -match '^CONFIG_RADIANT_BACKEND_NULL=y' }) {
        throw "$($Row.name) ($Tree): null radio backend"
    }
    if ($Row.radiant -eq 'nrf' -and ($cfg | Where-Object { $_ -match '^CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL=y' })) {
        throw "$($Row.name) ($Tree): the MPSL gate is on in a plain row"
    }
    foreach ($sym in @('CONFIG_RADIANT_SEARCH_COVERAGE', 'CONFIG_RADIANT_SEARCH_PHASE_DIAG')) {
        if ($cfg | Where-Object { $_ -match "^$sym=y" }) {
            throw "$($Row.name) ($Tree): $sym is on"
        }
    }
}

$results = @()
$failed = @()
foreach ($r in $rows) {
    Write-Host "`n=== $($r.name)  [$($r.board)]  [$($r.app)$(if ($r.conf) { " + $($r.conf)" })]" -ForegroundColor Cyan
    $hOut = Join-Path $buildRoot "$($r.name)\head"
    $bOut = Join-Path $buildRoot "$($r.name)\base"

    if (-not $SkipBuild) {
        Invoke-RowBuild $r $repo $hOut
        Invoke-RowBuild $r $baseTree $bOut
    }
    Assert-Config $r $hOut 'head'
    Assert-Config $r $bOut 'base'

    $hElf = Get-RowElf $hOut $r
    $bElf = Get-RowElf $bOut $r
    $hSec = Get-Sections $hElf
    $bSec = Get-Sections $bElf
    $hSym = Get-Symbols $hElf
    $bSym = Get-Symbols $bElf
    if ($hSec.Count -eq 0 -or $hSym.Count -eq 0) {
        throw "$($r.name): empty comparison (no sections or no symbols)"
    }

    $secDiff = @($hSec.Keys + $bSec.Keys | Sort-Object -Unique | Where-Object { $hSec[$_] -ne $bSec[$_] })
    $symOnlyHead = @($hSym | Where-Object { -not $bSym.Contains($_) })
    $symOnlyBase = @($bSym | Where-Object { -not $hSym.Contains($_) })

    $ok = ($secDiff.Count -eq 0 -and $symOnlyHead.Count -eq 0 -and $symOnlyBase.Count -eq 0)
    $hBin = Join-Path (Split-Path $hElf -Parent) 'zephyr.bin'
    $bBin = Join-Path (Split-Path $bElf -Parent) 'zephyr.bin'
    $binSame = $false
    if ((Test-Path $hBin) -and (Test-Path $bBin)) {
        $binSame = ((Get-FileHash $hBin -Algorithm SHA256).Hash -eq (Get-FileHash $bBin -Algorithm SHA256).Hash)
    }

    $results += [pscustomobject]@{
        Row      = $r.name
        Sections = if ($secDiff.Count -eq 0) { "same ($($hSec.Count))" } else { "DIFF: $($secDiff -join ',')" }
        Symbols  = if ($symOnlyHead.Count + $symOnlyBase.Count -eq 0) { "same ($($hSym.Count))" } else { "DIFF +$($symOnlyHead.Count) -$($symOnlyBase.Count)" }
        ZephyrBin = if ($binSame) { 'identical' } else { 'differs' }
    }
    if (-not $ok) {
        $failed += $r.name
        foreach ($s in $secDiff) { Write-Host "  $s : base $($bSec[$s]) head $($hSec[$s])" -ForegroundColor Red }
        foreach ($s in $symOnlyHead) { Write-Host "  + $s" -ForegroundColor Red }
        foreach ($s in $symOnlyBase) { Write-Host "  - $s" -ForegroundColor Red }
    }
}

if ($haveBase -and -not $KeepBase -and -not $failed) {
    Invoke-Native 'git' @('-C', $repo, 'worktree', 'remove', '--force', $baseTree) | Out-Null
}

$results | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
if ($failed) {
    Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "PASS: every row identical against the base" -ForegroundColor Green
exit 0
