#!/bin/bash
# Several LAN pairs at once (a native pair costs about 10 % of a core per console in real time; the null renderer is light):
#   heavy.sh -t 900 tools/mp/turn.sh "NAME|native_pair.py options" ["NAME2|options" ...]
# Each pair gets its own UDP port base (41000 + 20 * index). Env: BIN (executable), QEMU=1 (armhf under qemu-arm: run ONE pair per turn).
# Summary per pair on stdout; details in build/np/NPM2/pairs/NAME/{result.json,peer0/runtime.log,peer1/runtime.log}
cd "$(dirname "$0")/../.."
BIN=${BIN:-/home/claude/msc/build/np/NPM2/host32/np-host32-full-null}
P=${PAIRS:-/home/claude/msc/build/np/NPM2/pairs}   # PAIRS=<dir>: where the pair directories go
mkdir -p $P
i=0; pids=()
for spec in "$@"; do
  name=${spec%%|*}; opts=${spec#*|}
  rm -rf $P/$name
  eval "python3 tools/mp/native_pair.py --binary $BIN --out $P/$name --timeout ${PAIR_TIMEOUT:-840} --port-base $((41000 + 20 * i)) $opts" > $P/$name.out 2>&1 &
  pids+=($!); i=$((i + 1)); sleep 1
done
for p in "${pids[@]}"; do wait $p; done
for spec in "$@"; do
  name=${spec%%|*}
  echo "=== $name  ($(date +%T))"
  python3 tools/mp/pair_summary.py $P/$name
done
