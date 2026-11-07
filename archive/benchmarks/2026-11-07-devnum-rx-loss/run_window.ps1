param(
    [string]$TxPort,
    [string]$TxSerial,
    [int]$Dn = 54100,
    [int]$Seconds = 120,
    [string]$Tag = "window",
    [string[]]$LogPorts = @(),
    [string[]]$Targets = @("Garmin=usb:006", "Feather=usb:755972D7183A6183", "Dongle=usb:3D55F77818BE772A", "54L15=port:COM4", "5340=port:COM5")
)
# One ant_sim master (UART --port or USB --serial), every receiver in
# $Targets recording the same window. Derived from
# ../2026-11-07-garmin-compare/devnum-ab/diag_b.ps1 with $out fixed to this
# directory and both the transmitter and the target list parameterised.
# ASCII only (PowerShell 5.1 reads .ps1 as ANSI).
$ErrorActionPreference = 'Continue'
$py = "C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
$tools = "C:\Users\Colin\ant_dongle\tools"
$out = Join-Path $PSScriptRoot $Tag
New-Item -ItemType Directory -Force $out | Out-Null
if ($TxPort) { $txArgs = @("--port", $TxPort) } elseif ($TxSerial) { $txArgs = @("--serial", $TxSerial) } else { throw "need -TxPort or -TxSerial" }
$logs = @()
foreach ($lp in $LogPorts) {
    # Board log VCOMs (DTR asserted by capture_log.ps1), for the rxdiag lines.
    $logs += Start-Process -FilePath powershell.exe -ArgumentList @("-NoProfile", "-File", "C:\Users\Colin\ant_dongle\scripts\capture_log.ps1", "-Port", $lp, "-Seconds", ($Seconds + 60), "-Out", "$out\log_$lp.txt") -PassThru -WindowStyle Hidden
}
& $py -u "$tools\ant_probe.py" @txArgs *> "$out\reset.log"
"reset rc $LASTEXITCODE"
$simArgs = @("-u", "$tools\ant_sim.py") + $txArgs + @("--seconds", ($Seconds + 40), "--device-number", $Dn, "--profile", "power")
$sim = Start-Process -FilePath $py -ArgumentList $simArgs -RedirectStandardOutput "$out\sim.log" -RedirectStandardError "$out\sim.err" -PassThru -NoNewWindow
Start-Sleep -Seconds 8
$targs = @(); foreach ($t in $Targets) { $targs += @("--target", $t) }
& $py -u "$tools\ant_multi_verify.py" @targs --seconds $Seconds --device-number $Dn --out-dir $out --record-dir $out -- --profile power *> "$out\multi.log"
"multi rc $LASTEXITCODE"
$sim.WaitForExit()
"sim rc $($sim.ExitCode)"
foreach ($l in $logs) { $l.WaitForExit() }
# Let Windows release the VCOM before the next window's capture opens it:
# back-to-back windows otherwise lost every other log.
if ($logs.Count -gt 0) { Start-Sleep -Seconds 4 }
@{ tx = ($txArgs -join ' '); dn = $Dn; seconds = $Seconds; targets = $Targets; started = (Get-Date).ToString('s') } | ConvertTo-Json | Set-Content -Encoding utf8 "$out\window.json"
Get-Content "$out\multi.log" | Select-Object -Last 12
