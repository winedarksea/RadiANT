# B4 pair driver: for each leg, fresh flash of the Pro Micro, then leg_b4.py.
# Stops at the first leg whose flash probe or runner fails.
#   powershell -File run_legs.ps1 leg1_A leg2_B ...
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Legs)
$D = 'C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-16-b4-pair'
$py = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
$img = @{ A = 'C:\wt_c2\build\pm_o2\dongle\zephyr\zephyr.uf2';
          B = 'C:\wt_b4\build\r2_pmdiago2\dongle\zephyr\zephyr.uf2' }
$md5 = @{ A = 'C4B4CA99FC52DEEC40ADD6BA97B312C9'; B = 'BA0A7084DA330FBF71F1CED903D61A24' }
foreach ($leg in $Legs) {
  $k = $leg.Substring($leg.Length - 1)
  $out = Join-Path $D $leg
  New-Item -ItemType Directory -Force $out | Out-Null
  if ((Get-FileHash -Algorithm MD5 $img[$k]).Hash -ne $md5[$k]) { "STOP $leg md5 mismatch"; exit 3 }
  & (Join-Path $D 'flash_pm.ps1') -Uf2 $img[$k] -Log (Join-Path $out 'flash.log') | Out-Null
  if (-not (Select-String -Quiet -Path (Join-Path $out 'flash.log') -Pattern 'probe rc=0')) { "STOP $leg flash/probe failed"; exit 4 }
  "$(Get-Date -Format T) $leg flashed ($k)"
  Push-Location $D
  & $py -u leg_b4.py $leg --trainer *> (Join-Path $out 'runner_stdout.txt')
  $rc = $LASTEXITCODE
  Pop-Location
  "$(Get-Date -Format T) $leg runner rc=$rc"
  if ($rc -ne 0) { "STOP $leg runner rc=$rc"; exit 5 }
}
"ALL DONE"
