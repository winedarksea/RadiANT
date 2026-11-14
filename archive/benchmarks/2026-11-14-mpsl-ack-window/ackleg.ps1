param(
    [Parameter(Mandatory)][string]$Leg,      # label, e.g. h_gate_1
    [string]$Hex = '',                       # flash this to the L15 first ('' = no flash)
    [string]$Out = 'C:\wt_mpsl\archive\benchmarks\2026-11-14-mpsl-ack-window',
    [string[]]$Dirs = @('slave_dut', 'master_dut', 'master_acked_dut'),
    [int]$Sends = 60
)
$ErrorActionPreference = 'Continue'
$py = 'C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe'
$t = 'C:\wt_mpsl\tools'
New-Item -ItemType Directory -Force $Out | Out-Null
if ($Hex) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File C:\wt_mpsl\scripts\flash_sim_jlink.ps1 -HexPath $Hex -Device nRF54L15_M33 -Serial 1057737173 *> "$Out\$Leg-flash.txt"
    "flash rc=$LASTEXITCODE md5=$((Get-FileHash $Hex -Algorithm MD5).Hash)" | Tee-Object -FilePath "$Out\$Leg-flash.txt" -Append
    Start-Sleep 4
}
foreach ($d in $Dirs) {
    $j = "$Out\$Leg-$d.json"; $txt = "$Out\$Leg-$d.txt"
    switch ($d) {
        'slave_dut'        { $a = @('--master-serial', '006', '--slave-port', 'COM4', '--seconds', '90', '--sends', "$Sends", '--interval', '1', '--tx-power', '3', '--json', $j) }
        'master_dut'       { $a = @('--master-port', 'COM4', '--slave-serial', '006', '--seconds', '90', '--sends', "$Sends", '--interval', '1', '--tx-power', '3', '--json', $j) }
        'master_acked_dut' { $a = @('--master-port', 'COM4', '--slave-serial', '006', '--master-acked', "$Sends", '--tx-power', '3', '--json-bursts', $j) }
        'pm_slave_ctrl'    { $a = @('--master-serial', '006', '--slave-serial', 'EE6986854FD72803', '--seconds', '90', '--sends', "$Sends", '--interval', '1', '--tx-power', '3', '--json', $j) }
    }
    Push-Location $t
    cmd /c "`"$py`" `"$t\ant_ack_pair.py`" $($a -join ' ') > `"$txt`" 2>&1"
    $rc = $LASTEXITCODE
    Pop-Location
    "== $Leg $d rc=$rc"
    Get-Content $txt -Tail 12
    # Reset the Garmin: it keeps its channels open for the next tool.
    & $py "$t\ant_probe.py" --serial 006 *> $null
    Start-Sleep 2
}
