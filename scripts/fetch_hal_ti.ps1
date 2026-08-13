# SPDX-License-Identifier: Apache-2.0

<#
.SYNOPSIS
    Place the hal_ti Zephyr module in the NCS workspace, at the revision the
    workspace's own Zephyr already pins.

.DESCRIPTION
    NCS cannot fetch this module and `west update` will never produce it, which
    is why a script exists rather than a line in the README.

    The mechanism: nrf/west.yml imports zephyr's manifest through a
    `name-allowlist`, and hal_ti is not on that list, so the project is dropped
    from the *resolved* manifest. It is therefore not "a project west chose not
    to clone" - it is a project west has never heard of, and
    `manifest.project-filter` cannot re-add one the resolved manifest does not
    define. The revision below is nonetheless authoritative: it is read from
    <ncs>\zephyr\west.yml, which still carries the pin, so this script never
    invents a version.

    The result is dropped at <ncs>\<version>\modules\hal\ti - beside the other
    HALs the workspace already carries, exactly where west would
    have put it, so that -DEXTRA_ZEPHYR_MODULES=<that path> is the only build
    flag ever needed and nothing has to move if NCS starts shipping it.

    A single-revision fetch rather than a clone: hal_ti carries the SimpleLink
    SDK sources and its full history is a multi-GB download for one commit.

    ASCII only, deliberately: Windows PowerShell 5.1 reads .ps1 files as ANSI
    unless they carry a BOM, so non-ASCII characters here become parse errors.

.PARAMETER NcsVersion
    Which NCS install to place the module in. Its zephyr\west.yml supplies the
    revision.

.PARAMETER NcsRoot
    Root of the Nordic install. Defaults to C:\ncs.

.PARAMETER Revision
    Override the pin. Only for bisecting hal_ti itself; the default is read
    from the workspace and is what every build should use.

.PARAMETER Force
    Re-fetch even if the destination already holds the right revision.

.EXAMPLE
    . .\scripts\env.ps1 -NcsVersion v3.4.0
    .\scripts\fetch_hal_ti.ps1

.NOTES
    Needs git on PATH; the toolchain bundle carries one, so dot-source
    scripts\env.ps1 first.
#>
[CmdletBinding()]
param(
    [string]$NcsVersion = 'v3.4.0',
    [string]$NcsRoot = 'C:\ncs',
    [string]$Revision,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$zephyrManifest = Join-Path $NcsRoot "$NcsVersion\zephyr\west.yml"
$dest = Join-Path $NcsRoot "$NcsVersion\modules\hal\ti"
$url = 'https://github.com/zephyrproject-rtos/hal_ti'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "git is not on PATH. Dot-source the environment first:`n" +
          "  . .\scripts\env.ps1 -NcsVersion $NcsVersion"
}

# Read the pin out of the workspace rather than hardcoding it here. Two copies
# of a revision is one copy too many, and the failure of a stale one is a build
# against a HAL the rest of the SDK was not tested with.
if (-not $Revision) {
    if (-not (Test-Path $zephyrManifest)) {
        throw "No $zephyrManifest. Is NCS $NcsVersion installed at $NcsRoot?"
    }

    $lines = Get-Content $zephyrManifest
    $inProject = $false
    foreach ($line in $lines) {
        if ($line -match '^\s*-\s*name:\s*hal_ti\s*$') { $inProject = $true; continue }
        if ($inProject) {
            if ($line -match '^\s*revision:\s*([0-9a-fA-F]{40}|\S+)\s*$') {
                $Revision = $Matches[1]
                break
            }
            # A new list entry means the hal_ti block ended without a revision.
            if ($line -match '^\s*-\s*name:') { break }
        }
    }

    if (-not $Revision) {
        throw "No hal_ti revision in $zephyrManifest. Pass -Revision <sha> explicitly."
    }
    Write-Host "hal_ti revision $Revision (from $zephyrManifest)"
}

$head = $null
if (Test-Path (Join-Path $dest '.git')) {
    $head = (& git -C $dest rev-parse HEAD 2>$null)
}

if ($head -eq $Revision -and -not $Force) {
    Write-Host "Already at ${Revision}: $dest"
    Write-Host "Build with -DEXTRA_ZEPHYR_MODULES=$($dest -replace '\\', '/')"
    return
}

New-Item -ItemType Directory -Force -Path $dest | Out-Null

if (-not (Test-Path (Join-Path $dest '.git'))) {
    & git -C $dest init --quiet
    if ($LASTEXITCODE -ne 0) { throw "git init failed in $dest" }
    & git -C $dest remote add origin $url
    if ($LASTEXITCODE -ne 0) { throw "git remote add failed in $dest" }
}

# --depth 1 of the exact SHA. GitHub allows fetching an unadvertised but
# reachable commit, so this pulls one revision of a repository whose full
# history is several GB of vendored SimpleLink sources.
Write-Host "Fetching $Revision from $url ..."
& git -C $dest fetch --depth 1 origin $Revision
if ($LASTEXITCODE -ne 0) {
    # The fallback is a blobless clone-shaped fetch of the default branch,
    # which works if the server refuses SHA fetches. Still not a full clone.
    Write-Warning "Single-revision fetch failed; retrying with a blobless fetch of all branches."
    & git -C $dest fetch --filter=blob:none origin
    if ($LASTEXITCODE -ne 0) { throw "could not fetch hal_ti from $url" }
}

& git -C $dest checkout --quiet --detach $Revision
if ($LASTEXITCODE -ne 0) { throw "could not check out $Revision in $dest" }

$moduleYml = Join-Path $dest 'zephyr\module.yml'
if (-not (Test-Path $moduleYml)) {
    throw "Fetched $Revision but there is no zephyr\module.yml in $dest - that is not a Zephyr module."
}

Write-Host ''
Write-Host "hal_ti at $Revision in $dest"
Write-Host "Build with -DEXTRA_ZEPHYR_MODULES=$($dest -replace '\\', '/')"
