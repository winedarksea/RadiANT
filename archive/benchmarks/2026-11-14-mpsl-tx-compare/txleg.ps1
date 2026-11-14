param(
    [Parameter(Mandatory)][string]$Leg,
    [Parameter(Mandatory)][string]$Hex,
    [string]$Out = 'C:\wt_mpsl\archive\benchmarks\2026-11-14-mpsl-tx-compare',
    [int]$Seconds = 120,
    [switch]$Diag
)
$ErrorActionPreference = 'Continue'
$py = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
$t = 'C:\wt_mpsl\tools'
New-Item -ItemType Directory -Force $Out | Out-Null
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\wt_mpsl\scripts\flash_sim_jlink.ps1 -HexPath $Hex -Device nRF54L15_M33 -Serial 1057737173 *> "$Out\$Leg-flash.txt"
"flash rc=$LASTEXITCODE md5=$((Get-FileHash $Hex -Algorithm MD5).Hash)" | Tee-Object -FilePath "$Out\$Leg-flash.txt" -Append
Start-Sleep 4
$sim = Start-Process -FilePath $py -ArgumentList @("$t\ant_sim.py", '--port', 'COM4', '--profile', 'power', '--device-number', '3333', '--seconds', "$($Seconds + 30)") -WorkingDirectory $t -RedirectStandardOutput "$Out\$Leg-sim.txt" -RedirectStandardError "$Out\$Leg-sim.err" -PassThru -NoNewWindow
Start-Sleep 8
Push-Location $t
cmd /c "`"$py`" `"$t\ant_verify.py`" --serial 006 --profile power --device-number 3333 --seconds $Seconds --json `"$Out\$Leg-ref.json`" > `"$Out\$Leg-ref.txt`" 2>&1"
Pop-Location
$sim.WaitForExit(120000) | Out-Null
if (-not $sim.HasExited) { $sim.Kill() }
& $py "$t\ant_probe.py" --serial 006 *> $null
if ($Diag) {
    Push-Location $t
    cmd /c "`"$py`" `"$t\ant_diag.py`" --port COM4 > `"$Out\$Leg-diag.txt`" 2>&1"
    Pop-Location
    Select-String -Path "$Out\$Leg-diag.txt" -Pattern 'start_missed|page 7|handoff' | ForEach-Object { $_.Line }
}
Select-String -Path "$Out\$Leg-ref.txt" -Pattern 'loss' | Select-Object -Last 4 | ForEach-Object { "$Leg $($_.Line)" }
