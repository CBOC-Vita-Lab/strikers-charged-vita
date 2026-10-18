#!/bin/bash
# W5-SEED: the held-out whole-match turn (one heavy.sh turn, ~15-20 minutes):
#   1. hl_: 20 fresh random WHOLE 3-minute matches (never in the union): how many uncatalogued shaders does a new match meet?
#   2. the new keys are folded into the catalogue; when there are new SHADERS a Vita3K delta run compiles just those (the profile `v3k` keeps the cache of final_v1's compile)
#   3. the seed directories and packages are made again from the extended cache (core list eager, as in final_v1)
#   NOBATCH=1 : skip step 1 (use the finished rec/ ones)   NOVITA=1 : no emulator run   TEST=1 : write into build/np/W5SEED/test-final3 instead of seed-core / pkg
#   HL=<glob> : which runs are the held-out set (default hl_*; a test can use hx_*)
cd "$(dirname "$0")/../.."
R=/home/claude/msc/build/np/W5SEED
LAB=/home/claude/msc/build/np/W5INT/lto/vita/pkg/np/mscharged-np.vpk
PLAYER=/home/claude/msc/build/np/W5INT/lto/vita/pkg-player/np/mscharged-np.vpk
HL=${HL:-hl_*}
V=; [ "${NOVITA:-0}" = 1 ] && V=echo
U=$R/union/final
OUT=$R/seed-core; PK=$R/pkg
if [ "${TEST:-0}" = 1 ]; then OUT=$R/test-final3/seed-core; PK=$R/test-final3/pkg; U=$R/test-final3/union; mkdir -p $U $PK; cp $R/union/final/final.shl $R/union/final/core.shl $U/; fi
[ "${NOBATCH:-0}" = 1 ] || tools/seed/batch.sh -b 800 $R/runs_hl.txt
echo "== held-out whole matches ($(date +%T))"
python3 -I tools/seed/shl.py table --base $U/final.shl $R/rec/$HL.shl | tee $U/hl_table.txt | tail -25
python3 -I tools/seed/newkeys.py $U/final.shl $U/new_hl.shl $R/rec/$HL.shl | tee $U/hl_new.txt
python3 -I tools/seed/shl.py merge -o $U/catalogue.shl $U/final.shl $R/rec/$HL.shl > /dev/null
python3 -I tools/seed/shl.py info $U/catalogue.shl | tee $U/catalogue_info.txt
if [ -s $U/new_hl.shl ]; then
  echo "== delta compile of the new shaders ($(date +%T))"
  $V tools/seed/v3k_warm.sh -k -n 60 -s 300 d2 $U/new_hl.shl
  echo "== seed again ($(date +%T))"
  rm -rf $OUT; mkdir -p $OUT
  tools/seed/make_seed.sh $R/v3k/pref $U/core.shl $OUT/shadercache bc447993 || echo "SEED: make_seed reported a problem"
  cp -a $OUT/shadercache/bc447993 $OUT/shadercache/30223cc9
  SC=/home/claude/msc/build/np/W5SEED/tools/seedcheck
  echo "catalogue check against the seed's cache: $($SC $U/catalogue.shl $OUT/shadercache/bc447993/v2 | tail -1)"
  mkdir -p $PK
  tools/seed/pack_vpk.sh $LAB $OUT/shadercache $PK/lab-core.vpk
  tools/seed/pack_vpk.sh $PLAYER $OUT/shadercache $PK/player-core.vpk
  ls -l $PK/*.vpk
else
  echo "no new shader: the seed stays as it is"
fi
echo "== done ($(date +%T), used $SECONDS s)"
