# nRF regression for the ack-window reserve (radiant_burst.c is shared):
# Pro Micro master, Garmin slave at level 3, 150 x 8-packet bursts per leg,
# alternating N = build\pm_lead (e042a10) and O = the original 12:18 image.
Set-Location C:\Users\Colin\ant_dongle-ti
$bench = 'archive\benchmarks\2026-11-12-ti-lead'
$py = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
$orig = 'C:\Users\Colin\AppData\Local\Temp\claude\0b707879d249ade1\ff23c0e0-11a7-4b8d-a40d-630bde9ad5c7\scratchpad\promicro_restore_1218.uf2'
$legs = @(@('N1','build\pm_lead\dongle\zephyr\zephyr.uf2'),@('O1',$orig),@('N2','build\pm_lead\dongle\zephyr\zephyr.uf2'),@('O2',$orig))
foreach ($l in $legs) {
    $md5 = (Get-FileHash $l[1] -Algorithm MD5).Hash
    "=== nrf_$($l[0]) md5 $md5 $(Get-Date -Format HH:mm:ss)"
    & $py -u tools\ant_probe.py --serial EE6986854FD72803 --reboot-to-bootloader *>&1 | Out-Null
    Start-Sleep -Seconds 8
    .\scripts\flash_uf2.ps1 -Uf2Path $l[1] -VolumeLabel NICENANO *>&1 | ForEach-Object { "$_" } | Select-Object -Last 2
    Start-Sleep -Seconds 12
    & $py -u tools\ant_probe.py --serial EE6986854FD72803 *>&1 | ForEach-Object { "$_" } | Select-String "PASS|FAIL"
    $log = "$bench\nrf_$($l[0])_burst.log"
    cmd /c "$py -u tools\ant_ack_pair.py --master-serial EE6986854FD72803 --slave-serial 006 --burst-tx-power 3 --master-bursts 150 --burst-packets 8 --json-bursts $bench\nrf_$($l[0])_burst.json < NUL > $log 2>&1"
    Get-Content $log | Select-String "failure rate|master was told"
}
"=== done $(Get-Date -Format HH:mm:ss)"
