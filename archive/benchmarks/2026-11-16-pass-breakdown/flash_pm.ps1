param([string]$Uf2, [string]$Log)
$ErrorActionPreference = 'Continue'
$py = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
"md5 $((Get-FileHash -Algorithm MD5 $Uf2).Hash) $Uf2" | Out-File -Encoding ascii $Log
cmd /c "$py -u C:\wt_c2\tools\ant_probe.py --serial EE6986854FD72803 --reboot-to-bootloader >> $Log 2>&1"
"reboot rc=$LASTEXITCODE" | Out-File -Append -Encoding ascii $Log
& C:\wt_c2\scripts\flash_uf2.ps1 -Uf2Path $Uf2 -VolumeLabel NICENANO -TimeoutSeconds 30 *>> $Log
"flash done $(Get-Date -Format o)" | Out-File -Append -Encoding ascii $Log
$ok = $false
for ($i = 0; $i -lt 30; $i++) {
  Start-Sleep 1
  if (Get-PnpDevice -PresentOnly -InstanceId 'USB\VID_0FCF&PID_1009\EE6986854FD72803' -ErrorAction SilentlyContinue) { $ok = $true; break }
}
"enumerated=$ok after ${i}s" | Out-File -Append -Encoding ascii $Log
Start-Sleep 2
cmd /c "$py -u C:\wt_c2\tools\ant_probe.py --serial EE6986854FD72803 >> $Log 2>&1"
"probe rc=$LASTEXITCODE" | Out-File -Append -Encoding ascii $Log
Get-Content $Log
