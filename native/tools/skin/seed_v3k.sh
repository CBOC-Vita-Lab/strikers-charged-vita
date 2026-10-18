#!/bin/bash
# (copied into the repo from /home/claude/msc/npw/SKIN.v2.sh; run as one heavy.sh turn: tools/heavy.sh -t 1500 tools/skin/seed_v3k.sh, STEP=seed for the seed only)
# SKIN v2 (ONE heavy turn, NOHEAVY inside): shader keys of the switches -> delta list -> Vita3K delta compile in my own profile (pre-filled with the W5SEED seed's
# GXPs) -> seed-skin -> test packages (seed added, nothing rebuilt) -> Vita3K match60 in a FRESH profile with the seeded lab package and both switches on.
#   heavy.sh -t 1500 /home/claude/msc/npw/SKIN.v2.sh            STEP=seed|match (default both)
cd /home/claude/msc/npw/W5SKIN || exit 1
R=/home/claude/msc/build/np/W5SKIN; S=$R/seed; U=$S/union; mkdir -p $U
W=/home/claude/msc/build/np/W5SEED/seed-core/shadercache
LAB=$R/lto/vita/pkg/np/mscharged-np.vpk; PLAYER=$R/lto/vita/pkg-player/np/mscharged-np.vpk
SC=$R/tools/seedcheck
export NOHEAVY=1
pack() { # in.vpk seed-dir out.vpk  (= tools/seed/pack_vpk.sh with the temp dir under my build root)
  local T; T=$(mktemp -d $R/packXXXX); cp "$1" "$3"; zip -q -d "$3" 'shadercache/*' 2>/dev/null
  mkdir -p $T/shadercache; cp -a "$2"/. $T/shadercache/; ( cd $T && zip -q -r -X -D "$3" shadercache ); rm -rf $T
  echo "$3: $(stat -c %s "$3") bytes ($(stat -c %s "$1") before), $(unzip -l "$3" | grep -c 'shadercache/.*gxp') gxp, libshacccg entries: $(unzip -l "$3" | grep -ci shacccg)"; }
if [ "${STEP:-seed}" != match ]; then
  echo "== lists $(date +%T): $(ls $S/rec/*.done | wc -l) scenarios done"
  python3 -I tools/seed/shl.py merge -o $U/skin.shl $S/rec/*.shl > /dev/null
  python3 -I tools/seed/shl.py info $U/skin.shl
  python3 -I tools/seed/newkeys.py tools/seed/catalogue.shl $U/new_skin.shl $S/rec/*.shl | tee $U/new_skin.txt
  python3 -I tools/seed/shl.py merge -o $U/catalogue_skin.shl tools/seed/catalogue.shl $U/skin.shl > /dev/null
  python3 -I tools/seed/shl.py info $U/catalogue_skin.shl
  P=$R/v3k
  if [ ! -d $P/pref/ux0/data/mscharged/shadercache/bc447993 ]; then mkdir -p $P/pref/ux0/data/mscharged/shadercache; cp -a $W/bc447993 $P/pref/ux0/data/mscharged/shadercache/; fi
  echo "profile cache before: $(find $P/pref/ux0/data/mscharged/shadercache -name '*.gxp' | wc -l) gxp"
  if [ -s $U/new_skin.shl ]; then
    mkdir -p $R/v3k-in; cp $U/new_skin.shl $R/v3k-in/warm_list.bin
    printf '%s\n' "# SKIN: delta warm-up" "MSC_VGX_SHADERLIST=ux0:data/mscharged/warm_list.bin" "MSC_WATCH=1" "MSC_WATCH_EVERY=120" "MSC_VITA_RENDER=320x224" \
      "NP_DATA_DIR=ux0:data/mscharged/np-data" "MSC_MAX_FRAMES=60" > $R/v3k-in/env-delta.txt
    touch $R/v3k-in/mark
    KEEPDATA=1 FILES="$R/v3k-in/warm_list.bin" RUNS_DIR=$R/v3k-runs PROFILE_DIR=$P GAME=1 platform/vita/tools/job_v3k.sh delta 400 $R/v3k-in/env-delta.txt \
      "reached MSC_MAX_FRAMES|script exit|CRASH|STALL" $LAB MSCN00001 | tail -4
    echo "GXPs compiled in the delta run: $(find $P/pref/ux0/data/mscharged/shadercache -name '*.gxp' -newer $R/v3k-in/mark | wc -l); log: $(grep -a -m3 'frame 1:\|shader work' $R/v3k-runs/delta/log.txt | tr '\n' ' ')"
  fi
  rm -rf $R/seed-skin; mkdir -p $R/seed-skin
  SEEDCHECK=$SC tools/seed/make_seed.sh $P/pref tools/seed/core.shl $R/seed-skin/shadercache bc447993 || echo "SEED: make_seed reported a problem"
  cp -a $R/seed-skin/shadercache/bc447993 $R/seed-skin/shadercache/30223cc9
  echo "catalogue + skin keys against the seed: $($SC $U/catalogue_skin.shl $R/seed-skin/shadercache/bc447993/v2 | tail -1)"
  echo "skin keys alone against the seed:       $($SC $U/skin.shl $R/seed-skin/shadercache/bc447993/v2 | tail -1)"
  mkdir -p $R/pkg
  pack $LAB $R/seed-skin/shadercache $R/pkg/lab-skin.vpk
  pack $PLAYER $R/seed-skin/shadercache $R/pkg/player-skin.vpk
fi
if [ "${STEP:-match}" != seed ]; then
  echo "== Vita3K match60, fresh profile, seeded lab package, both switches on $(date +%T)"
  rm -rf $R/v3k-m
  FILES="platform/vita/session/nav/match60.txt" RUNS_DIR=$R/v3k-runs PROFILE_DIR=$R/v3k-m GAME=1 platform/vita/tools/job_v3k.sh m-on 560 \
    platform/vita/session/env/env-np-skin-on.txt "script end|script exit|CRASH|STALL" $R/pkg/lab-skin.vpk MSCN00001 | tail -6
  C=$R/v3k-m/pref/ux0/data/mscharged/shadercache; M=$(ls $C/*/seeded 2>/dev/null | head -1)
  echo "match: gxp in cache $(find $C -name '*.gxp' | wc -l); written after the seed copy (libshacccg compiles): $([ -n "$M" ] && find $C -name '*.gxp' -newer $M | wc -l || echo 'no seed marker')"
  grep -a "\[softinst\]\|GPU skinning\|FAILED\|STALL\|CRASH\|script end\|vitaGL:" $R/v3k-runs/m-on/log.txt | tail -12
fi
[ "${STEP:-x}" != seed ] && { echo "== qemu: final armhf exe with both switches off (the off path's cost) $(date +%T)"; /home/claude/msc/npw/W5SKIN/tools/skin/qsurvey.sh before2; }
echo "== SKIN.v2 done $(date +%T)"
