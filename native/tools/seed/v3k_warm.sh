#!/bin/bash
# W5-SEED: one Vita3K run of a package that warms up a shader list (MSC_VGX_SHADERLIST) at its first frame; the shaders go to the profile's vitaGL cache.
#   tools/seed/v3k_warm.sh [-k] [-n frames] [-s seconds] [-v vpk] [-S script] [-x "VAR=val VAR=val"] <tag> <list.shl>
#   -k          keep the profile's data (the cache of the earlier turn): a list too big for one turn is finished in the next
#   -S script   a nav script played after the warm-up (copied into ux0:data/mscharged), default none; the run ends at the script's `exit`
#   -x          more environment lines for env.txt
# Needs the heavy.sh turn to be held by the caller (NOHEAVY=1 is set) -> run it as:  tools/heavy.sh -t 900 tools/seed/v3k_warm.sh ...
# Result: build/np/W5SEED/v3k-runs/<tag>/ (log.txt = the game's log: look for "frame 1: N shader compiles", "shader work so far"), the profile in build/np/W5SEED/v3k.
cd "$(dirname "$0")/../.."
R=/home/claude/msc/build/np/W5SEED
KEEP=0; FR=240; SECS=700; VPK=/home/claude/msc/build/np/W5INT/lto/vita/pkg/np/mscharged-np.vpk; SCRIPT=; XENV=
while getopts "kn:s:v:S:x:" o; do case $o in k) KEEP=1;; n) FR=$OPTARG;; s) SECS=$OPTARG;; v) VPK=$OPTARG;; S) SCRIPT=$OPTARG;; x) XENV=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND-1))
TAG=${1:?tag}; LIST=${2:?list}
mkdir -p $R/v3k-in
cp "$LIST" $R/v3k-in/warm_list.bin
FILES="$R/v3k-in/warm_list.bin"
ENVF=$R/v3k-in/env-$TAG.txt
{
  echo "# W5-SEED: warm-up of $LIST"
  echo "MSC_VGX_SHADERLIST=ux0:data/mscharged/warm_list.bin"
  echo "MSC_WATCH=1"; echo "MSC_WATCH_EVERY=120"
  echo "MSC_VITA_RENDER=320x224"
  echo "NP_DATA_DIR=ux0:data/mscharged/np-data"
  if [ -n "$SCRIPT" ]; then cp "$SCRIPT" $R/v3k-in/; FILES="$FILES $R/v3k-in/$(basename $SCRIPT)"; echo "MSC_INPUT_SCRIPT=$(basename $SCRIPT)"; echo "MSC_MAX_FRAMES=200000"; else echo "MSC_MAX_FRAMES=$FR"; fi
  for kv in $XENV; do echo "$kv"; done
} > $ENVF
UNTIL="reached MSC_MAX_FRAMES|script exit|CRASH|STALL"
NOHEAVY=1 KEEPDATA=$KEEP FILES="$FILES" RUNS_DIR=$R/v3k-runs PROFILE_DIR=$R/v3k GAME=1 platform/vita/tools/job_v3k.sh "$TAG" "$SECS" "$ENVF" "$UNTIL" "$VPK" MSCN00001
C=$R/v3k/pref/ux0/data/mscharged/shadercache
M=$(ls $C/*/seeded 2>/dev/null | head -1)
{
  echo "gxp files in the cache at the end: $(find $C -name '*.gxp' | wc -l)"
  if [ -n "$M" ]; then echo "written after the package's seed was copied (= compiled by libshacccg in this run): $(find $C -name '*.gxp' -newer $M | wc -l)"; else echo "no seed marker: the package carries no seed for this key"; fi
} | tee $R/v3k-runs/$TAG/cache.txt
