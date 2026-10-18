#!/bin/bash
# Determinism sweep: the same binary, script and virtual clock, run once per environment size (a dummy variable of N bytes), digests compared.
#   heavy.sh -t 900 tools/det/envsweep.sh [-x exe] [-s script] [-f frames] [-H hashEvery] [-t tag] [-w watchexpr] pad1 pad2 ...
# pad N = NPB3PAD=<N chars> in the environment (N=0: variable not set). Logs: build/np/B3/<tag>-pad<N>.log; result: first differing [hash] line per pad vs the first.
cd /home/claude/msc/np
B=/home/claude/msc/build/np
EXE=$B/host32/np-host32-full-null; SCRIPT=tools/det/../nav/nav_kickoff.txt; FR=6000; HE=30; TAG=sweep; TMO=200
while getopts "x:s:f:H:t:T:" o; do case $o in x) EXE=$OPTARG;; s) SCRIPT=$OPTARG;; f) FR=$OPTARG;; H) HE=$OPTARG;; t) TAG=$OPTARG;; T) TMO=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND-1))
[ -f "$SCRIPT" ] || SCRIPT=/home/claude/msc/tools/scripts/nav_kickoff.txt
for P in "$@"; do
  PADV=$(head -c $P /dev/zero | tr '\0' 'x')
  if [ "$P" = 0 ]; then unset NPB3PAD; else export NPB3PAD=$PADV; fi
  MSC_STATE_HASH=$HE MSC_VIRTUAL_TICK_NS=${TICK:-1000} TAILN=0 tools/runfull.sh -n ${TAG}-pad$P -V -X $EXE -s $SCRIPT -t $TMO -f $FR -e 100000 > /dev/null 2>&1
  cp $B/B2/${TAG}-pad$P.log $B/B3/${TAG}-pad$P.log; rm -rf $B/B2/data-${TAG}-pad$P $B/B2/frames-${TAG}-pad$P $B/B2/${TAG}-pad$P.log
  echo "pad $P: $(grep -c '^\[hash\]' $B/B3/${TAG}-pad$P.log) hash lines"
done
FIRST=$1
for P in "$@"; do
  [ "$P" = "$FIRST" ] && continue
  echo "== pad $FIRST vs pad $P: first differing line:"
  diff <(grep '^\[hash\]' $B/B3/${TAG}-pad$FIRST.log) <(grep '^\[hash\]' $B/B3/${TAG}-pad$P.log) | head -4
done
