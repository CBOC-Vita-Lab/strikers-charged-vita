#!/bin/bash
# TWO Vita3K instances (the native Vita package) as two consoles, Wi-Fi backend over the host's loopback: lobby, create/join, draft, loading,
# the first seconds of the match (clock >= 10). One heavy.sh turn:  tools/heavy.sh -t 800 tools/mp/v3k_pair.sh TAG SECONDS [vpk]
# Result: build/np/NPM2/v3k-runs/TAG/{host,join}/{log.txt,end.png,...} and a comparison of the game's checksum lines.
cd "$(dirname "$0")/../.."
# W5ONE: ROOT=<build root> (default NPM2's), TID=<title id> (default MSCN00001, the lab build; the player build is MSCN00003 or MSCH00002), ENVNAME=np-player.txt for the
# player build (it reads that file, not env.txt), KEEPALIVE_MS=<ms> (default 20000), FRESHSHADERS=0 keeps the emulator's shader cache (a cold cache makes first-use frames take 10 to 20 s
# and, under load, more than the 20 s keep-alive: the match is then ended by the game's own 'connection lost'), EXTRA="a b" more files for ux0:data/mscharged, SHOTS=<s> seconds between screenshots.
ROOT=${ROOT:-/home/claude/msc/build/np/NPM2}
TAG=${1:?tag}; SECS=${2:-600}; VPK=${3:-/home/claude/msc/build/np/NPM2/keep/mscharged-np-npm2-ba4e1d3a.vpk}; TID=${TID:-MSCN00001}
OUT=$ROOT/v3k-runs/$TAG; rm -rf "$OUT"; mkdir -p "$OUT/host" "$OUT/join"
mk() {  # role index port vip script
  cat > $OUT/$1/env.txt <<EOT
# Vita3K pair (tools/mp/v3k_pair.sh): $1
MSC_INPUT_SCRIPT=$5
MSC_MAX_FRAMES=30000
MSC_WATCH=1
MSC_WATCH_EVERY=300
MSC_VITA_RENDER=320x224
NP_DATA_DIR=ux0:data/mscharged/np-data
MSC_LAN_MODE=wifi
MSC_LINK_PORT=$3
MSC_LINK_BCAST=127.0.0.1:41000,127.0.0.1:41001
MSC_LINK_VIP=$4
MSC_LINK_NAME=VITA-$2
MSC_LAN_SYNC_LOG=50
MSC_LAN_PARTS=1
MSC_LAN_KEEPALIVE_MS=${KEEPALIVE_MS:-20000}
EOT
}
mk host 1 41000 10.0.0.1 v3k_host.txt
mk join 2 41001 10.0.0.2 v3k_join.txt
echo "tag=$TAG secs=$SECS vpk=$VPK ($(sha256sum "$VPK" | cut -c1-16)) start=$(date +%T)" > $OUT/run.txt
run() {  # role profile display
  PROFILE_DIR=$ROOT/$2 V3K_DNUM=$3 GAME=1 ENVTXT=$OUT/$1/env.txt ENVNAME=${ENVNAME:-env.txt} SHOTEVERY=${SHOTS:-60} KEEPDATA=${KEEPDATA:-0} \
    FILES="tools/mp/lan/v3k_$1.txt ${EXTRA:-}" tools/mp/v3k_run_multi.sh "$VPK" "$TID" "$SECS" "${UNTIL:-STATE played}" > $OUT/$1/run.txt 2>&1
  P=$ROOT/$2
  cp "$P/log.txt" "$P/stdout.txt" "$P/end.png" $OUT/$1/ 2>/dev/null; cp -r "$P/shots" $OUT/$1/ 2>/dev/null
}
run host v3k_h 78 &
sleep 4
run join v3k_j 79 &
wait
for r in host join; do
  [ -f $OUT/$r/stdout.txt ] && { head -c 200000 $OUT/$r/stdout.txt > $OUT/$r/stdout.head; grep -a -i "exception\|error\|invalid\|abort\|unimplemented\|not found\|unresolved\|net" $OUT/$r/stdout.txt | head -200 > $OUT/$r/stdout.errors; rm -f $OUT/$r/stdout.txt; }
done
cat $OUT/run.txt; for r in host join; do echo "== $r"; cat $OUT/$r/run.txt | tail -3; grep -a -E "\[mp-lan\]|\[vlink|\[script\]|STALL|FATAL" $OUT/$r/log.txt 2>/dev/null | grep -v "scene push" | tail -${TAILN:-30} | cut -c1-170; done
