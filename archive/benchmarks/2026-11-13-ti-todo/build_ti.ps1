# Build apps/dongle_ti (cc26xx backend, NCS v3.4.0) into build\<Name>, the
# same west invocation build_all.ps1's ti_launchxl row makes, plus any extra
# -D arguments. Prints the zephyr.hex md5.
#   .\build_ti.ps1 -Name tt_p0
#   .\build_ti.ps1 -Name tt_trim_p100 -Extra '-DCONFIG_RADIANT_CC26XX_FREQ_TRIM_KHZ=100'
param(
    [Parameter(Mandatory)] [string]$Name,
    [string]$Conf = 'ti_health.conf;ti_diag.conf',
    [string[]]$Extra = @()
)
$ErrorActionPreference = 'Stop'
$repo = 'C:\Users\Colin\ant_dongle-ti'
. "$repo\scripts\env.ps1" -Bundle dcbdc366a1 | Out-Null
$out = "$repo\build\$Name"
$log = "$repo\build\$Name.log"
New-Item -ItemType Directory -Force "$repo\build" | Out-Null
$d = "-DANT_RADIO=core -DRADIANT_BACKEND=cc26xx -DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti `"-DEXTRA_CONF_FILE=$Conf`" $($Extra -join ' ')"
Push-Location C:\ncs\v3.4.0
try {
    cmd /c "west -z C:\ncs\v3.4.0\zephyr build -s $repo\apps\dongle_ti -d $out -b cc26x2r1_launchxl -p always -- $d > $log 2>&1"
    $rc = $LASTEXITCODE
} finally { Pop-Location }
if ($rc -ne 0) { Get-Content $log -Tail 30; throw "build $Name failed (rc $rc), log $log" }
$cfg = Get-Content "$out\dongle_ti\zephyr\.config"
foreach ($want in 'CONFIG_RADIANT_BACKEND_CC26XX=y', 'CONFIG_ANT_DONGLE_UART_BAUD=57600') {
    if (-not ($cfg -contains $want)) { throw "$Name .config lacks $want" }
}
$hex = "$out\dongle_ti\zephyr\zephyr.hex"
"$Name md5 $((Get-FileHash $hex -Algorithm MD5).Hash)"
$cfg | Select-String 'RADIANT_CC26XX_(RX_BW|FREQ_TRIM|AGC)'
