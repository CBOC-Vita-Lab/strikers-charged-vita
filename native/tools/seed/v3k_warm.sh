#!/bin/bash
# W5-SEED: one Vita3K run of a package that warms up a shader list (MSC_VGX_SHADERLIST) at its first frame; the shaders go to the profile's vitaGL cache.
#   tools/seed/v3k_warm.sh [-k] [-n frames] [-s seconds] [-v vpk] [-S script] [-x "VAR=val VAR=val"] <tag> <list.shl>
#   -k          keep the profile's data (the cache of the earlier turn): a list too big for one turn is finished in the next
#   -S script   a nav script played after the warm-up (copied into ux0:data/mscharged), default none; the run ends at the script's `exit`
#   -x          more environment lines for env.txt
# (1.3 workspace: no heavy.sh; takes the emulator lock itself; SEED_ROOT=<folder> holds the private profile v3kp/ and the runs v3k-runs/)
# Result: build/np/W5SEED/v3k-runs/<tag>/ (log.txt = the game's log: look for "frame 1: N shader compiles", "shader work so far"), the profile in $SEED_ROOT/v3kp.
cd "$(dirname "$0")/../.."
R=${SEED_ROOT:-/work/msc/build/np/W5SEED}   # (SEED_ROOT=<folder> keeps the work somewhere else, e.g. under the build root of this checkout)
# DATA: the title's data folder in the emulator profile. 1.3 workspace: the staged build is always the isolated player build CDXMP0001 (stage-hardware-test.py
# overrides NP_PLAYER and the title id), whose data root is ux0:data/mscharged-perf-test and which reads np-player.txt; MSCN00001 (a true lab build) would be mscharged.
DATA=${SEED_DATA:-mscharged-perf-test}
KEEP=0; FR=240; SECS=700; VPK=/work/msc/build/np/W5INT/lto/vita/pkg/np/mscharged-np.vpk; SCRIPT=; XENV=
while getopts "kn:s:v:S:x:" o; do case $o in k) KEEP=1;; n) FR=$OPTARG;; s) SECS=$OPTARG;; v) VPK=$OPTARG;; S) SCRIPT=$OPTARG;; x) XENV=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND-1))
TAG=${1:?tag}; LIST=${2:?list}
mkdir -p $R/v3k-in
cp "$LIST" $R/v3k-in/warm_list.bin
FILES="$R/v3k-in/warm_list.bin"
ENVF=$R/v3k-in/env-$TAG.txt
{
  echo "# W5-SEED: warm-up of $LIST"
  echo "MSC_VGX_SHADERLIST=ux0:data/$DATA/warm_list.bin"
  echo "MSC_WATCH=1"; echo "MSC_WATCH_EVERY=120"
  case " $XENV " in *" MSC_VITA_RENDER="*) ;; *) echo "MSC_VITA_RENDER=320x224";; esac   # (-x "MSC_VITA_RENDER=..." replaces the small picture: dynamic resolution and the multisampled rungs need none)
  echo "NP_DATA_DIR=ux0:data/$DATA/np-data"
  if [ -n "$SCRIPT" ]; then cp "$SCRIPT" $R/v3k-in/; FILES="$FILES $R/v3k-in/$(basename $SCRIPT)"; echo "MSC_INPUT_SCRIPT=ux0:data/$DATA/$(basename $SCRIPT)"; echo "MSC_MAX_FRAMES=200000"; else echo "MSC_MAX_FRAMES=$FR"; fi
  for kv in $XENV; do [ "$kv" = "MSC_VITA_RENDER=" ] || echo "$kv"; done   # (-x MSC_VITA_RENDER= alone: no fixed picture size at all)
} > $ENVF
UNTIL="${SEED_UNTIL:-reached MSC_MAX_FRAMES|script exit|CRASH}"   # (no STALL: with the real compiler in software GL the first frame of a big list takes minutes)
# 1.3 workspace: the headless emulator through v3k_emu.sh, in the private profile $R/v3kp (see v3k_emu.sh), under the shared emulator lock.
# (the old job_v3k.sh / heavy.sh of the 1.2 workspace do not exist here)
export V3K_PREF=$R/v3kp/pref V3K_XDG=$R/v3kp/xdg V3K_RUNS=$R/v3k-runs
mkdir -p $R/v3k-runs
TAG=$TAG FILES="$FILES" KEEPDATA=$KEEP UNTIL="$UNTIL" SHOTEVERY=${SHOTEVERY:-120} flock /home/claude/work/v3k/emu.lock nice -n 5 \
  $(pwd)/tools/seed/v3k_emu.sh "$VPK" "$ENVF" "$SECS" > $R/v3k-runs.$TAG.out 2>&1
tail -4 $R/v3k-runs.$TAG.out
C=$R/v3kp/pref/ux0/data/$DATA/shadercache
M=$(ls $C/*/seeded 2>/dev/null | head -1)
{
  echo "gxp files in the cache at the end: $(find $C -name '*.gxp' | wc -l)"
  if [ -n "$M" ]; then echo "written after the package's seed was copied (= compiled by libshacccg in this run): $(find $C -name '*.gxp' -newer $M | wc -l)"; else echo "no seed marker: the package carries no seed for this key"; fi
} | tee $R/v3k-runs/$TAG/cache.txt
