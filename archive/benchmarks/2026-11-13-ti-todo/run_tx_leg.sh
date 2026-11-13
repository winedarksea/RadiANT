#!/bin/bash
# TI as the TRANSMITTER: ant_sim on COM11 as power meter #4244, received by
# the Garmin (006) and the L15 DK (COM4) in the same window. Checks that an
# intFreq change did not move TI's transmit carrier.
#   run_tx_leg.sh NAME SECONDS
cd "$(dirname "$0")/../../../tools" || exit 1
P=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
O=../archive/benchmarks/2026-11-13-ti-todo
name=$1; secs=${2:-120}
echo "=== $name ${secs}s $(date +%T)"
for a in "--port COM11 --baud 57600" "--serial 006" "--port COM4"; do
  $P -u ant_probe.py $a < /dev/null > /dev/null 2>&1 || echo "  probe FAILED $a"
done
$P -u ant_sim.py --port COM11 --baud 57600 --device-number 4244 --seconds $((secs + 40)) < /dev/null > $O/${name}_sim_ti.log 2>&1 &
s1=$!
sleep 8
$P -u ant_scan_rate.py --port COM4 --serial 006 --seconds $secs --track 4244:0x0B --json $O/${name}.json < /dev/null > $O/${name}.log 2>&1
wait $s1
for a in "--port COM11 --baud 57600" "--serial 006" "--port COM4"; do
  $P -u ant_probe.py $a < /dev/null > /dev/null 2>&1
done
$P - "$O/${name}.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
for k, v in d["dongles"].items():
    t = v["summary"]["tracked"]
    print(f"  {k:6s} " + "  ".join(f"{dev}: {x['loss_pct']:.2f}% ({x['received']}/{x['rx_fail']})" for dev, x in t.items()))
EOF
echo "=== done $name $(date +%T)"
