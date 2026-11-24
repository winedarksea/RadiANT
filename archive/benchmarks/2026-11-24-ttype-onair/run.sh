#!/bin/bash
# usage: run.sh <label>  -- TI receives wildcard HR #12345; L15 impostor joins after 15 s
SP=${SP:-./out}
PY=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
cd /c/Users/Colin/ant_dongle
L=$1
$PY tools/ant_verify.py --profile heart-rate --device-number 12345 --trans-type 0 --seconds $(( ${3:-200} + 25 )) \
   --port COM11 --baud 57600 --record $SP/$L.antcap --json $SP/$L.json > $SP/$L.verify.log 2>&1 &
VP=$!
sleep 15
$PY tools/ant_sim.py --profile heart-rate --device-number 12345 --trans-type 21 --bpm 77 --noise 0 \
   --period-ppm ${2:-3000} --seconds ${3:-200} --port COM4 --baud 115200 > $SP/$L.sim.log 2>&1
wait $VP
echo verify_rc=$?
