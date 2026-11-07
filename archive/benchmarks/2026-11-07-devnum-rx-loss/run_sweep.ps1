param(
    [int[]]$Numbers = @(54101, 54100, 54102, 54096, 54186, 54058, 54080, 54132, 43634, 21845),
    [int]$Seconds = 120,
    [string]$Prefix = "p3",
    [string]$TxSerial = "3D55F77818BE772A",
    [string[]]$Targets = @("Garmin=usb:006", "Feather=usb:755972D7183A6183", "54L15=port:COM4", "5340=port:COM5"),
    [string[]]$LogPorts = @("COM3")
)
# Phase 3 device-number sweep: one run_window.ps1 per number, same rig.
foreach ($dn in $Numbers) {
    "=== $dn $(Get-Date -Format s)"
    & (Join-Path $PSScriptRoot "run_window.ps1") -TxSerial $TxSerial -Dn $dn -Seconds $Seconds -Tag "${Prefix}_dn$dn" -Targets $Targets -LogPorts $LogPorts | Select-Object -First 3
}
"=== done $(Get-Date -Format s)"
