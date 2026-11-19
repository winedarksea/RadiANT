#!/bin/sh
# G1 re-judge: A1 B A2 B2 A3, 600 s each, TI = crcfix_ti_rx (a3862fbf), Pro Micro pm_diag_acdd9d2.
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
S='C:\Users\Colin\ant_dongle\archive\benchmarks\2026-11-19-power-g1-rejudge\scripts\g1r_arm.py'
O='C:\Users\Colin\ant_dongle\build\g1r_runs'
set -e
$PY -u "$S" A1 g1_A "$O"
$PY -u "$S" B  g1_B "$O" --lp
$PY -u "$S" A2 g1_A "$O"
$PY -u "$S" B2 g1_B "$O" --lp
$PY -u "$S" A3 g1_A "$O"
echo SEQ DONE
