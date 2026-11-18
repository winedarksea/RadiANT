# B4 gate_extra driver: fresh flash of the Pro Micro before each step, then the TX leg
# (tx_leg.py) or conformance. Stops at the first failed flash/probe/runner.
#   powershell -File run_gate_extra.ps1 txB1 txA confA txB2
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Steps)
$D = 'C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-16-b4-pair\gate_extra'
$R = 'C:\Users\Colin\ant_dongle'
$py = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
$img = @{ A = 'C:\wt_c2\build\pm_o2\dongle\zephyr\zephyr.uf2';
          B = 'C:\wt_b4\build\r2_pmdiago2\dongle\zephyr\zephyr.uf2' }
$md5 = @{ A = 'C4B4CA99FC52DEEC40ADD6BA97B312C9'; B = 'BA0A7084DA330FBF71F1CED903D61A24' }
foreach ($s in $Steps) {
  $k = $s.Substring(2, 1)   # txB1 -> B, confA -> n? handled below
  if ($s.StartsWith('conf')) { $k = $s.Substring(4, 1) }
  $out = Join-Path $D $s
  New-Item -ItemType Directory -Force $out | Out-Null
  if ((Get-FileHash -Algorithm MD5 $img[$k]).Hash -ne $md5[$k]) { "STOP $s md5 mismatch"; exit 3 }
  & 'C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-16-b4-pair\flash_pm.ps1' -Uf2 $img[$k] -Log (Join-Path $out 'flash.log') | Out-Null
  if (-not (Select-String -Quiet -Path (Join-Path $out 'flash.log') -Pattern 'probe rc=0')) { "STOP $s flash/probe failed"; exit 4 }
  "$(Get-Date -Format T) $s flashed ($k)"
  if ($s.StartsWith('conf')) {
    Push-Location $R
    $rel = "archive\benchmarks\2026-11-16-b4-pair\gate_extra\conformance-$k.antser"
    $js = "archive\benchmarks\2026-11-16-b4-pair\gate_extra\conformance-$k.json"
    & $py -u tools\ant_conformance.py --serial EE6986854FD72803 --out $rel --json $js *> (Join-Path $out 'stdout.txt')
    $rc = $LASTEXITCODE
    Pop-Location
  } else {
    & $py -u (Join-Path $D 'tx_leg.py') $out --seconds 300 *> (Join-Path $out 'runner_stdout.txt')
    $rc = $LASTEXITCODE
  }
  "$(Get-Date -Format T) $s rc=$rc"
  if ($rc -ne 0) { "STOP $s rc=$rc"; exit 5 }
}
"ALL DONE"
