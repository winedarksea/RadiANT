#!/bin/bash
# Item 1 (TI acked reception): TI master on COM11, 100 acked sends per leg
# from a slave originator, alternating Pro Micro / Garmin. 0xF7 and 0xF6 are
# read before and after each leg; ant_probe resets between workloads.
cd "$(dirname "$0")/../../../tools" || exit 1
P=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
O=../archive/benchmarks/2026-11-12-ti-findings
TI="--port COM11 --baud 57600"
for leg in "1P EE6986854FD72803" "2G 006" "3P EE6986854FD72803" "4G 006"; do
  set -- $leg
  name=item1_$1; slave=$2
  echo "=== $name slave $slave $(date +%T)"
  $P -u ant_probe.py $TI < /dev/null > /dev/null 2>&1
  $P -u ant_diag.py $TI --json $O/${name}_diag_before.json < /dev/null > /dev/null 2>&1
  $P -u ant_health.py $TI --json < /dev/null > $O/${name}_health_before.json 2>&1
  $P -u ant_ack_pair.py --master-port COM11 --master-baud 57600 \
      --slave-serial $slave --sends 100 < /dev/null > $O/${name}.log 2>&1
  $P -u ant_diag.py $TI --json $O/${name}_diag_after.json < /dev/null > /dev/null 2>&1
  $P -u ant_health.py $TI --json < /dev/null > $O/${name}_health_after.json 2>&1
  grep -E "COMPLETED|FAILED|no terminal" $O/${name}.log | grep -c COMPLETED | sed "s/^/completed: /"
  grep -A12 "THE MASTER'S HOST" $O/${name}.log | head -14
  $P ant_diag.py --diff $O/${name}_diag_before.json $O/${name}_diag_after.json < /dev/null | grep -A9 "page [1234]"
done
$P -u ant_probe.py $TI < /dev/null > /dev/null 2>&1
echo "=== done $(date +%T)"
