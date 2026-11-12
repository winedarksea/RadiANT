#!/bin/bash
# Item 2 (TI burst chaining), before the pre-staging fix: TI master + Garmin
# slave, 100 x 8-packet bursts with 8-byte host blocks, then 100 x 9-packet
# bursts with 24-byte blocks (3 blocks, chained in-block from the callback),
# then the same-day control: Pro Micro master + Garmin slave.
# Garmin at --burst-tx-power 3 (0 dBm; level 4 is its weakest).
cd "$(dirname "$0")/../../../tools" || exit 1
P=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
O=../archive/benchmarks/2026-11-12-ti-findings
TI="--port COM11 --baud 57600"
TAG=${1:-pre}
leg() {  # name master-args burst-args
  local name=item2_${TAG}_$1; shift
  local margs=$1; shift
  echo "=== $name $(date +%T)"
  $P -u ant_probe.py $TI < /dev/null > /dev/null 2>&1
  $P -u ant_diag.py $TI --json $O/${name}_diag_before.json < /dev/null > /dev/null 2>&1
  timeout 900 $P -u ant_ack_pair.py $margs --slave-serial 006 \
      --burst-tx-power 3 "$@" --json-bursts $O/${name}.json < /dev/null > $O/${name}.log 2>&1
  $P -u ant_diag.py $TI --json $O/${name}_diag_after.json < /dev/null > /dev/null 2>&1
  grep -a "failure rate\|the master was told\|slave whole" $O/${name}.log
  $P ant_diag.py --diff $O/${name}_diag_before.json $O/${name}_diag_after.json < /dev/null | grep -A9 "page 2"
}
leg ti8  "--master-port COM11 --master-baud 57600" --master-bursts 100 --burst-packets 8
leg ti24 "--master-port COM11 --master-baud 57600" --master-bursts 100 --burst-packets 9 --burst-block 24
leg pm8  "--master-serial EE6986854FD72803" --master-bursts 100 --burst-packets 8
$P -u ant_probe.py $TI < /dev/null > /dev/null 2>&1
echo "=== done $(date +%T)"
