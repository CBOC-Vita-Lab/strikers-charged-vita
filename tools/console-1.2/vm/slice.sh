#!/bin/bash
# Console runs in short slices (each Mac call may last at most ~3 minutes and nothing survives between calls).
#   VITA_IP=<address> KIT=<tools/console-1.2> OUT=<dir> bash slice.sh start <id>      # env/<id>.txt -> np-player.txt, old log deleted, quit + launch CDXMP0001
#   ... slice.sh wait <id> [seconds]     # polls the log until the route's exit line, a stall (60 s without growth) or the time is up; prints the state
#   ... slice.sh pull <id>               # log -> OUT/<id>/log.txt
#   ... slice.sh restore                 # puts the owner's settings file back (OUT/owner/np-player.txt), quits the test app
#   ... slice.sh routes                  # uploads $KIT/routes/*.txt to the test app's private folder, each read back and compared (do it once per kit)
# Touches only ux0:data/mscharged-perf-test/{np-player.txt,log.txt} and, for `routes`, route files <name>.txt there. Commands sent: "quit CDXMP0001", "launch CDXMP0001".
# $OUT/.dirty exists from a `start` until the next successful `restore` (q.sh uses it to restore before an executable swap and at a stop point).
set -u
H=${VITA_IP:?}; FP=1337; CP=1338; TID=CDXMP0001; PRIV="ftp://$H:$FP/ux0:/data/mscharged-perf-test"
KIT=${KIT:-${KITDIR:?}}; OUT=${OUT:?}; mkdir -p "$OUT"
say() { echo "[$(date +%T)] $*"; }
SHA() { if command -v sha256sum >/dev/null; then sha256sum | cut -c1-64; else shasum -a 256 | cut -c1-64; fi; }
cmd() { (printf '%s\n' "$1"; sleep 2) | nc -w 8 "$H" "$CP" 2>&1 | tr -d '\r' | head -2 | tr '\n' ' '; }
size() { curl -sS --max-time 15 "$PRIV/" 2>/dev/null | tr -d '\r' | awk '$NF=="log.txt"{print $5}'; }
case "${1:-}" in
start) id=${2:?}; env="$KIT/env/$id.txt"; [ -f "$env" ] || { say "no $env"; exit 2; }
  mkdir -p "$OUT/owner"; [ -s "$OUT/owner/np-player.txt" ] || { say "REFUSED: the owner's settings file is not saved in $OUT/owner"; exit 3; }
  say "quit: $(cmd "quit $TID")"; sleep 3
  curl -sS --max-time 30 -T "$env" "$PRIV/np-player.txt" || { say "upload failed"; exit 1; }
  [ "$(curl -sS --max-time 30 "$PRIV/np-player.txt" | SHA)" = "$(SHA < "$env")" ] || { say "settings file differs after upload"; exit 1; }
  curl -sS -o /dev/null --max-time 15 -Q "DELE /ux0:/data/mscharged-perf-test/log.txt" "ftp://$H:$FP/" 2>/dev/null
  [ -z "$(size)" ] || { say "old log could not be deleted"; exit 1; }
  mkdir -p "$OUT/$id"; cp "$env" "$OUT/$id/env.txt"; date +%s > "$OUT/$id/started"; : > "$OUT/.dirty"
  say "$id launch: $(cmd "launch $TID")";;
wait) id=${2:?}; lim=${3:-140}; t0=$(date +%s); last=-1; lastt=$t0
  while :; do
    s=$(size); s=${s:-0}; now=$(date +%s)
    if [ "$s" != "$last" ]; then last=$s; lastt=$now; fi
    tail=$(curl -sS --max-time 20 -r $(( s > 20000 ? s - 20000 : 0 ))- "$PRIV/log.txt" 2>/dev/null | tr -d '\r')
    if echo "$tail" | grep -q -E "^\[script\] f=[0-9]+ exit|^\[plat\] script exit at frame|\[run\] end"; then say "$id DONE: log $s bytes, $(( now - $(cat "$OUT/$id/started") )) s since launch"; exit 0; fi
    if [ $(( now - lastt )) -ge 60 ] && [ $(( now - $(cat "$OUT/$id/started") )) -ge 75 ]; then say "$id STALLED: log $s bytes, no growth for $(( now - lastt )) s; last lines:"; echo "$tail" | tail -4 | cut -c1-200; exit 4; fi
    if [ $(( now - t0 )) -ge "$lim" ]; then say "$id RUNNING: log $s bytes; $(echo "$tail" | grep -E "^\[script\]|^\[lane\] [0-9]" | tail -2 | cut -c1-150 | tr '\n' '|')"; exit 9; fi
    sleep 10
  done;;
pull) id=${2:?}; mkdir -p "$OUT/$id"; curl -sS --max-time 120 -o "$OUT/$id/log.txt" "$PRIV/log.txt" && say "$id log: $(wc -c < "$OUT/$id/log.txt") bytes, $(grep -c . "$OUT/$id/log.txt") lines";;
restore) say "quit: $(cmd "quit $TID")"; sleep 2; curl -sS --max-time 30 -T "$OUT/owner/np-player.txt" "$PRIV/np-player.txt" && [ "$(curl -sS --max-time 30 "$PRIV/np-player.txt" | SHA)" = "$(SHA < "$OUT/owner/np-player.txt")" ] && { rm -f "$OUT/.dirty"; say "owner's settings file is back (sha $(SHA < "$OUT/owner/np-player.txt" | cut -c1-12))"; } || { say "RESTORE FAILED"; exit 1; };;
routes) n=0; man="$KIT/routes.sha256"; shopt -s nullglob; set +f
  for f in "$KIT"/routes/*.txt; do b=${f##*/}
    case "$b" in np-*|log.txt) say "REFUSED: $b is not a route name"; exit 3;; esac
    [[ "$b" =~ ^[A-Za-z0-9_-]+\.txt$ ]] || { say "REFUSED: odd route file name $b"; exit 3; }
    if [ -f "$man" ]; then want=$(awk -v n="$b" '$2==n{print $1}' "$man"); [ -n "$want" ] && [ "$want" = "$(SHA < "$f")" ] || { say "REFUSED: $b is not the file listed in routes.sha256 (damaged copy?)"; exit 3; }; fi
    curl -sS --max-time 30 -T "$f" "$PRIV/$b" || { say "upload of $b failed"; exit 1; }
    [ "$(curl -sS --max-time 30 "$PRIV/$b" | SHA)" = "$(SHA < "$f")" ] || { say "route $b differs after upload"; exit 1; }
    n=$((n+1)); say "route $b ok ($(wc -c < "$f") bytes)"
  done
  [ $n -gt 0 ] || { say "no route files in $KIT/routes"; exit 2; }
  say "ROUTES OK: $n files uploaded and read back";;
*) echo "usage: slice.sh start|wait|pull|restore|routes"; exit 2;;
esac
