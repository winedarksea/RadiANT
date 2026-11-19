#!/bin/sh
# G1 H3 round 1: A/B interleaved with the TI bit-diag receiver (h3_ti_bits).
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
S='C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-18-power-g1-h3\scripts\h3_arm.py'
O='C:\Users\Colin\ant_dongle\build\g1h3_runs'
set -e
$PY -u "$S" R1_B   g1_B "$O" --lp
$PY -u "$S" R2_A   g1_A "$O"
$PY -u "$S" R3_B   g1_B "$O" --lp
$PY -u "$S" R4_A   g1_A "$O"
echo SEQ DONE
