param([string[]]$Rows)
$ErrorActionPreference = 'Continue'
. C:\wt_c2\scripts\env.ps1 -NcsVersion v3.4.0 | Out-Null
$ErrorActionPreference = 'Continue'
Push-Location C:\ncs\v3.4.0
$z = 'C:\ncs\v3.4.0\zephyr'
$b = 'C:\wt_c2\build'
$common = '"-DANT_RADIO=core" "-DRADIANT_BACKEND=cc26xx" "-DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti"'
$defs = @{
  'ti_os' = "$common `"-DEXTRA_CONF_FILE=ti_health.conf;ti_diag.conf`""
  'ti_o2' = "$common `"-DEXTRA_CONF_FILE=ti_health.conf;ti_diag.conf;C:/wt_c2/build/o2.conf`""
}
foreach ($r in $Rows) {
  $out = "$b\$r"
  $log = "$b\$r.log"
  $t0 = Get-Date
  cmd /c "west -z $z build -s C:\wt_c2\apps\dongle_ti -d $out -b cc26x2r1_launchxl -p always -- $($defs[$r]) > $log 2>&1"
  "$r rc=$LASTEXITCODE secs=$([int]((Get-Date)-$t0).TotalSeconds)" | Out-File -Append -Encoding ascii "$b\ti_status.txt"
}
"done" | Out-File -Append -Encoding ascii "$b\ti_status.txt"
Pop-Location
