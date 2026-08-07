<#
.SYNOPSIS
    Put an nRF Connect SDK toolchain on PATH for the current PowerShell session.

.DESCRIPTION
    The Nordic toolchain bundles carry their own git.exe, west.exe, cmake, ninja
    and the Zephyr SDK compilers. None of them are on the system PATH, so a bare
    `west build` fails until this runs. The bundle ships an environment.json
    describing exactly what to set; this script applies it, session-scoped only.

    Nothing is written to the registry and ZEPHYR_BASE is deliberately left
    alone - always pass `west -z <ncs>\zephyr` so a stray ZEPHYR_BASE cannot
    silently target the wrong SDK.

    ASCII only, deliberately: Windows PowerShell 5.1 reads .ps1 files as ANSI
    unless they carry a BOM, so non-ASCII characters here become parse errors.

.PARAMETER NcsVersion
    NCS version to select the toolchain for, e.g. v3.4.0 or v3.2.4.

.PARAMETER Bundle
    Toolchain bundle id (directory name under <NcsRoot>\toolchains). Overrides
    the lookup by NcsVersion.

.PARAMETER NcsRoot
    Root of the Nordic install. Defaults to C:\ncs.

.EXAMPLE
    . .\scripts\env.ps1 -NcsVersion v3.4.0

.NOTES
    Dot-source it (`. .\scripts\env.ps1`); running it as a child process would
    discard the environment on exit.
#>
[CmdletBinding()]
param(
    [string]$NcsVersion = 'v3.4.0',
    [string]$Bundle,
    [string]$NcsRoot = 'C:\ncs'
)

$ErrorActionPreference = 'Stop'

$toolchainsRoot = Join-Path $NcsRoot 'toolchains'
if (-not (Test-Path $toolchainsRoot)) {
    throw "No toolchains directory at $toolchainsRoot. Install an SDK via the nRF Connect extension or nrfutil first."
}

if (-not $Bundle) {
    $manifest = Join-Path $toolchainsRoot 'toolchains.json'
    if (-not (Test-Path $manifest)) {
        throw "No $manifest; pass -Bundle <id> explicitly."
    }

    $entries = (Get-Content $manifest -Raw | ConvertFrom-Json).toolchains
    $match = $entries | Where-Object { $_.ncs_versions -contains $NcsVersion }

    if (-not $match) {
        $known = ($entries | ForEach-Object { "$($_.identifier.bundle_id) -> $($_.ncs_versions -join ',')" }) -join "`n  "
        throw "No installed toolchain for $NcsVersion. Installed:`n  $known"
    }

    $Bundle = @($match)[0].identifier.bundle_id
}

$bundleDir = Join-Path $toolchainsRoot $Bundle
$envJson = Join-Path $bundleDir 'environment.json'
if (-not (Test-Path $envJson)) {
    throw "No environment.json in $bundleDir. Is '$Bundle' a valid bundle id?"
}

foreach ($entry in (Get-Content $envJson -Raw | ConvertFrom-Json).env_vars) {
    $value = switch ($entry.type) {
        'string'         { $entry.value }
        'relative_paths' { ($entry.values | ForEach-Object { Join-Path $bundleDir $_ }) -join ';' }
        default          { throw "Unhandled environment.json entry type '$($entry.type)'" }
    }

    if ($entry.existing_value_treatment -eq 'prepend_to') {
        $existing = [Environment]::GetEnvironmentVariable($entry.key)
        if ($existing) {
            $value = $value + ';' + $existing
        }
    }

    Set-Item -Path ('Env:' + $entry.key) -Value $value
}

$zephyrBase = Join-Path (Join-Path $NcsRoot $NcsVersion) 'zephyr'
Write-Host "Toolchain $Bundle on PATH for this session (NCS $NcsVersion)."
Write-Host "west: $((Get-Command west -ErrorAction SilentlyContinue).Source)"
if (Test-Path $zephyrBase) {
    # `west build` is an extension command discovered through the workspace
    # manifest, so west must be invoked with its cwd inside the workspace even
    # though -s/-d/-z all point elsewhere.
    Write-Host "Build from inside the workspace:"
    Write-Host "  Push-Location $(Join-Path $NcsRoot $NcsVersion)"
    Write-Host "  west -z $zephyrBase build -s <app> -d <app>\build\stage1 -b adafruit_feather_nrf52840/nrf52840/uf2 -p always -- `"-DEXTRA_CONF_FILE=stub.conf`""
    Write-Host "  Pop-Location"
} else {
    Write-Warning "NCS $NcsVersion is not installed at $zephyrBase (only the toolchain is)."
}
