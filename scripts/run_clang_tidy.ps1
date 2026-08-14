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

$sourceDbs = @('ztest_hw_l15\tests', 'p15_gate\dongle_thread', 'ti_cc26xx\dongle_ti') |
    ForEach-Object { Join-Path $repoRoot "build\$_\compile_commands.json" }

$merged = @{}
foreach ($db in $sourceDbs) {
    if (-not (Test-Path $db)) { Write-Warning "Missing $db, skipping"; continue }
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
& $clangTidy -p $mergedDbDir @targets

if ($uncovered) {
    Write-Warning "Not covered by any of the three reference builds (skipped):"
    $uncovered | ForEach-Object { Write-Warning "  $($_.FullName.Substring($repoRoot.Length + 1))" }
}
