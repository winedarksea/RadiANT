param([string[]]$Arms = @('ga_def_q', 'ga_lp_q', 'ga_lp1w_q', 'ga_def_log'))
$ErrorActionPreference = 'Stop'
$wt = 'C:\wt_power_ta'
Set-Location $wt
. .\scripts\env.ps1
$confs = @{
    'ga_def_q'   = 'power_diag_quiet.conf'
    'ga_lp_q'    = 'lowpower.conf;power_diag_quiet.conf'
    'ga_lp1w_q'  = 'lowpower.conf;one_way.conf;power_diag_quiet.conf'
    'ga_def_log' = 'power_diag.conf'
}
$app = "$wt\radiant\samples\hr_transmitter"
Push-Location C:\ncs\v3.4.0
try {
    foreach ($a in $Arms) {
        $d = "$wt\build\$a"
        $log = "$wt\build\$a.log"
        Write-Host "=== $a ($($confs[$a]))"
        cmd /c "west -z C:\ncs\v3.4.0\zephyr build -s $app -d $d -b nrf54l15dk/nrf54l15/cpuapp -p always -- `"-DEXTRA_CONF_FILE=$($confs[$a])`" > $log 2>&1"
        Write-Host "rc=$LASTEXITCODE"
        if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 30; throw "build $a failed" }
    }
} finally { Pop-Location }
