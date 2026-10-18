#!/bin/bash
# W5-SEED: runs the scenarios of a runs.txt (name script maxframes timeout) one after the other, inside ONE heavy.sh turn:
#   heavy.sh -t 590 tools/seed/batch.sh [-r regex] [-b budget seconds] [-X exe] runs.txt
# Skips scenarios that already have rec/<name>.done; stops starting new ones after the budget (default 480 s) so that the turn ends cleanly.
OUT=${SEED_OUT:-/home/claude/msc/build/np/W5SEED}
RE=.; BUDGET=480; EXE=/home/claude/msc/build/np/W5SEED/h32/host32/np-host32-full-null
while getopts "r:b:X:" o; do case $o in r) RE=$OPTARG;; b) BUDGET=$OPTARG;; X) EXE=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND-1))
RUNS=$1; HERE=$(cd "$(dirname "$0")/../.." && pwd)
S=${SAVE:-/home/claude/msc/build/np/B3/rundata-save}
T0=$SECONDS; n=0; bad=0
mkdir -p $OUT/rec
while read -r name script maxf tmo; do
  [ -n "$name" ] || continue
  [[ "$name" =~ $RE ]] || continue
  [ -e $OUT/rec/$name.done ] && continue
  if [ $((SECONDS - T0)) -gt $BUDGET ]; then echo "budget used: stopping before $name"; break; fi
  case $script in /*) sp=$script;; *) sp=$HERE/$script;; esac
  $HERE/tools/seed/rec.sh -X $EXE -n $name -s $sp -f $maxf -S $S -t $tmo
  # status: ok = the script reached its `exit`; otherwise what stopped it (TIMEOUT of a wait, MAX_FRAMES, a crash or the run's own time limit)
  L=$OUT/rec/$name.log
  if grep -a -q "script exit at frame" $L; then st=ok; elif grep -a -q "\[script\] TIMEOUT" $L; then st=TIMEOUT; elif grep -a -q "MSC_MAX_FRAMES" $L; then st=maxframes; else st="rc=$(grep -a '^rc=' $L | tail -1)"; fi
  echo "$st" > $OUT/rec/$name.done; echo "  -> $name: $st"
  if [ "$st" = ok ]; then bad=0; else bad=$((bad+1)); fi
  if [ $bad -ge ${MAXBAD:-5} ]; then echo "$bad failures in a row: stopping the batch (something systematic)"; break; fi
  rm -rf $OUT/data/$name
  n=$((n+1))
done < $RUNS
echo "batch: $n runs in $((SECONDS - T0)) s; $(ls $OUT/rec/*.done 2>/dev/null | wc -l) done in all"
