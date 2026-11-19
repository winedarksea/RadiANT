#!/bin/sh
# G1 diag round 2: never-sleep control and the 10 ms start-up allowance.
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
S='C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-18-power-g1-diag\scripts\g1d_arm.py'
O='C:\Users\Colin\ant_dongle\build\g1d_runs'
set -e
$PY -u "$S" E1_Bkeep  g1d_Bkeep  "$O" --lp
$PY -u "$S" E2_B10000 g1d_B10000 "$O" --lp
$PY -u "$S" E3_A      g1_A       "$O"
$PY -u "$S" E4_B10000 g1d_B10000 "$O" --lp
$PY -u "$S" E5_Bkeep  g1d_Bkeep  "$O" --lp
$PY -u "$S" E6_A      g1_A       "$O"
echo SEQ DONE
