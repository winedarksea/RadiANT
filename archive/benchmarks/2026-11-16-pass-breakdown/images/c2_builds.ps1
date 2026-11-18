param([string[]]$Rows)
$ErrorActionPreference = 'Continue'
. C:\wt_c2\scripts\env.ps1 -NcsVersion v3.4.0 | Out-Null
$ErrorActionPreference = 'Continue'
Push-Location C:\ncs\v3.4.0
$z = 'C:\ncs\v3.4.0\zephyr'
$b = 'C:\wt_c2\build'
$defs = @{
  'pm_os' = @('apps\dongle', 'promicro_nrf52840/nrf52840/uf2', '-DANT_RADIO=core -DRADIANT_BACKEND=nrf -DEXTRA_CONF_FILE=diag_counters.conf')
  'pm_o2' = @('apps\dongle', 'promicro_nrf52840/nrf52840/uf2', '"-DANT_RADIO=core" "-DRADIANT_BACKEND=nrf" "-DEXTRA_CONF_FILE=diag_counters.conf;C:/wt_c2/build/o2.conf"')
  'dg'    = @('apps\dongle', 'nrf52840dongle/nrf52840', '-DANT_RADIO=core -DRADIANT_BACKEND=nrf')
  'n53net'= @('apps\dongle', 'nrf5340dk/nrf5340/cpunet', '-DANT_RADIO=core -DRADIANT_BACKEND=nrf')
  'n53app'= @('radiant\spike\promisc\appcore', 'nrf5340dk/nrf5340/cpuapp', '')
}
foreach ($r in $Rows) {
  $d = $defs[$r]
  $out = "$b\$r"
  $log = "$b\$r.log"
  $t0 = Get-Date
  if ($d[2]) {
    cmd /c "west -z $z build -s C:\wt_c2\$($d[0]) -d $out -b $($d[1]) -p always -- $($d[2]) > $log 2>&1"
  } else {
    cmd /c "west -z $z build -s C:\wt_c2\$($d[0]) -d $out -b $($d[1]) -p always > $log 2>&1"
  }
  "$r rc=$LASTEXITCODE secs=$([int]((Get-Date)-$t0).TotalSeconds)" | Out-File -Append -Encoding ascii "$b\status.txt"
}
"done" | Out-File -Append -Encoding ascii "$b\status.txt"
Pop-Location
