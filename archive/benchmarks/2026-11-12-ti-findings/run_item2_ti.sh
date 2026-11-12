#!/bin/bash
# Item 2: TI master + Garmin slave burst legs only. Usage: run_item2_ti.sh TAG N
cd "$(dirname "$0")/../../../tools" || exit 1
P=/c/ncs/toolchains/dcbdc366a1/opt/bin/python.exe
O=../archive/benchmarks/2026-11-12-ti-findings
TI="--port COM11 --baud 57600"
TAG=${1:-stage}; N=${2:-100}; shift 2
for spec in "${@:-ti8}"; do
  case $spec in
    ti8)  extra="--burst-packets 8" ;;
    ti24) extra="--burst-packets 9 --burst-block 24" ;;
  esac
  name=item2_${TAG}_${spec}
  echo "=== $name $(date +%T)"
  $P -u ant_probe.py $TI < /dev/null > /dev/null 2>&1
  $P -u ant_diag.py $TI --json $O/${name}_diag_before.json < /dev/null > /dev/null 2>&1
  timeout 2400 $P -u ant_ack_pair.py --master-port COM11 --master-baud 57600 \
      --slave-serial 006 --burst-tx-power 3 --master-bursts $N $extra \
      --json-bursts $O/${name}.json < /dev/null > $O/${name}.log 2>&1
  $P -u ant_diag.py $TI --json $O/${name}_diag_after.json < /dev/null > /dev/null 2>&1
  grep -a "failure rate\|the master was told\|slave whole" $O/${name}.log
  $P ant_diag.py --diff $O/${name}_diag_before.json $O/${name}_diag_after.json < /dev/null | grep -A9 "page [24]"
done
$P -u ant_probe.py $TI < /dev/null > /dev/null 2>&1
