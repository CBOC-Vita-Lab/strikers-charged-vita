#!/bin/bash
# runq3: after finding that the host harness ignores MSC_VGX_LODBIAS_RUNG (bias is applied only in the Vita build: b0 == b60 bit for bit), only runs that can differ.
export PICS_NOBUILD=1 PICS_LEAD=2
export PICS_FRAMES=720,1300,2040,2041,2400,2520,2760,3120,3600,3601
P=/home/claude/s13/tools/pics.sh; T=/home/claude/s13/strikers
run() { tag=$1; size=$2; shift 2
  [ -f /home/claude/work/pics/$tag/$size/frame_003601.png ] && { echo "skip $tag"; return; }
  PICS_SIZES="$size" $P $T $tag MSC_MAX_FRAMES=3610 "$@" ; }
B60="MSC_VGX_LODBIAS_RUNG=60"
run look-gl691 691x392 $B60 MSC_VGX_SCREEN=960x544 MSC_VITA_UPSCALE=combined MSC_VITA_SHARPEN=0.62 MSC_VITA_UPSCALE_EXTRA=linear,fast,smooth,cas
for s in 595x337 691x392 499x283 442x250; do run look-$s-msaa2 $s $B60 MSC_VGX_MSAA=2; done
for s in 622x436 535x375 448x314; do run look-$s-b60 $s $B60; done
echo ALLDONE
