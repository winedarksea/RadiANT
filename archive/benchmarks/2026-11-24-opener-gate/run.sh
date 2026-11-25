#!/bin/sh
# Opener-gate A/B/A, 2026-11-24. TI flashed between arms with
#   scripts/flash_ti.ps1 -HexPath images/ti_{old_d94d07e,new_22c3aa0}.hex
# then tools/ant_probe.py --port COM11 --baud 57600 (PASS each time).
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
R=archive/benchmarks/2026-11-24-opener-gate/raw
common="--master-port COM4 --master-baud 115200 --slave-serial 006 --dut-port COM11 --dut-baud 57600 --trials 200"
$PY -u tools/ant_opener_ab.py $common --seed 101 --label A1_old --out $R/A1_old.jsonl
# flash new
$PY -u tools/ant_opener_ab.py $common --seed 202 --label B_new  --out $R/B_new.jsonl
# flash old (ship image restored)
$PY -u tools/ant_opener_ab.py $common --seed 303 --label A2_old --out $R/A2_old.jsonl
$PY archive/benchmarks/2026-11-24-opener-gate/analyse.py $R/A1_old.jsonl $R/B_new.jsonl $R/A2_old.jsonl
