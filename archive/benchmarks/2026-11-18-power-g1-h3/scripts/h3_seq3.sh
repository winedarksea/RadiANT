#!/bin/sh
# G1 H3 round 3: B + 50 us carrier hold after PHYEND, against B and A (TI h3_ti_bits2).
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
S='C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-18-power-g1-h3\scripts\h3_arm.py'
O='C:\Users\Colin\ant_dongle\build\g1h3_runs'
T='C:\wt_g1_h3\build\h3_B_tail50\hr_transmitter\zephyr'
set -e
$PY -u "$S" T1_Bt50 "$T"  "$O" --lp
$PY -u "$S" T2_B    g1_B  "$O" --lp
$PY -u "$S" T3_A    g1_A  "$O"
$PY -u "$S" T4_Bt50 "$T"  "$O" --lp
$PY -u "$S" T5_B    g1_B  "$O" --lp
$PY -u "$S" T6_Bt50 "$T"  "$O" --lp
echo SEQ DONE
