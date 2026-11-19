#!/bin/sh
# G1 diag round 3: plain B against B10000 in adjacent pairs (the signal drifted in round 2).
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
S='C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-18-power-g1-diag\scripts\g1d_arm.py'
O='C:\Users\Colin\ant_dongle\build\g1d_runs'
set -e
$PY -u "$S" F1_B      g1_B       "$O" --lp
$PY -u "$S" F2_B10000 g1d_B10000 "$O" --lp
$PY -u "$S" F3_A      g1_A       "$O"
$PY -u "$S" F4_B10000 g1d_B10000 "$O" --lp
$PY -u "$S" F5_B      g1_B       "$O" --lp
$PY -u "$S" F6_A      g1_A       "$O"
echo SEQ DONE
