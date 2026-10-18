#!/bin/bash
# W5-SEED: the first Vita3K turn of the end game (one heavy.sh turn, ~15 minutes): final lists, delta compile (the profile `v3k` of the compile runs is kept),
# seeds (full list eager / core list eager), packages, and the proof runs of two stadiums the old seed never saw, with the FULL seed.
#   NOVITA=1 final_v1.sh   : everything but the emulator runs (a dry run of the file handling)
cd "$(dirname "$0")/../.."
R=/home/claude/msc/build/np/W5SEED
LAB=/home/claude/msc/build/np/W5INT/lto/vita/pkg/np/mscharged-np.vpk
PLAYER=/home/claude/msc/build/np/W5INT/lto/vita/pkg-player/np/mscharged-np.vpk
V=; [ "${NOVITA:-0}" = 1 ] && V=echo
U=$R/union/final
echo "== lists ($(date +%T))"
tools/seed/final_lists.sh $U || exit 1
echo "== delta compile ($(date +%T))"
$V tools/seed/v3k_warm.sh -k -n 90 -s 520 d1 $U/final.shl
echo "== seeds ($(date +%T))"
for t in full core; do
  L=$U/final.shl; [ $t = core ] && L=$U/core.shl
  rm -rf $R/seed-$t; mkdir -p $R/seed-$t
  tools/seed/make_seed.sh $R/v3k/pref $L $R/seed-$t/shadercache bc447993 || { echo "SEED $t: make_seed reported a problem"; [ $t = full ] && FULLBAD=1; }
  cp -a $R/seed-$t/shadercache/bc447993 $R/seed-$t/shadercache/30223cc9    # the perf vitaGL's key: the old seed had identical contents under both
  tools/seed/pack_vpk.sh $LAB $R/seed-$t/shadercache $R/pkg/lab-$t.vpk
  tools/seed/pack_vpk.sh $PLAYER $R/seed-$t/shadercache $R/pkg/player-$t.vpk
done
{ echo "lab (no seed): $(stat -c %s $LAB)"; echo "player (old seed): $(stat -c %s $PLAYER)"; for t in full core; do echo "player-$t: $(stat -c %s $R/pkg/player-$t.vpk)  seed files: $(find $R/seed-$t/shadercache/bc447993 -name '*.gxp' | wc -l) per key, folders: $(find $R/seed-$t/shadercache/bc447993 -type d | wc -l)"; done; } | tee $R/pkg/sizes.txt
[ "${FULLBAD:-0}" = 1 ] && { echo "full seed incomplete: another compile turn is needed (v3k_warm.sh -k ...); no proofs now"; exit 3; }
echo "== proofs, full seed ($(date +%T), used $SECONDS s)"
$V tools/seed/v3k_proof.sh -s 420 -v $R/pkg/lab-full.vpk -S $R/nav/stad_kong6.txt full_kong6
if [ $SECONDS -lt 900 ]; then $V tools/seed/v3k_proof.sh -s 420 -v $R/pkg/lab-full.vpk -S $R/nav/stad_sand15.txt full_sand15; else echo "no time left: full_sand15 goes to the next turn"; fi
echo "== done ($(date +%T), used $SECONDS s)"
