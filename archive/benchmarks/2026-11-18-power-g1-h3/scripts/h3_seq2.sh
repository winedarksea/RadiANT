#!/bin/sh
# G1 H3 round 2: A/B with the TI CRC-bit-index receiver (h3_ti_bits2).
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
S='C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-18-power-g1-h3\scripts\h3_arm.py'
O='C:\Users\Colin\ant_dongle\build\g1h3_runs'
set -e
$PY -u "$S" Q1_B   g1_B "$O" --lp
$PY -u "$S" Q2_A   g1_A "$O"
$PY -u "$S" Q3_B   g1_B "$O" --lp
$PY -u "$S" Q4_A   g1_A "$O"
echo SEQ DONE
