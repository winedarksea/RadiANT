<#
.SYNOPSIS
    Recover the diagnostic log that the firmware wrote to flash.

.DESCRIPTION
    There is no J-Link on this machine, so CONFIG_USE_SEGGER_RTT compiles but
    nothing can read it. A build with CONFIG_ANT_DONGLE_FLASH_LOG=y (see
    diag.conf) instead commits the log to a reserved flash region, which the
    Adafruit bootloader exposes as part of CURRENT.UF2.

    Double-tap RESET so FTHR840BOOT mounts, then run this. It reassembles the
    UF2 readback, finds the log header and prints the text.

    Note that CURRENT.UF2 only spans flash up to the last page the bootloader
    considers programmed. The firmware writes two copies for that reason; this
    reports which slots were inside the readback and which were not.

    ASCII only: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.

.PARAMETER Uf2Path
    Read a saved UF2 instead of the live bootloader volume.

.PARAMETER Offsets
    Flash offsets to look for the log at. Must match
    CONFIG_ANT_DONGLE_FLASH_LOG_OFFSET / _ALT.

.PARAMETER VolumeLabel
    Bootloader volume label. FTHR840BOOT for the Feather nRF52840 Express.

.PARAMETER TimeoutSeconds
    How long to wait for the volume to appear. 0 means do not wait.

.PARAMETER OutFile
    Also write the recovered text here.

.EXAMPLE
    .\scripts\read_flash_log.ps1 -TimeoutSeconds 30
    .\scripts\read_flash_log.ps1 -Uf2Path .\backup\CURRENT-readback.uf2
#>
[CmdletBinding()]
param(
    [string]$Uf2Path,
    [uint32[]]$Offsets = @(0xe6000, 0x40000),
    [string]$VolumeLabel = 'FTHR840BOOT',
    [int]$TimeoutSeconds = 0,
    [string]$OutFile
)

$ErrorActionPreference = 'Stop'

# Parsed from strings rather than written as 0x... literals: PowerShell types a
# hex literal that fits in 32 bits as Int32, so 0x9E5D5157 comes out negative
# and never matches the unsigned value BitConverter returns.
$UF2_MAGIC_START0 = [Convert]::ToUInt32('0A324655', 16)
$UF2_MAGIC_START1 = [Convert]::ToUInt32('9E5D5157', 16)
$UF2_MAGIC_END    = [Convert]::ToUInt32('0AB16F30', 16)
$DIAG_MAGIC       = [Convert]::ToUInt32('474C4441', 16)   # 'ADLG'

function Get-BootVolume {
    param([string]$Label)
    Get-Volume -FileSystemLabel $Label -ErrorAction SilentlyContinue |
        Where-Object { $_.DriveLetter }
}

if (-not $Uf2Path) {
    $volume = Get-BootVolume -Label $VolumeLabel
    if (-not $volume -and $TimeoutSeconds -gt 0) {
        Write-Host "Waiting up to $TimeoutSeconds s for $VolumeLabel - double-tap RESET now..."
        $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
        while (-not $volume -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 500
            $volume = Get-BootVolume -Label $VolumeLabel
        }
    }

    if (-not $volume) {
        throw @"
Bootloader volume '$VolumeLabel' is not mounted.
Double-tap RESET on the Feather until it appears, then re-run.
"@
    }

    $Uf2Path = "$($volume.DriveLetter):\CURRENT.UF2"
}

if (-not (Test-Path $Uf2Path)) {
    throw "No UF2 readback at $Uf2Path."
}

$bytes = [IO.File]::ReadAllBytes($Uf2Path)
if ($bytes.Length -lt 512 -or ($bytes.Length % 512) -ne 0) {
    throw "$Uf2Path is not a whole number of 512-byte UF2 blocks ($($bytes.Length) bytes)."
}

# Reassemble the readback as address -> payload. UF2 blocks are self-describing
# and need not be contiguous or ordered, so index them rather than assuming.
$chunks = @{}
$minAddr = [uint32]::MaxValue
$maxAddr = [uint32]0
$malformed = 0

for ($pos = 0; $pos -lt $bytes.Length; $pos += 512) {
    if ([BitConverter]::ToUInt32($bytes, $pos) -ne $UF2_MAGIC_START0 -or
        [BitConverter]::ToUInt32($bytes, $pos + 4) -ne $UF2_MAGIC_START1 -or
        [BitConverter]::ToUInt32($bytes, $pos + 508) -ne $UF2_MAGIC_END) {
        $malformed++
        continue
    }

    $addr = [BitConverter]::ToUInt32($bytes, $pos + 12)
    $size = [BitConverter]::ToUInt32($bytes, $pos + 16)
    if ($size -gt 476) { $size = 476 }

    $payload = New-Object byte[] $size
    [Array]::Copy($bytes, $pos + 32, $payload, 0, $size)
    $chunks[$addr] = $payload

    if ($addr -lt $minAddr) { $minAddr = $addr }
    if (($addr + $size) -gt $maxAddr) { $maxAddr = $addr + $size }
}

Write-Host ("Readback {0}: {1} blocks covering 0x{2:X}-0x{3:X}" -f `
    (Split-Path $Uf2Path -Leaf), $chunks.Count, $minAddr, $maxAddr)
if ($malformed) {
    Write-Warning "$malformed block(s) failed the UF2 magic check and were skipped."
}

# Chunks are keyed by their start address, so reading an arbitrary span means
# walking back to the chunk each byte falls in.
function Read-Span {
    param([uint32]$Address, [int]$Length)

    $out = New-Object byte[] $Length
    $got = 0

    while ($got -lt $Length) {
        $want = [uint32]($Address + $got)
        $chunk = $null
        $base = [uint32]0

        foreach ($key in $chunks.Keys) {
            $payload = $chunks[$key]
            if ($want -ge $key -and $want -lt ($key + $payload.Length)) {
                $chunk = $payload
                $base = $key
                break
            }
        }

        if (-not $chunk) { return $null }

        $offsetInChunk = [int]($want - $base)
        $take = [Math]::Min($chunk.Length - $offsetInChunk, $Length - $got)
        [Array]::Copy($chunk, $offsetInChunk, $out, $got, $take)
        $got += $take
    }

    return $out
}

$found = $false

foreach ($offset in $Offsets) {
    $label = "0x{0:X}" -f $offset
    $header = Read-Span -Address $offset -Length 16

    if (-not $header) {
        Write-Host "  slot $label : outside the readback range"
        continue
    }

    if ([BitConverter]::ToUInt32($header, 0) -ne $DIAG_MAGIC) {
        Write-Host "  slot $label : no log header (never written, or erased)"
        continue
    }

    $len     = [BitConverter]::ToUInt32($header, 4)
    $seq     = [BitConverter]::ToUInt32($header, 8)
    $dropped = [BitConverter]::ToUInt32($header, 12)

    $text = Read-Span -Address ([uint32]($offset + 16)) -Length ([int]$len)
    if (-not $text) {
        Write-Host "  slot $label : header found but the text is truncated by the readback"
        continue
    }

    $found = $true
    Write-Host ""
    Write-Host "===== diagnostic log at $label (flush #$seq, $len bytes, $dropped dropped) ====="
    $decoded = [Text.Encoding]::ASCII.GetString($text)
    Write-Host $decoded
    Write-Host "===== end ====="

    if ($OutFile) {
        Set-Content -Path $OutFile -Value $decoded -Encoding ascii
        Write-Host "Written to $OutFile"
    }

    break
}

if (-not $found) {
    Write-Host ""
    Write-Host "No diagnostic log recovered. Check that the flashed image was built"
    Write-Host "with diag.conf, and that it ran for a few seconds before the reset -"
    Write-Host "the first flush happens about 2 s after boot."
    exit 1
}
