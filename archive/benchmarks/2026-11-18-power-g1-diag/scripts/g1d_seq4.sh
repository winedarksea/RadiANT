#!/bin/sh
# G1 diag round 4: B with CONSTLAT held (HFXO still released) against plain B.
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
S='C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-18-power-g1-diag\scripts\g1d_arm.py'
O='C:\Users\Colin\ant_dongle\build\g1d_runs'
set -e
$PY -u "$S" G1_Bcl    g1d_Bcl      "$O" --lp
$PY -u "$S" G2_B      g1_B       "$O" --lp
$PY -u "$S" G3_A      g1_A       "$O"
$PY -u "$S" G4_Bcl    g1d_Bcl    "$O" --lp
$PY -u "$S" G5_B      g1_B       "$O" --lp
$PY -u "$S" G6_A      g1_A       "$O"
echo SEQ DONE
