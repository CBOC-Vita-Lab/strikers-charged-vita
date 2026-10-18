#!/bin/bash
# W5-SEED: one Vita3K run of a package (with or without a seed inside) through a nav script, with the package's OWN warm-up list (no MSC_VGX_SHADERLIST override):
# what a player's console would do after installing it. The profile is FRESH (KEEPDATA=0: a new install: the seed is copied from app0, the list from the package).
#   tools/seed/v3k_proof.sh [-k] [-s seconds] [-v vpk] [-S nav script] [-x "VAR=val VAR=val"] [-P profile dir] <tag>
#   -k   keep the profile's data (second start of the same install: the cache and the list exist, the seed marker matches)
# Run it inside a heavy.sh turn (NOHEAVY=1 is set):  tools/heavy.sh -t 900 tools/seed/v3k_proof.sh ...
# Result: build/np/W5SEED/v3k-runs/<tag>/ (log.txt, report.txt = v3k_report.py, cache.txt = how many GXP files this run had to compile with libshacccg).
cd "$(dirname "$0")/../.."
R=/home/claude/msc/build/np/W5SEED
KEEP=0; SECS=600; VPK=; SCRIPT=; XENV=; PROF=$R/v3k-proof
while getopts "ks:v:S:x:P:" o; do case $o in k) KEEP=1;; s) SECS=$OPTARG;; v) VPK=$OPTARG;; S) SCRIPT=$OPTARG;; x) XENV=$OPTARG;; P) PROF=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND-1))
TAG=${1:?tag}; : ${VPK:?-v vpk}
mkdir -p $R/v3k-in
ENVF=$R/v3k-in/env-$TAG.txt
FILES=
{
  echo "# W5-SEED proof run $TAG of $VPK"
  echo "MSC_WATCH=1"; echo "MSC_WATCH_EVERY=300"
  echo "MSC_VITA_RENDER=320x224"
  echo "NP_DATA_DIR=ux0:data/mscharged/np-data"
  if [ -n "$SCRIPT" ]; then cp "$SCRIPT" $R/v3k-in/; FILES="$R/v3k-in/$(basename $SCRIPT)"; echo "MSC_INPUT_SCRIPT=$(basename $SCRIPT)"; echo "MSC_MAX_FRAMES=200000"; else echo "MSC_MAX_FRAMES=120"; fi
  for kv in $XENV; do echo "$kv"; done
} > $ENVF
UNTIL="reached MSC_MAX_FRAMES|script exit|CRASH|STALL"
NOHEAVY=1 KEEPDATA=$KEEP FILES="$FILES" RUNS_DIR=$R/v3k-runs PROFILE_DIR=$PROF GAME=1 platform/vita/tools/job_v3k.sh "$TAG" "$SECS" "$ENVF" "$UNTIL" "$VPK" MSCN00001
OUT=$R/v3k-runs/$TAG
C=$PROF/pref/ux0/data/mscharged/shadercache
M=$(ls $C/*/seeded 2>/dev/null | head -1)
{
  echo "package: $VPK ($(stat -c %s "$VPK") bytes)"
  echo "gxp files in the cache at the end: $(find $C -name '*.gxp' 2>/dev/null | wc -l)"
  if [ -n "$M" ]; then echo "written after the package's seed was copied (= compiled by libshacccg in this run): $(find $C -name '*.gxp' -newer $M | wc -l)"; else echo "no seed marker: the package carries no seed for this key"; fi
} | tee $OUT/cache.txt
[ -f $OUT/log.txt ] && python3 -I tools/seed/v3k_report.py $OUT/log.txt > $OUT/report.txt 2>&1
cat $OUT/report.txt 2>/dev/null | head -60
