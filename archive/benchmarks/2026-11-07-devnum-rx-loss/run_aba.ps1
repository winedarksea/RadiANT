param(
    [int]$Dn = 54100,
    [int]$Seconds = 300,
    [string]$TxSerial = "3D55F77818BE772A",
    [string]$HexA = "build\rx_l15_n5\dongle\zephyr\zephyr.hex",
    [string]$HexB = "build\fix_l15\dongle\zephyr\zephyr.hex"
)
# Phase 5 A/B/A: the 54L15 DK alternates stock (5-byte match, no guard) and
# the slip guard; everything else is fixed for all three arms. The Garmin is
# a receiver here, so the Done gate (RadiANT within the Garmin's Wilson CI)
# can be read off directly. Run from the repo root. ASCII only.
$root = Split-Path (Split-Path (Split-Path $PSScriptRoot))
Push-Location $root
$targets = @("Garmin=usb:006", "Feather=usb:755972D7183A6183", "54L15=port:COM4", "5340=port:COM5")
foreach ($arm in @(@("A", $HexA), @("B", $HexB), @("A2", $HexA))) {
    "=== arm $($arm[0]) $(Get-Date -Format s)"
    & .\scripts\flash_sim_jlink.ps1 -HexPath $arm[1] -Serial 001057737173 | Select-String -Pattern 'O\.K\.|rror' | Select-Object -Last 2
    Start-Sleep -Seconds 3
    & (Join-Path $PSScriptRoot "run_window.ps1") -TxSerial $TxSerial -Dn $Dn -Seconds $Seconds -Tag "p5aba_$($arm[0])_dn$Dn" -Targets $targets -LogPorts @("COM3") | Select-Object -First 3
}
Pop-Location
"=== done $(Get-Date -Format s)"
