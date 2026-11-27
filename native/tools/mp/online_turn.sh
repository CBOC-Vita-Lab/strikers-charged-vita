#!/bin/bash
# Online pairs through the NAT simulator: tools/mp/online_turn.sh "NAME|online_pair.py options" ["NAME2|options" ...]   (one pair at a time per call: the
# consoles of a pair run in real time; several pairs at once need their own --sim-port, set from the index here)
#   BIN=<np-host32-full-null> PROXY=<natsim_proxy> PAIRS=<dir> PAIR_TIMEOUT=840 heavy.sh tools/mp/online_turn.sh "cone|--nat0 port --nat1 port" "loss5|--loss 5 --delay 75"
cd "$(dirname "$0")/../.."
BIN=${BIN:?set BIN to the np-host32-full-null executable}
PROXY=${PROXY:?set PROXY to the natsim_proxy executable (tools/net/natsim_proxy.cpp)}
P=${PAIRS:-/tmp/np-online-pairs}
mkdir -p $P
i=0; pids=()
for spec in "$@"; do
  name=${spec%%|*}; opts=${spec#*|}
  rm -rf $P/$name
  eval "python3 tools/mp/online_pair.py --binary $BIN --proxy $PROXY --out $P/$name --sim-port $((${SIM_PORT_BASE:-47000} + 20 * i)) --port-base $((${PORT_BASE:-41000} + 20 * i)) --timeout ${PAIR_TIMEOUT:-840} $opts" > $P.$name.out 2>&1 &
  pids+=($!); i=$((i + 1)); sleep 1
done
for p in "${pids[@]}"; do wait $p; done
for spec in "$@"; do
  name=${spec%%|*}
  echo "=== $name  ($(date +%T))"
  python3 tools/mp/pair_summary.py $P/$name/pair
  grep -h "stats" $P/$name/proxy.log | tail -1
done
