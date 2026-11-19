#!/bin/bash
# Gate G2 Garmin rerun: arms A1 B1 A2 B2 A3 back to back (Git Bash, NCS toolchain python).
S=/c/Users/Colin/AppData/Local/Temp/claude/0b707879d249ade1/455ff62d-a306-44ec-ba83-9f98c9b995b4/scratchpad/g2g
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
HERE=/c/Users/Colin/ant_dongle/archive/benchmarks/2026-11-19-power-g2-garmin/scripts
mkdir -p $S/runs
for arm in A1:g2_A: B1:g2_B:--lp A2:g2_A: B2:g2_B:--lp A3:g2_A:; do
  IFS=: read lab img lp <<< "$arm"
  $PY -u $HERE/g2g_arm.py $lab $img $S/runs --phases gsweep,gburst,tsweep,tburst \
      --count 300 --bursts 100 --ti-count 100 --ti-bursts 50 $lp > $S/${lab}_arm.log 2>&1
  echo "$lab rc $? $(date +%T)"
done
echo chain done
