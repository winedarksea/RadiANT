#!/bin/bash
# One same-window leg: TI (COM11) and the Garmin (006) both run
# ant_scan_rate.py, tracking the trainer #52233 and the L15 DK sim #4243
# (ant_sim on COM4), plus the Pro Micro clone sim #4242 when CLONE=1.
# 0xF7 is read before and after; ant_probe resets TI between workloads.
#   run_leg.sh NAME SECONDS [CLONE]
cd "$(dirname "$0")/../../../tools" || exit 1
P=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
O=../archive/benchmarks/2026-11-13-ti-todo
TI="--port COM11 --baud 57600"
name=$1; secs=${2:-120}; clone=${3:-0}
simsecs=$((secs + 40))
echo "=== $name ${secs}s clone=$clone $(date +%T)"
$P -u ant_probe.py $TI < /dev/null > /dev/null 2>&1 || echo "  TI probe FAILED"
$P -u ant_probe.py --serial 006 < /dev/null > /dev/null 2>&1 || echo "  Garmin probe FAILED"
$P -u ant_sim.py --port COM4 --device-number 4243 --seconds $simsecs < /dev/null > $O/${name}_sim_l15.log 2>&1 &
s1=$!
track="--track 52233:0x11 --track 4243:0x0B"
if [ "$clone" = 1 ]; then
  $P -u ant_sim.py --serial EE6986854FD72803 --device-number 4242 --seconds $simsecs < /dev/null > $O/${name}_sim_clone.log 2>&1 &
  s2=$!
  track="$track --track 4242:0x0B"
fi
sleep 8
$P -u ant_diag.py $TI --json $O/${name}_diag_before.json < /dev/null > /dev/null 2>&1
$P -u ant_scan_rate.py $TI --serial 006 --seconds $secs $track --json $O/${name}.json < /dev/null > $O/${name}.log 2>&1
$P -u ant_diag.py $TI --json $O/${name}_diag_after.json < /dev/null > /dev/null 2>&1
wait $s1 ${s2:-}
$P -u ant_probe.py $TI < /dev/null > /dev/null 2>&1
$P -u ant_probe.py --serial 006 < /dev/null > /dev/null 2>&1
$P - "$O/${name}.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
for k, v in d["dongles"].items():
    t = v["summary"]["tracked"]
    print(f"  {k:6s} " + "  ".join(f"{dev}: {x['loss_pct']:.2f}% ({x['received']}/{x['rx_fail']})" for dev, x in t.items()))
EOF
echo "=== done $name $(date +%T)"
