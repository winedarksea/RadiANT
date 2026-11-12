# In-grant-lead A/B on TI: L = build\ti_lead (e042a10), S = build\ti_stage
# (0207f86, the master code), alternating L/S x3. Each leg: run_leg.sh.
Set-Location C:\Users\Colin\ant_dongle-ti
$bench = 'archive\benchmarks\2026-11-12-ti-lead'
$legs = @(@('L1','ti_lead'),@('S1','ti_stage'),@('L2','ti_lead'),@('S2','ti_stage'),@('L3','ti_lead'),@('S3','ti_stage'))
foreach ($l in $legs) {
    $hex = "build\$($l[1])\dongle_ti\zephyr\zephyr.hex"
    $md5 = (Get-FileHash $hex -Algorithm MD5).Hash
    "=== $($l[0]) $($l[1]) md5 $md5 $(Get-Date -Format HH:mm:ss)"
    .\scripts\flash_ti.ps1 -HexPath $hex *>&1 | Select-String "verification successful" | ForEach-Object { '  flashed' }
    & 'C:\Program Files\Git\bin\bash.exe' "$bench/run_leg.sh" $l[0] 2>&1 | ForEach-Object { "$_" }
}
"=== done $(Get-Date -Format HH:mm:ss)"
