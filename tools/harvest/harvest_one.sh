#!/bin/bash
# harvest_one.sh <slot 1|2> <tag> <route name> <env file> <vpk> <dest dir> <seconds> : ONE emulator run on the slot, the profile's shader cache and grown list copied out
# inside the same lock hold (the profile is shared by every run of the slot: a later KEEPDATA=0 run deletes it). Called by harvest.sh; usable alone.
# Env: HV_NETNS=0 opts out of the network isolation: by default (tools15) the emulator runs inside `unshare -n` (tools/netns_run.sh: empty namespace, loopback only; fail closed).
# Env: STOPON=<egrep> passes to run.sh (harvest passes set it to ignore the emulator's |C| lines, so a failing draw does not end the run before the compiles).
# Writes <dest>/{cache/,log.txt,result.txt}. result.txt: end reason, MISS lines, shaderplay lines, faults, new gxp files. Exit 0 always when the emulator ran.
set -u
SLOT=$1; TAG=$2; ROUTE=$3; ENVF=$4; VPK=$5; DEST=$6; SECS=$7
case $SLOT in 1) V=/home/claude/work/v3k; L=/tmp/s13-heavy.lock;; 2) V=/home/claude/work/v3k2; L=/tmp/s13-heavy2.lock;; *) echo "bad slot"; exit 2;; esac
export LD_LIBRARY_PATH=/home/claude/work/v3k/libs QT_PLUGIN_PATH=/home/claude/work/v3k/qt-plugins
FB=${FASTBOOT:-${HV_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}/tools/console-1.2/fastboot}
mkdir -p "$DEST"; rm -rf "$DEST/cache"
exec 9>"$L"; flock 9                      # (one run at a time on the slot; held to the end of this script)
for try in 1 2 3; do
  for w in $(seq 40); do a=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo); [ "$a" -ge 3300 ] && break; sleep 15; done   # (an emulator needs ~3 GB)
  # the emulator run can never reach a live service: an empty network namespace (tools/netns_run.sh, root needed; HV_NETNS=0 opts out). Fail closed: 125 = no namespace, no run.
  env ${STOPON+STOPON="$STOPON"} TAG="$TAG" FILES="$FB/routes/$ROUTE.txt" KEEPDATA=0 bash "${HV_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}/tools/netns_run.sh" nice -n 10 "$V/run.sh" "$VPK" "$ENVF" "$SECS" > "$DEST/run.out" 2>&1
  [ $? = 125 ] && { echo "harvest_one: netns_run refused (see run.out): HV_NETNS=0 to run without isolation" >&2; exit 125; }
  O=$V/runs/$TAG
  code=$(sed -n 2p "$O/run.txt" 2>/dev/null | grep -o 'code [0-9]*' | awk '{print $2}')
  [ "$code" = 137 ] && { echo "OOM-killed, try $try" >> "$DEST/run.out"; sleep 30; continue; }
  break
done
D=$V/pref/ux0/data/mscharged-perf-test
cp -a "$D/shadercache" "$DEST/cache" 2>/dev/null
cp "$O/log.txt" "$DEST/log.txt" 2>/dev/null
LG=$DEST/log.txt
f=$(cat "$O/vita3k.log" "$O/stdout.txt" 2>/dev/null | grep -a -c -E "invalid_access|Guest fault|GUEST FAULT")
{
  echo "route=$ROUTE end=$(sed -n 2p "$O/run.txt" | sed 's/^ *//')"
  echo "miss=$(grep -a -c 'shader pack MISS' "$LG") shaderplay=$(grep -a -c '\[shaderplay\]' "$LG") faults=$f gxp_new=$(find "$DEST/cache" -name '*.gxp' 2>/dev/null | wc -l)"
  echo "packline=$(grep -a 'shader pack:' "$LG" | tail -1 | cut -c1-260)"
  echo "emu_critical=$(grep -a -c '|C|' "$O/stdout.txt")"
  echo "complete=$(sed -n 2p "$O/run.txt" | grep -c -E 'log matched: .*(script exit|route done|tour done)|emulator exited, code 0|app exited with code 0')"
  echo "unanswered=$(grep -a 'shader pack:' "$LG" | tail -1 | grep -o '[0-9]* asks it did not answer' | grep -o '^[0-9]*')"
  echo "warm=$(grep -a -m1 '\[warm\] match warm-up' "$LG" | cut -c1-240)"
} > "$DEST/result.txt"
rm -rf "$O/shots"
