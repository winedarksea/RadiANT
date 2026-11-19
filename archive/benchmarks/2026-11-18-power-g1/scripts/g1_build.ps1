# Gate G1 image builds (hand west, from C:\ncs\v3.4.0). Run from the repo root.
param([string[]]$Arms = @('g1_B', 'g1_A', 'g1_ti_rx'))
$ErrorActionPreference = 'Stop'
$repo = 'C:\Users\Colin\ant_dongle'
Set-Location $repo
. .\scripts\env.ps1
$offConf = "C:/Users/Colin/ant_dongle/archive/benchmarks/2026-11-18-power-g1/g1_lp_off.conf"
$hrtx = "$repo\radiant\samples\hr_transmitter"
$ti = "$repo\apps\dongle_ti"
$halTi = 'C:/ncs/v3.4.0/modules/hal/ti'
$rows = @{
    'g1_B'     = @{ app = $hrtx; board = 'nrf54l15dk/nrf54l15/cpuapp'; extra = "`"-DEXTRA_CONF_FILE=lowpower.conf;power_diag_quiet.conf`"" }
    'g1_A'     = @{ app = $hrtx; board = 'nrf54l15dk/nrf54l15/cpuapp'; extra = "`"-DEXTRA_CONF_FILE=lowpower.conf;power_diag_quiet.conf;$offConf`"" }
    'g1_ti_rx' = @{ app = $ti;   board = 'cc26x2r1_launchxl';          extra = "-DANT_RADIO=core -DRADIANT_BACKEND=cc26xx -DEXTRA_ZEPHYR_MODULES=$halTi `"-DEXTRA_CONF_FILE=ti_health.conf;ti_diag.conf`"" }
}
Push-Location C:\ncs\v3.4.0
try {
    foreach ($a in $Arms) {
        $r = $rows[$a]
        $d = "$repo\build\$a"
        $log = "$repo\build\$a.log"
        Write-Host "=== $a"
        cmd /c "west -z C:\ncs\v3.4.0\zephyr build -s $($r.app) -d $d -b $($r.board) -p always -- $($r.extra) > $log 2>&1"
        Write-Host "rc=$LASTEXITCODE"
        if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 30; throw "build $a failed" }
    }
} finally { Pop-Location }
