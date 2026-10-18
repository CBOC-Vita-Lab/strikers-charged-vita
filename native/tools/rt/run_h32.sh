#!/bin/bash
# W5 RT: one scripted run of a host32 executable (virtual clock), with the GL census csv and (GL builds) dumped pictures.
#   tools/rt/run_h32.sh <exe> <tag> [max frames] [dump list]       env: EXTRA_ENV="A=1 B=2", SCRIPT=<nav script>, TMO=<s>, OUTROOT=<dir>
# The PNGs are hashed (pics.md5) and deleted except KEEP (a comma list of frame numbers). Not a heavy turn by itself: call it inside one.
EXE=${1:?exe}; TAG=${2:?tag}; FR=${3:-4600}; LIST=${4:-}; SCRIPT=${SCRIPT:-/home/claude/msc/tools/scripts/nav_match60.txt}
OUTROOT=${OUTROOT:-/home/claude/msc/build/np/W5RT/runs}; OUT=$OUTROOT/$TAG; rm -rf $OUT; mkdir -p $OUT
D=$OUT/data; mkdir -p $D; cp -a /home/claude/msc/build/np/B3/rundata-save/. $D/
cd $D
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$SCRIPT MSC_MAX_FRAMES=$FR MSC_VIRTUAL_TIME=1 MSC_FRAME_LOG=100000 MSC_WATCH=1 MSC_WATCH_EVERY=100000 MSC_STATE_HASH=${HASHEVERY:-30} NP_DET_NAN=1
export MSC_VGX_GLSTATS=$OUT/glstats.csv
if [ -n "$LIST" ]; then
  export MSC_DUMP_DIR=$OUT/pics MSC_DUMP_LIST=$LIST
  R=/home/claude/msc/build/np/NPR/i386apt/root; L=$R/usr/lib/i386-linux-gnu
  export LD_LIBRARY_PATH=$L LIBGL_DRIVERS_PATH=$L/dri __EGL_VENDOR_LIBRARY_DIRS=$R/usr/share/glvnd/egl_vendor.d EGL_PLATFORM=surfaceless GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=1
  export MSC_VGX_LEAD=0 MSC_FRAMESEND=nofork
fi
[ -n "$EXTRA_ENV" ] && export $EXTRA_ENV
( time timeout -k 3 ${TMO:-420} $EXE ) > $OUT/log.txt 2>&1; echo "rc=$?" >> $OUT/log.txt
rm -rf $D
if [ -d $OUT/pics ]; then
  ( cd $OUT/pics && md5sum *.png > ../pics.md5 )
  KEEPRE=$(echo "${KEEP:-}" | tr ',' '\n' | sed 's/^/frame_0*/;s/$/\\.png/' | paste -sd'|')
  ( cd $OUT/pics && for f in *.png; do echo "$f" | grep -qE "^(${KEEPRE:-none})$" || rm -f "$f"; done )
fi
echo "$TAG: $(grep -c . $OUT/pics.md5 2>/dev/null) pictures; $(tail -1 $OUT/log.txt)"
