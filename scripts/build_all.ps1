<#
.SYNOPSIS
    Build every target and collect the release artifacts into dist\.

.DESCRIPTION
    Mirrors the matrix in .github\workflows\build.yml, including the two checks
    it makes after each build, so a local dist\ matches what CI would publish
    rather than approximately resembling it.

    Those checks exist because both failures are silent. Partition Manager
    decides where the application links and does not complain when that
    disagrees with CONFIG_FLASH_LOAD_OFFSET - the image just installs cleanly
    and boots into nothing. And the transport is defaulted from devicetree, so
    a board whose USB node moved would quietly compile a different one and
    still succeed.

    ASCII only, deliberately: Windows PowerShell 5.1 reads .ps1 files as ANSI
    unless they carry a BOM, so non-ASCII characters here become parse errors.

.PARAMETER NcsVersion
    NCS version to build against. Must be the one sdk-ant pairs with.

.PARAMETER Only
    Build just the targets whose artifact name matches this wildcard.

.PARAMETER SkipDfu
    Do not package the dongle DFU zip (which needs nrfutil).

.EXAMPLE
    .\scripts\build_all.ps1
    .\scripts\build_all.ps1 -Only "*promicro*"
#>
[CmdletBinding()]
param(
    [string]$NcsVersion = 'v3.2.4',
    [string]$Only = '*',
    [switch]$SkipDfu
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent
$zephyr = "C:\ncs\$NcsVersion\zephyr"
$dist = Join-Path $repo 'dist'

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

if (-not (Get-Command west -ErrorAction SilentlyContinue)) {
    throw "west is not on PATH. Dot-source the environment first:`n" +
          "  . .\scripts\env.ps1 -NcsVersion $NcsVersion"
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
        Write-Host "`n=== $($t.artifact)  [$($t.board)]" -ForegroundColor Cyan

        $extra = @()
        if ($t.conf) { $extra = @('--', "-DEXTRA_CONF_FILE=$($t.conf)") }

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

        Write-Host ("  ok: links at 0x{0:x}, {1} transport" -f $offset, $got)

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
} finally {
    Pop-Location
}

Write-Host "`n=== dist\ ===" -ForegroundColor Cyan
Get-ChildItem $dist | Sort-Object Name |
    Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize

$ship = $built | Where-Object { $_.release } | ForEach-Object { $_.artifact }
if ($ship) {
    Write-Host "Release artifacts: $($ship -join ', ')"
    Write-Host "The rest are test/dev images and are not attached to releases."
}
