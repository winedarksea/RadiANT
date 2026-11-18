param([string[]]$Rows = @('ti2_os','ti2_o2'))
# build_all row ti_launchxl_diag by hand (apps/dongle_ti + ti_health.conf + ti_diag.conf), master 5584dea in C:\wt_c2.
# ti2_os adds C:/wt_c2/build/os_off.conf (CONFIG_RADIANT_HOT_PATH_O2=n); ti2_o2 is as shipped (prj.conf O2=y).
$ErrorActionPreference = 'Continue'
. C:\wt_c2\scripts\env.ps1 -NcsVersion v3.4.0 | Out-Null
$ErrorActionPreference = 'Continue'
Push-Location C:\ncs\v3.4.0
$z = 'C:\ncs\v3.4.0\zephyr'
$b = 'C:\wt_c2\build'
$common = '"-DANT_RADIO=core" "-DRADIANT_BACKEND=cc26xx" "-DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti"'
$defs = @{
  'ti2_os' = "$common `"-DEXTRA_CONF_FILE=ti_health.conf;ti_diag.conf;C:/wt_c2/build/os_off.conf`""
  'ti2_o2' = "$common `"-DEXTRA_CONF_FILE=ti_health.conf;ti_diag.conf`""
}
foreach ($r in $Rows) {
  $out = "$b\$r"
  $log = "$b\$r.log"
  $t0 = Get-Date
  cmd /c "west -z $z build -s C:\wt_c2\apps\dongle_ti -d $out -b cc26x2r1_launchxl -p always -- $($defs[$r]) > $log 2>&1"
  "$r rc=$LASTEXITCODE secs=$([int]((Get-Date)-$t0).TotalSeconds)" | Out-File -Append -Encoding ascii "$b\ti2_status.txt"
}
"done" | Out-File -Append -Encoding ascii "$b\ti2_status.txt"
Pop-Location
