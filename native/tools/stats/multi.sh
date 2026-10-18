#!/bin/bash
# Statistics over several seeds (MSC_VIRTUAL_TICK_NS changes the virtual clock the game seeds its random numbers from).
#   tools/stats/multi.sh native|oracle <firstSeed> <lastSeed> [outdir]
# one match of tools/nav/nav_stats.txt per seed; logs in <outdir>/<kind>_<seed>.log (default build/np/B2/stats).
# Run through heavy.sh (native: ~50 s per match, oracle: ~150 s). Then:  tools/stats/cmp.py n1=<outdir>/native_1.log o1=...
KIND=$1; A=$2; Z=$3; OUT=${4:-/home/claude/msc/build/np/B2/stats}
mkdir -p $OUT
NP=/home/claude/msc/np
for s in $(seq $A $Z); do
  TICK=$((1000 + 37 * s))
  if [ $KIND = native ]; then
    B=/home/claude/msc/build/np/host32
    MSC_VIRTUAL_TICK_NS=$TICK MSC_WATCH="$(python3 $NP/tools/stats/mkwatch.py native)" TAILN=0 $NP/tools/runfull.sh -n st_native_$s -V -X $B/np-host32-full-null -s $NP/tools/nav/nav_stats.txt -t 200 -f 30000 -e 100000 > /dev/null 2>&1
    cp /home/claude/msc/build/np/B2/st_native_$s.log $OUT/native_$s.log
  else
    D=/home/claude/msc/build/np/B2/oracle-data-$s; rm -rf $D; mkdir -p $D/MSCRecomp
    printf '[video]\nframe_rate = 60\n[paths]\ndvd_root = "/home/claude/msc/game"\n' > $D/MSCRecomp/Config.toml
    ( cd /home/claude/msc/wt/gx && XDG_DATA_HOME=$D MSC_INPUT_SCRIPT=$NP/tools/nav/nav_stats.txt MSC_WATCH="$(python3 $NP/tools/stats/mkwatch.py oracle)" MSC_WATCH_EVERY=100000 MSC_FRAME_LOG=100000 MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_TICK_NS=$TICK timeout 400 /home/claude/msc/build/host-gl/msc_null_hle > $OUT/oracle_$s.log 2>&1 )
    rm -rf $D
  fi
  echo "seed $s tick $TICK: $(grep -o 'score=[0-9]*-[0-9]*' $OUT/${KIND}_$s.log | tail -1)"
done
