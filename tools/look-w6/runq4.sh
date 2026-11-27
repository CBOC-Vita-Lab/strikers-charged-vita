#!/bin/bash
export PICS_NOBUILD=1 PICS_LEAD=2
export PICS_FRAMES=720,1300,2040,2041,2400,2520,2760,3120,3600,3601
P=/home/claude/s13/tools/pics.sh; T=/home/claude/s13/strikers
until grep -q ALLDONE /home/claude/s13/w6/look/runq3.log; do sleep 10; done
run() { tag=$1; size=$2; shift 2
  [ -f /home/claude/work/pics/$tag/$size/frame_003601.png ] && { echo "skip $tag"; return; }
  PICS_SIZES="$size" $P $T $tag MSC_MAX_FRAMES=3610 "$@" ; }
B60="MSC_VGX_LODBIAS_RUNG=60"
run look-714x401-b60 714x401 $B60
run look-640x448-b60 640x448 $B60
run look-448x314-msaa2 448x314 $B60 MSC_VGX_MSAA=2
echo ALLDONE
