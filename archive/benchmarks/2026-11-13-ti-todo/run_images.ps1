# Flash each image, then run one same-window run_leg.sh leg on it.
#   run_images.ps1 -Legs 'trim_0:tt_trim0','trim_p100:tt_trim_p100' -Seconds 120 -Clone 1
param([string[]]$Legs, [int]$Seconds = 120, [int]$Clone = 1, [string]$Script = "run_leg.sh")
Set-Location C:\Users\Colin\ant_dongle-ti
$bench = 'archive\benchmarks\2026-11-13-ti-todo'
foreach ($l in $Legs) {
    $leg, $img = $l -split ':'
    $hex = "build\$img\dongle_ti\zephyr\zephyr.hex"
    "=== $leg image $img md5 $((Get-FileHash $hex -Algorithm MD5).Hash) $(Get-Date -Format HH:mm:ss)"
    $ok = .\scripts\flash_ti.ps1 -HexPath $hex *>&1 | Select-String 'verification successful|Flashed'
    if (-not $ok) { "  FLASH FAILED"; continue }
    & 'C:\Program Files\Git\bin\bash.exe' "$bench/$Script" $leg $Seconds $Clone 2>&1
}
"=== all done $(Get-Date -Format HH:mm:ss)"
