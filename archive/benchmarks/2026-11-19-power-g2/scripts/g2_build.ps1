# Gate G2 trainer images (hand west, from C:\ncs\v3.4.0), built in C:\wt_power_g2.
param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][string]$Conf)
$ErrorActionPreference = 'Stop'
$repo = 'C:\wt_power_g2'
Set-Location $repo
. .\scripts\env.ps1 -NcsVersion v3.4.0
$d = "$repo\build\$Name"
$log = "$repo\build\$Name.log"
New-Item -ItemType Directory -Force "$repo\build" | Out-Null
Push-Location C:\ncs\v3.4.0
try {
    cmd /c "west -z C:\ncs\v3.4.0\zephyr build -s $repo\apps\trainer -d $d -b nrf54l15dk/nrf54l15/cpuapp -p always -- -DRADIANT_BACKEND=nrf `"-Dtrainer_EXTRA_CONF_FILE=$Conf`" > $log 2>&1"
    Write-Host "rc=$LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 30; throw "build failed" }
} finally { Pop-Location }
