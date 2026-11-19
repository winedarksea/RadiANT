# G1 diag image builds (hand west, from C:\ncs\v3.4.0). One image per call.
param([Parameter(Mandatory)][string]$Arm)
$ErrorActionPreference = 'Stop'
$repo = 'C:\Users\Colin\ant_dongle'
Set-Location $repo
. .\scripts\env.ps1
$diag = 'C:/Users/Colin/ant_dongle/archive/benchmarks/2026-11-18-power-g1-diag'
$hrtx = "$repo\radiant\samples\hr_transmitter"
$rows = @{
    'g1d_B3000' = "lowpower.conf;power_diag_quiet.conf;$diag/g1d_su3000.conf"
    'g1d_Bkeep' = "lowpower.conf;power_diag_quiet.conf;$diag/g1d_keep.conf"
    'g1d_B10000' = "lowpower.conf;power_diag_quiet.conf;$diag/g1d_su10000.conf"
    'g1d_Bcl' = "lowpower.conf;power_diag_quiet.conf;$diag/g1d_constlat.conf"
}
$d = "$repo\build\$Arm"
$log = "$repo\build\$Arm.log"
Push-Location C:\ncs\v3.4.0
try {
    cmd /c "west -z C:\ncs\v3.4.0\zephyr build -s $hrtx -d $d -b nrf54l15dk/nrf54l15/cpuapp -p always -- `"-DEXTRA_CONF_FILE=$($rows[$Arm])`" > $log 2>&1"
    Write-Host "rc=$LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 30; throw "build $Arm failed" }
} finally { Pop-Location }
