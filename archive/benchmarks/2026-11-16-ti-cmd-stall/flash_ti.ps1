param([string]$Hex, [string]$Log)
$ErrorActionPreference = 'Continue'
$py = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
"md5 $((Get-FileHash -Algorithm MD5 $Hex).Hash) $Hex" | Out-File -Encoding ascii $Log
"start $(Get-Date -Format o)" | Out-File -Append -Encoding ascii $Log
& C:\wt_tistall\scripts\flash_ti.ps1 -HexPath $Hex -Serial L1100TY2 *>&1 | ForEach-Object { "$_" } | Out-File -Append -Encoding ascii $Log
"flash_ti rc=$LASTEXITCODE done $(Get-Date -Format o)" | Out-File -Append -Encoding ascii $Log
cmd /c "$py -u C:\wt_tistall\tools\ant_probe.py --port COM11 --baud 57600 >> $Log 2>&1"
"probe rc=$LASTEXITCODE" | Out-File -Append -Encoding ascii $Log
Get-Content $Log
