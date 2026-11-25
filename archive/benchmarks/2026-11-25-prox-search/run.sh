#!/bin/sh
# Host 0x71 proximity search on air, 2026-11-25 (plan B4, "Host 0x71").
# DUT: TI LaunchXL COM11 @57600, flashed with
#   scripts/flash_ti.ps1 -HexPath images/ti_prox_d3f39f7.hex
# (build_all -Only radiant_dongle_cc26x2r1_launchxl.hex from wip/s24-bridge-prox
# d3f39f7), then tools/ant_probe.py --port COM11 --baud 57600 (PASS).
# Master: nRF54L15 DK COM4 @115200 (wd_dir_0fce666, unchanged), TX power stepped.
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
R=archive/benchmarks/2026-11-25-prox-search/raw
dut="--master-port COM4 --dut-port COM11"
$PY -u tools/ant_prox_onair.py $dut --mode wire --mode ladder \
    --levels +8,0,-12,-20,-28 --seed 2511 --out $R/ladder.jsonl
# One-shot / timeout-keeps: weak = -20 dBm (about -67 dBm at the DUT), strong = +8.
$PY -u tools/ant_prox_onair.py $dut --mode behaviour --strong +8 --weak -20 \
    --behaviour-bin 3 --rounds 6 --seed 2512 --out $R/behaviour.jsonl
# Tighter brackets: bin 3 against -57 dBm, bin 10 against -73 dBm.
$PY -u tools/ant_prox_onair.py $dut --mode ladder --levels=-12,-28 --margin 4 \
    --cal 10 --seed 2513 --out $R/edge.jsonl
# Real sensors (no master): ANT+ HR, RF 57, period 8070.
hr="--dut-port COM11 --dev-type 120 --trans-type 0 --rf 57 --period 8070 --levels=fixed"
$PY -u tools/ant_prox_onair.py $hr --device 0 --search-timeout 4 --mode ladder \
    --cal 20 --trials 10 --margin 3 --seed 2514 --out $R/hr_real.jsonl
$PY -u tools/ant_prox_onair.py $hr --device 8265 --search-timeout 2 --mode ladder \
    --cal 20 --trials 10 --margin 4 --seed 2515 --out $R/hr_8265.jsonl
$PY -u tools/ant_prox_onair.py $hr --device 8265 --search-timeout 2 --mode ladder \
    --cal 10 --trials 10 --margin 16 --seed 2516 --out $R/hr_8265_wide.jsonl
$PY -u tools/ant_prox_onair.py $hr --device 0 --search-timeout 2 --mode ladder \
    --cal 20 --trials 30 --pass-bin 3 --seed 2517 --out $R/hr_nearfar.jsonl
# Restore: scripts/flash_ti.ps1 -HexPath build/zwift_d94d07e/radiant_dongle_cc26x2r1_launchxl.hex
# (sha256 2cf25860...), then ant_probe PASS (advanced options 2 = 0x26, no prox bit).
