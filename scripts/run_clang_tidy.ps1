# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
  Run clang-tidy over radiant/src using existing build/*/compile_commands.json
  databases (no fresh `west build` needed).

.DESCRIPTION
  No single build config compiles every radiant/src file (backend selection
  is compile-time), so this merges the compile_commands.json from three builds
  that together cover the nrf, cc26xx and host-testable (ztest) source sets:
  ztest_hw_l15, p15_gate, ti_cc26xx. Files compiled in none of them are skipped
  and reported at the end.

.PARAMETER Files
  Optional list of specific .c files to check. Defaults to every file covered
  by the merged database.
#>
param(
    [string[]]$Files
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$clangTidy = "C:\Program Files\LLVM\bin\clang-tidy.exe"
if (-not (Test-Path $clangTidy)) {
    $cmd = Get-Command clang-tidy -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "clang-tidy not found. Install LLVM (winget install LLVM.LLVM) or add it to PATH." }
    $clangTidy = $cmd.Source
}

# WHICH BUILD DIRECTORIES, AND WHY A MISSING ONE IS FATAL.
#
# `p15_gate` was named here and is not an arm scripts\build_p4.ps1 has ever
# produced - build_p4.ps1 writes p4ctrl, p4med and p4sed. So the gate database
# was always absent, the Write-Warning below scrolled past, and the run
# degraded to "fewer files checked" while still printing a clean bill. The file
# it silently dropped is radiant_radio_nrf_gate_mpsl.c, which is compiled ONLY
# in a gate build and which holds bugs 22 and 23. A linter that quietly stops
# looking at the most dangerous file in the tree is worse than no linter.
#
# Each entry is a list of candidate paths, tried in order, so a build made by
# either build_p4.ps1 or a hand-run west lands. A group where none of the
# candidates exists is a hard failure with the command that produces one -
# never a warning.
$dbGroups = @(
    @{
        What = 'ztest (host-testable sources)'
        # run_ztest_hw.ps1 builds without sysbuild, so the database is at the
        # top of the build directory with no image level under it. The two
        # older paths are kept for a hand-run sysbuild build.
        Paths = @('ztest_hw', 'ztest_hw_l15\tests', 'ztest_hw\tests')
        How = '.\scripts\run_ztest_hw.ps1 -App radiant\tests -NcsVersion v3.4.0'
    }
    @{
        What = 'nrf + MPSL gate (radiant_radio_nrf_gate_mpsl.c lives here and nowhere else)'
        Paths = @('p4ctrl\dongle_thread', 'p4med\dongle_thread', 'p15_gate\dongle_thread')
        How = '.\scripts\build_p4.ps1 -Only p4ctrl -Pristine'
    }
    @{
        What = 'cc26xx'
        Paths = @('ti_launchxl\dongle_ti', 'ti_cc26xx\dongle_ti',
                  'ti_launchxl_gate\dongle_ti')
        How = '.\scripts\fetch_hal_ti.ps1 then .\scripts\build_all.ps1 -Backend core -Only "*launchxl*"'
    }
)

$sourceDbs = @()
$missingGroups = @()
foreach ($g in $dbGroups) {
    $found = $null
    foreach ($p in $g.Paths) {
        $candidate = Join-Path $repoRoot "build\$p\compile_commands.json"
        if (Test-Path $candidate) { $found = $candidate; break }
    }
    if ($found) {
        Write-Host "  db: $($g.What)" -ForegroundColor DarkGray
        Write-Host "      $found" -ForegroundColor DarkGray
        $sourceDbs += $found
    } else {
        $missingGroups += ("  {0}`n    tried: {1}`n    build one with: {2}" -f
                           $g.What, ($g.Paths -join ', '), $g.How)
    }
}
if ($missingGroups.Count) {
    throw ("No compile database for {0} of the {1} source sets, so those files would be silently skipped:`n{2}" -f
           $missingGroups.Count, $dbGroups.Count, ($missingGroups -join "`n"))
}

$merged = @{}
foreach ($db in $sourceDbs) {
    $entries = Get-Content $db -Raw | ConvertFrom-Json
    foreach ($e in $entries) {
        if ($e.file -match 'radiant[\\/]src') {
            # GCC-only flags clang-tidy's driver doesn't recognize; harmless to drop for analysis.
            $e.command = $e.command -replace '-fno-printf-return-value|-fno-reorder-functions|-mfp16-format=ieee|-mtp=soft', ''
            $merged[$e.file] = $e
        }
    }
}

if ($merged.Count -eq 0) { throw "No radiant/src entries found in any source compile_commands.json" }

$mergedDbDir = Join-Path $repoRoot 'build\clang_tidy_db'
New-Item -ItemType Directory -Force -Path $mergedDbDir | Out-Null
$merged.Values | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $mergedDbDir 'compile_commands.json')

$targets = @(if ($Files) { $Files | ForEach-Object { (Resolve-Path $_).Path } } else { $merged.Keys })

$uncovered = Get-ChildItem (Join-Path $repoRoot 'radiant\src') -Recurse -Filter *.c |
    Where-Object { -not $merged.ContainsKey($_.FullName.Replace('\', '/')) -and -not $merged.ContainsKey($_.FullName) }

Write-Host "Running clang-tidy on $($targets.Count) file(s) via $($mergedDbDir)"

# ErrorActionPreference is dropped to Continue across this call and nowhere
# else. clang-tidy writes its "[n/N] Processing file ..." progress to stderr,
# and Windows PowerShell 5.1 wraps a native command's stderr in ErrorRecords -
# so with 'Stop' in force the FIRST progress line aborts the script and reads
# exactly like a clang-tidy failure on file 1 of 44. Same trap, same fix, as
# the west calls in scripts\build_all.ps1 and scripts\build_p4.ps1.
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    & $clangTidy -p $mergedDbDir @targets
    $tidyRc = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $prev
}

if ($uncovered) {
    Write-Warning "Not covered by any of the three reference builds (skipped):"
    $uncovered | ForEach-Object { Write-Warning "  $($_.FullName.Substring($repoRoot.Length + 1))" }

    # Named explicitly rather than left in the list. This is the file the
    # stale p15_gate path used to drop in silence, and it is the one file in
    # radiant/src worth failing over: the DPPI/NVIC borrow it implements is the
    # most expensive thing in this tree to regress, and bugs 22 and 23 both
    # lived in it.
    $gateFile = $uncovered | Where-Object { $_.Name -eq 'radiant_radio_nrf_gate_mpsl.c' }
    if ($gateFile) {
        throw ("radiant_radio_nrf_gate_mpsl.c was not analysed. It compiles only in " +
               "a gate build, so the gate database above resolved to something that " +
               "is not one. Build an arm with .\scripts\build_p4.ps1 -Only p4ctrl -Pristine " +
               "and re-run.")
    }
}

exit $tidyRc
