# Item 2 A/B on TI: staging (S, build\ti_stage) vs no staging (P, build\ti_diag),
# alternating S/P/S/P, 150 x 8-packet bursts per leg, Garmin slave at level 3.
Set-Location C:\Users\Colin\ant_dongle-ti
$bench = 'archive\benchmarks\2026-11-12-ti-findings'
$legs = @(@('S1','ti_stage'),@('P1','ti_diag'),@('S2','ti_stage'),@('P2','ti_diag'))
foreach ($l in $legs) {
    $hex = "build\$($l[1])\dongle_ti\zephyr\zephyr.hex"
    $md5 = (Get-FileHash $hex -Algorithm MD5).Hash
    "=== ab_$($l[0]) $($l[1]) md5 $md5 $(Get-Date -Format HH:mm:ss)"
    .\scripts\flash_ti.ps1 -HexPath $hex *>&1 | Select-String "verification successful" | ForEach-Object { '  flashed' }
    & 'C:\Program Files\Git\bin\bash.exe' "$bench/run_item2_ti.sh" "ab_$($l[0])" 150 ti8 2>&1 | Select-String "failure rate|master was told|tx_expired|tx_margin"
}
"=== done $(Get-Date -Format HH:mm:ss)"
