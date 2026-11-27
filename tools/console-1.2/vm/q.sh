#!/bin/bash
# q.sh [seconds] : advance the console queue for up to N seconds (default 150). Queue = $HOME/x/w9/queue.txt, one entry per line:
#   <id> <base env id> [KEY=VAL | -KEY ...]    a run (env file $KIT/env/<id>.txt, or a copy of <base>'s with the overrides)
#   !eswap <eboot file>      put that executable on the console as ux0:app/CDXMP0001/eboot.bin (swap.sh; the owner's settings are restored first);
#                            the queue HALTS (not retried by itself) if the read-back differs
#   !restore                 put the owner's settings file back now
#   !stop <text>             a natural stop point: restore if needed, print the text, exit 0; the next call carries on after it
#   lines starting with # and empty lines are skipped. A finished line is moved to queue.done.
# Each call: finish the run in progress (wait, pull, summ), start the next, and restore the owner's settings when the queue is empty.
# Touches only what slice.sh and swap.sh touch (np-player.txt, log.txt, route .txt files in mscharged-perf-test, app/CDXMP0001/eboot.bin).
cd $HOME/x/w9; export VITA_IP="${VITA_IP:?export VITA_IP=<the Vita address>}" KIT="${KITDIR:-$HOME/x/w9/kit10/tools/console-1.2}" OUT="${OUT:-$HOME/x/w9/outq-r17}"
mkdir -p "$OUT/owner"; cp -n $HOME/x/w9/out/owner/np-player.txt "$OUT/owner/" 2>/dev/null
lim=${1:-150}; t0=$(date +%s); Q=queue.txt; touch $Q queue.done
left() { echo $(( lim - ($(date +%s) - t0) )); }
HALT="$OUT/.halt"
halt() { echo "HALT: $*" | tee "$HALT"; echo "(fix it, then: rm $HALT, and call q.sh again; the queue line is still at the top)"; exit 1; }
[ -f "$HALT" ] && { cat "$HALT"; echo "HALTED earlier: fix it, rm $HALT, call q.sh again"; exit 1; }
restore() { local o rc; o=$(bash slice.sh restore); rc=$?; echo "$o" | tail -1; [ $rc = 0 ] || halt "RESTORE FAILED: the owner's np-player.txt may NOT be back on the console; run: bash slice.sh restore"; }
done_line() { awk -v n="$1" 'NR!=n' $Q > $Q.t && mv $Q.t $Q; echo "$2" >> queue.done; }
while [ "$(left)" -gt 25 ]; do
  if [ -s "$OUT/.cur" ]; then id=$(cat "$OUT/.cur")
    bash slice.sh wait $id $(( $(left) - 15 )) | tail -1 | cut -c1-160; rc=${PIPESTATUS[0]}
    if [ $rc = 9 ]; then exit 0; fi
    bash slice.sh pull $id | tail -1
    if [ $rc = 4 ]; then echo "STALLED $id" >> queue.done; echo x >> "$OUT/.stalls"; else : > "$OUT/.stalls"; fi
    python3 summ.py -v "$OUT/$id/log.txt" | tee -a "$OUT/results.txt"; : > "$OUT/.cur"
    [ "$(grep -c x "$OUT/.stalls" 2>/dev/null)" -ge 3 ] && halt "three runs in a row stalled: the console is not answering or the app crashes at once"
    continue
  fi
  n=$(awk '!/^[ \t]*#/ && NF{print NR; exit}' $Q); line=""; [ -n "$n" ] && line=$(sed -n "${n}p" $Q)
  if [ -z "$line" ]; then bash slice.sh restore | tail -1; echo "QUEUE EMPTY"; exit 0; fi
  case "$line" in
    '!stop'*) [ -f "$OUT/.dirty" ] && restore; done_line $n "$line"; echo "STOP POINT: ${line#!stop}"; echo "queue left: $(grep -v '^[ 	]*#' $Q | grep -c .) lines; call q.sh again to carry on"; exit 0;;
    '!restore'*) restore; done_line $n "$line"; continue;;
    '!eswap'*) f=$(echo "${line#!eswap}" | sed 's/^ *//; s/ *$//'); [ -s "$f" ] || halt "!eswap: the file '$f' is not here (prepare-versions.py --eboot ... on the Mac, see CONSOLE-PLAN-r17.md)"
      if [ "$(left)" -lt 125 ]; then echo "DEFER: the executable swap needs a fresh call (about 2 minutes); call q.sh again"; exit 0; fi
      [ -f "$OUT/.dirty" ] && restore
      sm=$(( ($(left) - 30) / 2 )); [ $sm -gt 70 ] && sm=70; export SWAP_MAX=$sm
      o=$(bash swap.sh "$f"); rc=$?; echo "$o" | tail -2
      if [ $rc != 0 ] || ! echo "$o" | grep -q '^SWAPPED'; then halt "!eswap $f FAILED ('$(echo "$o" | tail -1)'): the eboot.bin on the console may be the old one or a partial file. Run 'bash swap.sh $f' by hand until it prints SWAPPED"; fi
      done_line $n "$line"; continue;;
    '!'*) halt "unknown queue command: $line";;
  esac
  set -- $line; id=$1; base=$2; shift 2; f="$KIT/env/$id.txt"
  [ -f "$KIT/env/$base.txt" ] || halt "no env file $KIT/env/$base.txt for the queue line '$line'"
  [ "$id" = "$base" ] || cp "$KIT/env/$base.txt" "$f"
  for kv in "$@"; do case "$kv" in -*) k=${kv#-}; grep -v "^$k=" "$f" > "$f.t"; mv "$f.t" "$f";; *) k=${kv%%=*}; grep -v "^$k=" "$f" > "$f.t"; mv "$f.t" "$f"; echo "$kv" >> "$f";; esac; done
  rm -rf "$OUT/$id"; o=$(bash slice.sh start $id); rc=$?; echo "$o" | tail -1
  [ $rc = 0 ] || halt "slice.sh start $id failed (rc $rc)"
  done_line $n "$line"; echo $id > "$OUT/.cur"
done
