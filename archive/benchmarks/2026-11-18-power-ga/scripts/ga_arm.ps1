param([Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$Arm, [switch]$NoFlash, [int]$VerifySeconds = 60)
$ErrorActionPreference = 'Stop'
$wt = 'C:\wt_power_ta'
Set-Location $wt
$py = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
$env:PYTHONIOENCODING = 'utf-8'
$hex = "$wt\build\$Build\hr_transmitter\zephyr\zephyr.hex"
$elf = "$wt\build\$Build\hr_transmitter\zephyr\zephyr.elf"
$d = "$wt\archive\benchmarks\2026-11-18-power-ga\$Arm"
New-Item -ItemType Directory -Force $d | Out-Null
$st = git status --short
if ($st) { Write-Host "git status not clean:`n$st"; throw 'dirty tree' }
if (-not $NoFlash) {
    .\scripts\flash_sim_jlink.ps1 -HexPath $hex -Device nRF54L15_M33 -Serial 001057737173 | Out-File -Encoding ascii "$d\flash.log"
    Write-Host "flash rc=$LASTEXITCODE at $(Get-Date -Format o)"
    Select-String -Path "$d\flash.log" -Pattern 'Verifying|O\.K\.|rror' | ForEach-Object { $_.Line }
    Start-Sleep -Seconds 32
}
$vp = Start-Process -FilePath $py -ArgumentList @('-u', 'tools\ant_verify.py', '--profile', 'heart-rate', '--device-number', '12345', '--seconds', "$VerifySeconds", '--serial', 'EE6986854FD72803', '--expect-bpm', '120', '--json', "$d\verify.json") -RedirectStandardOutput "$d\verify.log" -RedirectStandardError "$d\verify.err" -NoNewWindow -PassThru
.\scripts\jlink_peek.ps1 -Preset nrf54l15 -Serial 001057737173 -Samples 200 -ElfPath $elf -Decode -Json "$d\peek.json" 6>&1 | Out-File -Encoding ascii "$d\peek_report.txt"
Write-Host "peek exit=$LASTEXITCODE"
$j = Get-Content "$d\peek.json" -Raw | ConvertFrom-Json
Copy-Item $j.raw_log "$d\jlink_raw.log"
$vp.WaitForExit()
Write-Host "verify exit=$($vp.ExitCode)"
& $py tools\ant_power_est.py --board nrf54l15dk "$d\peek.json" --json "$d\est.json" | Out-File -Encoding utf8 "$d\est.txt"
& $py tools\ant_power_est.py --board nrf54l15dk "$d\peek.json" --hfxo-row X2 | Out-File -Encoding utf8 "$d\est_x2.txt"
Get-Content "$d\peek_report.txt" | Select-String -Pattern 'liveness|uptime|HFXO running|CONSTLAT on|NOT USABLE|PARSE|SECURITY'
Get-Content "$d\verify.log" | Select-String -Pattern '^\s+(OK|FAIL)|signal|heard on'
Get-Content "$d\est.txt"
(Get-FileHash $hex -Algorithm MD5).Hash
