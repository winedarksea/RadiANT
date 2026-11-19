#!/bin/sh
# The G1 diag sequence, 300 s arms, A controls interleaved.
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
S='C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-18-power-g1-diag\scripts\g1d_arm.py'
O='C:\Users\Colin\ant_dongle\build\g1d_runs'
set -e
$PY -u "$S" D1_A     g1_A      "$O"
$PY -u "$S" D2_B     g1_B      "$O" --lp
$PY -u "$S" D3_B3000 g1d_B3000 "$O" --lp
$PY -u "$S" D4_A     g1_A      "$O"
$PY -u "$S" D5_B3000 g1d_B3000 "$O" --lp
$PY -u "$S" D6_B     g1_B      "$O" --lp
$PY -u "$S" D7_A     g1_A      "$O"
echo SEQ DONE
