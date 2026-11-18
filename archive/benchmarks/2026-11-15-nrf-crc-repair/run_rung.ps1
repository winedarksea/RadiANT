# Bench helper for C1: 300 s crc_truth blocks at one rung until the rung's
# cumulative page-10 crc_repaired reaches 20 or 900 s have run.
param([int]$Dbm, [string]$Tag, [int]$FirstBlock)
Set-Location C:\Users\Colin\ant_dongle
$py = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
$A = 'C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-15-nrf-crc-repair'
$sn = 'EE6986854FD72803'
$total = 0
for ($k = 0; $k -lt 3; $k++) {
    $n = $FirstBlock + $k
    cmd /c "$py -u tools\ant_diag.py --serial $sn --json $A\diag_before_b$n.json > $A\diag_before_b$n.log 2>&1"
    $s = Get-Date
    "start $($s.ToString('o'))" | Out-File -Encoding ascii "$A\times_b$n.txt"
    cmd /c "$py -u $A\crc_truth_nrf.py --tx-port COM4 --tx-part nrf54l15 --rx-serial $sn --dbm $Dbm --seconds 300 --json $A\truth_b${n}_$Tag.json > $A\truth_b${n}_$Tag.log 2>&1"
    "b$n rc=$LASTEXITCODE"
    $e = Get-Date
    "end $($e.ToString('o')) elapsed_s $(($e-$s).TotalSeconds)" | Out-File -Append -Encoding ascii "$A\times_b$n.txt"
    cmd /c "$py -u tools\ant_diag.py --serial $sn --json $A\diag_after_b$n.json > $A\diag_after_b$n.log 2>&1"
    cmd /c "$py -u tools\ant_diag.py --diff $A\diag_before_b$n.json $A\diag_after_b$n.json > $A\diag_diff_b$n.txt 2>&1"
    $b = Get-Content "$A\diag_before_b$n.json" -Raw | ConvertFrom-Json
    $f = Get-Content "$A\diag_after_b$n.json" -Raw | ConvertFrom-Json
    $d = $f.pages.'10'.fields.crc_repaired - $b.pages.'10'.fields.crc_repaired
    $total += $d
    "b$n repaired=$d rung_total=$total"
    Get-Content "$A\truth_b${n}_$Tag.json"
    if ($total -ge 20) { break }
}
"DONE rung $Dbm total $total" | Out-File -Encoding ascii "$A\_rung_${Tag}_done.flag"
