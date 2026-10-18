#!/bin/bash
# Vita3K smoke of the multiplayer entry in the native Vita package (one heavy.sh turn):
#   tools/mp/v3k_lobby.sh TAG SECONDS wifi|adhoc [vpk]
# Boots the package in the container's Vita3K (software GL, ~8 frames/s), a nav script takes the main menu's MULTIPLAYER entry and, in the lobby,
# presses Create Game. Result: build/np/NPM2/v3k-runs/TAG/{log.txt (the game's own log), stdout.head/.errors (the emulator's), shots/, end.png}.
cd "$(dirname "$0")/../.."
TAG=${1:?tag}; SECS=${2:?seconds}; MODE=${3:-wifi}
VPK=${4:-/home/claude/msc/build/np/NPM2/keep/mscharged-np-npm2-ba4e1d3a.vpk}; TID=MSCN00001
P=/home/claude/msc/build/np/NPM2/v3k
OUT=/home/claude/msc/build/np/NPM2/v3k-runs/$TAG; rm -rf "$OUT"; mkdir -p "$OUT"
ENVF=$OUT/env.txt
cat > $ENVF <<EOT
# Vita3K (container, software GL): boot, MULTIPLAYER entry, lobby, Create Game (tools/mp/v3k_lobby.sh)
MSC_INPUT_SCRIPT=lan_lobby_only.txt
MSC_MAX_FRAMES=9000
MSC_WATCH=1
MSC_WATCH_EVERY=300
MSC_VITA_RENDER=320x224
NP_DATA_DIR=ux0:data/mscharged/np-data
MSC_LAN_MODE=$MODE
MSC_LINK_PORT=41000
MSC_LINK_BCAST=127.0.0.1:41000,127.0.0.1:41001
MSC_LINK_VIP=10.0.0.1
MSC_LINK_NAME=VITA-1
EOT
{ echo "tag=$TAG secs=$SECS mode=$MODE vpk=$VPK ($(sha256sum "$VPK" | cut -c1-16)) start=$(date +%T)"; } > "$OUT/run.txt"
PROFILE_DIR=$P GAME=1 ENVTXT=$ENVF SHOTEVERY=${SHOTEVERY:-30} FILES="tools/mp/lan/lan_lobby_only.txt" KEEPDATA=${KEEPDATA:-0} \
  /home/claude/msc/tools/heavy.sh -t $((SECS + 90)) /home/claude/msc/npw/NPV3/platform/vita/tools/v3k_run.sh "$VPK" "$TID" "$SECS" "${UNTIL:-created game}" >> "$OUT/run.txt" 2>&1
echo "end=$(date +%T)" >> "$OUT/run.txt"
cp "$P/log.txt" "$P/stdout.txt" "$P/end.png" "$OUT/" 2>/dev/null
cp -r "$P/shots" "$OUT/" 2>/dev/null
[ -f "$OUT/stdout.txt" ] && { head -c 300000 "$OUT/stdout.txt" > "$OUT/stdout.head"; grep -a -i "exception\|error\|invalid\|abort\|unimplemented\|not found\|missing\|unresolved\|net\|adhoc" "$OUT/stdout.txt" | head -300 > "$OUT/stdout.errors"; rm -f "$OUT/stdout.txt"; }
cat "$OUT/run.txt"; grep -a -E "\[mp-lan\]|\[vlink|\[mp-vita\]|\[vgx-vita\] (back end|Common)|\[script\]|STALL|FATAL" "$OUT/log.txt" 2>/dev/null | head -60
