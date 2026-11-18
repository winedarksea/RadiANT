# B4 image with the register shadow compiled out, source unchanged (C:\wt_b4 at f90fff9):
# the r2_pmdiago2 row of scratchpad b4\builds.ps1 plus EXTRA_CFLAGS=-DRADIANT_NRF_REG_SHADOW=0
# (radiant_radio_nrf.c honours a pre-defined RADIANT_NRF_REG_SHADOW).
$ErrorActionPreference = 'Continue'
. C:\wt_b4\scripts\env.ps1 -NcsVersion v3.4.0
$ErrorActionPreference = 'Continue'
Push-Location C:\ncs\v3.4.0
$d = 'C:\wt_b4\build\an_pmns'
cmd /c "west -z C:\ncs\v3.4.0\zephyr build -s C:\wt_b4\apps\dongle -d $d -b promicro_nrf52840/nrf52840/uf2 -p always -- -DANT_RADIO=core -DRADIANT_BACKEND=nrf -DEXTRA_CONF_FILE=diag_counters.conf -DCONFIG_RADIANT_HOT_PATH_O2=y -DEXTRA_CFLAGS=-DRADIANT_NRF_REG_SHADOW=0 > C:\wt_b4\build\an_pmns.log 2>&1"
"rc=$LASTEXITCODE"
Pop-Location
