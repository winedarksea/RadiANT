param([string]$Tx = "COM4", [int]$Dn = 54900, [int]$Seconds = 300, [string]$Tag = "diag_b")
# Round-B loss diagnostic: one ant_sim master on $Tx, every receiver recording,
# so the missed slots can be compared receiver by receiver.
$py = "C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
$tools = "C:\Users\Colin\ant_dongle\tools"
$out = "C:\Users\Colin\AppData\Local\Temp\claude\0b707879d249ade1\13af60b5-ca5d-466b-81ec-e97ee941179f\scratchpad\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
& $py -u "$tools\ant_probe.py" --port $Tx *> "$out\reset.log"
"reset rc $LASTEXITCODE"
$sim = Start-Process -FilePath $py -ArgumentList @("-u", "$tools\ant_sim.py", "--port", $Tx, "--seconds", ($Seconds + 40), "--device-number", $Dn, "--profile", "power") -RedirectStandardOutput "$out\sim.log" -RedirectStandardError "$out\sim.err" -PassThru -NoNewWindow
Start-Sleep -Seconds 8
$targets = @("Garmin=usb:006", "Feather=usb:755972D7183A6183", "Dongle=usb:3D55F77818BE772A")
if ($Tx -eq "COM4") { $targets += "5340=port:COM5" } else { $targets += "54L15=port:COM4" }
$targs = @(); foreach ($t in $targets) { $targs += @("--target", $t) }
& $py -u "$tools\ant_multi_verify.py" @targs --seconds $Seconds --device-number $Dn --out-dir $out --record-dir $out -- --profile power *> "$out\multi.log"
"multi rc $LASTEXITCODE"
$sim.WaitForExit()
"sim rc $($sim.ExitCode)"
