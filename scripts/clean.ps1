# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Delete build outputs, leaving bench records alone.

.DESCRIPTION
    West build trees accumulate one directory per experiment and are never
    pruned by anything. On this bench they reached 5.48 GB across 135,785 files
    in about 190 directories, at which point a plain recursive listing from the
    repository root took over two minutes - a tax on every grep, every tool and
    every agent that touches the tree. Nothing here is tracked by git; all of it
    rebuilds.

    WHAT THIS DELIBERATELY DOES NOT TOUCH, and the reason it takes an explicit
    list rather than just removing build*/:

      bench-logs/   Console captures and decoded traces pulled off boards.
                    These are PRIMARY RECORDS - bench-logs/p4/ holds the 149
                    files behind docs/radiant-bridge.md section 7.4.2, and
                    bench-logs/captures/zwift1_decoded.txt is the decoded USB
                    capture that established how Zwift actually discovers
                    sensors. They lived under build/ until 2026-08-14 and were
                    one careless `Remove-Item build -Recurse` from being gone.
                    They are perishable: re-running a bench sitting does not
                    reproduce them, it produces different ones.

      archive/      Preserved artefacts, budgeted and CI-enforced at 10 MB.
                    See docs/preservation.md.

    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.

.PARAMETER IncludeDist
    Also remove dist/. Off by default because dist/ is what a release was cut
    from and is cheap to keep; build_all.ps1 rewrites it wholesale anyway.

.EXAMPLE
    .\scripts\clean.ps1 -WhatIf
    .\scripts\clean.ps1
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$IncludeDist
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# Explicit list, not a wildcard sweep. A wildcard here is how bench-logs/ gets
# deleted the first time somebody adds a directory whose name starts "build".
$targets = @()
$targets += Get-ChildItem -Path $repo -Directory -Filter 'build*' -ErrorAction SilentlyContinue
$targets += Get-ChildItem -Path (Join-Path $repo 'apps') -Directory -Recurse -Filter 'build*' -ErrorAction SilentlyContinue
$targets += Get-ChildItem -Path (Join-Path $repo 'radiant') -Directory -Recurse -Filter 'build*' -ErrorAction SilentlyContinue
if ($IncludeDist) {
    $d = Join-Path $repo 'dist'
    if (Test-Path $d) { $targets += Get-Item $d }
}

# Belt and braces: refuse anything under bench-logs/ or archive/ even if a
# future rename makes one of the patterns above match it.
$protected = @('bench-logs', 'archive')
$targets = $targets | Where-Object {
    $rel = $_.FullName.Substring($repo.Length).TrimStart('\', '/')
    $first = ($rel -split '[\\/]')[0]
    $protected -notcontains $first
} | Sort-Object FullName -Unique

if ($targets.Count -eq 0) {
    Write-Host 'clean: nothing to remove.'
    return
}

$totalBytes = 0
foreach ($t in $targets) {
    $sum = (Get-ChildItem $t.FullName -Recurse -File -ErrorAction SilentlyContinue |
            Measure-Object Length -Sum).Sum
    if ($null -eq $sum) { $sum = 0 }
    $totalBytes += $sum
    Write-Host ('  {0,10:N1} MB  {1}' -f ($sum / 1MB), $t.FullName.Substring($repo.Length + 1))
}
Write-Host ('clean: {0:N1} MB in {1} directories' -f ($totalBytes / 1MB), $targets.Count)

foreach ($t in $targets) {
    if ($PSCmdlet.ShouldProcess($t.FullName, 'Remove-Item -Recurse -Force')) {
        Remove-Item $t.FullName -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue
        # A locked file (an editor holding a .ninja_log, most often) survives the
        # first pass and leaves an empty skeleton. One retry clears it; reporting
        # the leftover matters more than the retry, because a directory that
        # cannot be removed is usually a build still running.
        if (Test-Path $t.FullName) {
            Remove-Item $t.FullName -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue
        }
        if (Test-Path $t.FullName) {
            Write-Warning ('clean: could not fully remove {0} - a file is locked.' -f $t.FullName)
        }
    }
}
