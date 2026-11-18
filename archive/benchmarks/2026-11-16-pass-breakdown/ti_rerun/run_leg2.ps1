param([string]$Name, [string]$Image, [switch]$NoTrainer)
# Flash the TI (fresh boot), then run one leg. Output in ti_rerun\<Name>\.
$ErrorActionPreference = 'Continue'
$d = 'C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-16-pass-breakdown\ti_rerun'
$py = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
New-Item -ItemType Directory -Force "$d\$Name" | Out-Null
& "$d\flash_ti_leg.ps1" -Hex "$d\images\$Image" -Log "$d\$Name\flash.log" | Out-Null
$probe = Select-String -Path "$d\$Name\flash.log" -Pattern '^probe rc=0' -Quiet
if (-not $probe) { "TI probe after flash FAILED, leg not run" | Out-File -Append -Encoding ascii "$d\$Name\flash.log"; exit 3 }
$tr = '--trainer'
if ($NoTrainer) { $tr = '' }
cmd /c "$py -u $d\leg_ti2.py $d\$Name $tr > $d\$Name\runner.out 2>&1"
"leg rc=$LASTEXITCODE"
Get-Content "$d\$Name\leg.log"
