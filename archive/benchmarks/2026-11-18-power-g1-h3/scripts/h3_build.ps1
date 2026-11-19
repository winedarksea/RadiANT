# G1 H3 L15 image builds from the diag/g1-h3 worktree (hand west, from C:\ncs\v3.4.0). One image per call.
param([Parameter(Mandatory)][string]$Arm)
$ErrorActionPreference = 'Stop'
$wt = 'C:\wt_g1_h3'
Set-Location $wt
. .\scripts\env.ps1 | Out-Null
$h3 = 'C:/Users/Colin/ant_dongle/archive/benchmarks/2026-11-18-power-g1-h3'
$off = 'C:/Users/Colin/ant_dongle/archive/benchmarks/2026-11-18-power-g1/g1_lp_off.conf'
$rows = @{
    'h3_B_meas' = "lowpower.conf;power_diag_quiet.conf;$h3/h3_meas.conf"
    'h3_A_meas' = "lowpower.conf;power_diag_quiet.conf;$off;$h3/h3_meas.conf"
    'h3_B_fix'  = "lowpower.conf;power_diag_quiet.conf;$h3/h3_fix.conf"
    'h3_B_tail50' = "lowpower.conf;power_diag_quiet.conf;$h3/h3_tail50.conf"
    'h3_A_tail50' = "lowpower.conf;power_diag_quiet.conf;$off;$h3/h3_tail50.conf"
    'h3_A_fix'  = "lowpower.conf;power_diag_quiet.conf;$off;$h3/h3_fix.conf"
}
$d = "$wt\build\$Arm"
$log = "$wt\build\$Arm.log"
Push-Location C:\ncs\v3.4.0
try {
    cmd /c "west -z C:\ncs\v3.4.0\zephyr build -s $wt\radiant\samples\hr_transmitter -d $d -b nrf54l15dk/nrf54l15/cpuapp -p always -- `"-DEXTRA_CONF_FILE=$($rows[$Arm])`" > $log 2>&1"
    Write-Host "rc=$LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 30; throw "build $Arm failed" }
} finally { Pop-Location }
Select-String -Path "$d\hr_transmitter\zephyr\.config" -Pattern 'TX_PHYEND|HR_TX_RELEASE_HFXO|RADIANT_NRF_LOW_POWER='
