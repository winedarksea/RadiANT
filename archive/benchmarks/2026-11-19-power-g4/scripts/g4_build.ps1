# Gate G4 image (hand west, from C:\ncs\v3.4.0), built in C:\wt_mpsl_arm at 47bdf80.
# = build_all row node_ble_lp_pd (ble.conf;lowpower.conf;power_diag.conf) PLUS
# ble_nosuppress.conf, because the default HRM_SUPPRESS_ANT_WHEN_CONNECTED=y
# closes ANT+ while a central is connected and G4 needs BLE connected + ANT.
$ErrorActionPreference = 'Stop'
$repo = 'C:\wt_mpsl_arm'
Set-Location $repo
. .\scripts\env.ps1
$d = "$repo\build\g4_lp_pd_ns"
$log = "$repo\build\g4_lp_pd_ns.log"
Push-Location C:\ncs\v3.4.0
try {
    cmd /c "west -z C:\ncs\v3.4.0\zephyr build -s $repo\apps\hrm_ble -d $d -b nrf54l15dk/nrf54l15/cpuapp -p always -- -DRADIANT_BACKEND=nrf `"-Dhrm_ble_EXTRA_CONF_FILE=ble.conf;ble_nosuppress.conf;lowpower.conf;power_diag.conf`" > $log 2>&1"
    Write-Host "rc=$LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 30; throw "build failed" }
} finally { Pop-Location }
