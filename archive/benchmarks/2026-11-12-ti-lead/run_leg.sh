#!/bin/bash
# One leg of the in-grant-lead A/B. Usage: run_leg.sh TAG
# 150 x 8-packet TI master bursts to the Garmin slave (level 3), then 300 s of
# TI + Garmin tracking the trainer #52233 and the L15 DK simulating #4243.
cd "$(dirname "$0")/../../../tools" || exit 1
P=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
O=../archive/benchmarks/2026-11-12-ti-lead
TI="--port COM11 --baud 57600"
TAG=$1
echo "=== $TAG $(date +%T)"
$P -u ant_probe.py $TI < /dev/null > /dev/null 2>&1
$P -u ant_diag.py $TI --json $O/${TAG}_burst_diag_before.json < /dev/null > /dev/null 2>&1
timeout 1200 $P -u ant_ack_pair.py --master-port COM11 --master-baud 57600 \
    --slave-serial 006 --burst-tx-power 3 --master-bursts 150 --burst-packets 8 \
    --json-bursts $O/${TAG}_burst.json < /dev/null > $O/${TAG}_burst.log 2>&1
$P -u ant_diag.py $TI --json $O/${TAG}_burst_diag_after.json < /dev/null > /dev/null 2>&1
grep -a "failure rate\|the master was told" $O/${TAG}_burst.log
$P ant_diag.py --diff $O/${TAG}_burst_diag_before.json $O/${TAG}_burst_diag_after.json < /dev/null | grep -E "tx_expired|tx_margin_min|min_lead"
$P -u ant_probe.py $TI < /dev/null > /dev/null 2>&1
($P -u ant_sim.py --profile power --device-number 4243 --port COM4 --seconds 330 < /dev/null > $O/${TAG}_sim.log 2>&1 &)
sleep 8
$P -u ant_diag.py $TI --json $O/${TAG}_track_diag_before.json < /dev/null > /dev/null 2>&1
timeout 420 $P -u ant_scan_rate.py $TI --serial 006 --seconds 300 \
    --track 52233:0x11 --track 4243:0x0B --json $O/${TAG}_track.json < /dev/null > $O/${TAG}_track.log 2>&1
$P -u ant_diag.py $TI --json $O/${TAG}_track_diag_after.json < /dev/null > /dev/null 2>&1
grep -a -i "loss" $O/${TAG}_track.log | tail -6
sleep 25
$P -u ant_probe.py $TI < /dev/null > /dev/null 2>&1
