#!/bin/bash
# runq2.sh: ordered picture queue (sizes: f = fraction of 960x544 per side, 16:9; s = the same pixels in the game's 640x448 shape).
export PICS_NOBUILD=1 PICS_LEAD=2
export PICS_FRAMES=720,1300,2040,2041,2400,2520,2760,3120,3600,3601
P=/home/claude/s13/tools/pics.sh; T=/home/claude/s13/strikers
until [ -f /home/claude/work/pics/look-r100/960x544/frame_003601.png ]; do sleep 5; done
run() { tag=$1; size=$2; shift 2
  [ -f /home/claude/work/pics/$tag/$size/frame_003601.png ] && { echo "skip $tag"; return; }
  PICS_SIZES="$size" $P $T $tag MSC_MAX_FRAMES=3610 "$@" ; }
B60="MSC_VGX_LODBIAS_RUNG=60"; B0="MSC_VGX_LODBIAS_RUNG=0"
for s in 691x392 595x337 499x283 749x424 826x468 442x250; do run look-$s-b60 $s $B60; done
for s in 691x392 595x337 499x283 749x424 826x468 442x250; do run look-$s-b0 $s $B0; done
run look-gl691 691x392 $B60 MSC_VGX_SCREEN=960x544 MSC_VITA_UPSCALE=combined MSC_VITA_SHARPEN=0.62 MSC_VITA_UPSCALE_EXTRA=linear,fast,smooth,cas
for s in 595x337 691x392 499x283 442x250; do run look-$s-msaa2 $s $B60 MSC_VGX_MSAA=2; done
for s in 691x392 499x283; do run look-$s-b40 $s MSC_VGX_LODBIAS_RUNG=40; run look-$s-b80 $s MSC_VGX_LODBIAS_RUNG=80; done
for s in 622x436 535x375 448x314; do run look-$s-b60 $s $B60; done
run look-448x314-b0 448x314 $B0
echo ALLDONE
