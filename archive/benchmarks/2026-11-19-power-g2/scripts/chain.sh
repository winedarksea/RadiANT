#!/bin/bash
S=/c/Users/Colin/AppData/Local/Temp/claude/0b707879d249ade1/455ff62d-a306-44ec-ba83-9f98c9b995b4/scratchpad/g2
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
until grep -q "arm done\|Traceback\|SystemExit" $S/A1_arm.log; do sleep 10; done
for arm in B1:g2_B:--lp A2:g2_A: B2:g2_B:--lp A3:g2_A:; do
  IFS=: read lab b lp <<< "$arm"
  $PY -u $S/g2_arm.py $lab $b $S/runs --sim-port COM3 --count 300 --bursts 100 $lp > $S/${lab}_arm.log 2>&1
  echo "$lab rc $?"
done
echo chain done
