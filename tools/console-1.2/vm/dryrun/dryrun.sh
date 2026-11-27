#!/bin/bash
# Dry run of the whole r17 queue against a fake console (stub curl/nc/date/sleep, virtual clock; no socket is opened).
#   bash tools/console-1.2/vm/dryrun/dryrun.sh [slice seconds, default 150]
# Env: FAKE_LOG=<a real log to replay instead of the synthetic one>; FAKE_CORRUPT_ESWAP=2 corrupts the 2nd executable upload (the queue must halt); KEEP=1 keeps the work folder.
set -u -o pipefail
D=$(cd "$(dirname "$0")" && pwd); VM=$(dirname "$D"); lim=${1:-150}
export FAKE=$(mktemp -d /tmp/qdry.XXXXXX) FAKEDIR=$D; trap '[ "${KEEP:-0}" = 1 ] || rm -rf "$FAKE"' EXIT
export PATH="$D/bin:$PATH"; case "$(command -v curl)$(command -v nc)" in "$D/bin/curl$D/bin/nc") ;; *) echo "REFUSED: curl/nc are not the stubs"; exit 9;; esac
export HOME=$FAKE/home VITA_IP=fake-vita.invalid; W=$HOME/x/w9; mkdir -p "$W/versions/r17" "$W/versions/r17pgo" "$W/out/owner" "$FAKE/console/ux0/data/mscharged-perf-test" "$FAKE/console/ux0/app/CDXMP0001"
echo 1000000 > "$FAKE/clock"
cp "$VM"/{q.sh,slice.sh,swap.sh,summ.py} "$W/"; cp -r "$VM/kit-r17q" "$W/kit-r17q"; cp "$VM/queue-r17.txt" "$W/queue.txt"
head -c 3000000 /dev/urandom > "$W/versions/r17/eboot.bin"; head -c 3100000 /dev/urandom > "$W/versions/r17pgo/eboot.bin"
cp "$W/versions/r17/eboot.bin" "$FAKE/console/ux0/app/CDXMP0001/eboot.bin"
printf 'MSC_OWNER_SETTING=1\n# the owner own file\n' > "$FAKE/console/ux0/data/mscharged-perf-test/np-player.txt"; cp "$FAKE/console/ux0/data/mscharged-perf-test/np-player.txt" "$W/out/owner/"
cd "$W"; export KITDIR=$W/kit-r17q OUT=$W/outq; export KIT=$KITDIR
echo "== routes (once)"; bash slice.sh routes | tail -3 || exit 1
echo "== q.sh $lim s slices"; calls=0; : > "$FAKE/calls.log"
while [ $calls -lt 600 ]; do
  calls=$((calls+1)); out=$(bash q.sh "$lim" 2>&1); rc=$?; t0=$(cat "$FAKE/clock")
  { echo "--- call $calls rc=$rc t=$t0"; echo "$out"; } >> "$FAKE/calls.log"
  echo "$out" | grep -E "^(STOP POINT|HALT|DEFER|QUEUE EMPTY)" | sed "s/^/  call $calls: /"
  echo "$out" | grep -q "^QUEUE EMPTY" && break
  [ $rc != 0 ] && { echo "q.sh stopped with rc $rc after call $calls"; break; }
  echo $(( t0 + 5 )) > "$FAKE/clock"     # the gap between two Mac calls
done
echo "calls: $calls, virtual minutes: $(( ($(cat "$FAKE/clock") - 1000000) / 60 ))"
python3 -I "$D/check_dry.py" "$FAKE" "$W/queue.txt" "$VM/queue-r17.txt" "$VM/kit-r17q" "$VM/../r17-sitting.txt"; rc=$?
[ "${KEEP:-0}" = 1 ] && echo "kept: $FAKE"; exit $rc
