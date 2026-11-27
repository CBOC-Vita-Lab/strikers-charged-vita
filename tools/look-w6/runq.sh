#!/bin/bash
# runq.sh: the look agent's picture queue. One pics.sh call per (tag, size, env); PICS_NOBUILD (prebuilt main-tree binary).
export PICS_NOBUILD=1 PICS_LEAD=2
export PICS_FRAMES=720,1300,2040,2041,2400,2520,2760,3120,3600,3601
P=/home/claude/s13/tools/pics.sh; T=/home/claude/s13/strikers
run() { tag=$1; size=$2; shift 2
  [ -f /home/claude/work/pics/$tag/$size/frame_003601.png ] && { echo "skip $tag"; return; }
  PICS_SIZES="$size" $P $T $tag MSC_MAX_FRAMES=3610 "$@" ; }
# order = priority
for pair in 826x468:r86 749x424:r78 691x392:r72 595x337:r62; do s=${pair%%:*}; n=${pair##*:}
  :; done
run look-r100 960x544 MSC_VGX_LODBIAS_RUNG=60
run look-r72-b60 691x392 MSC_VGX_LODBIAS_RUNG=60
run look-r62-b60 595x337 MSC_VGX_LODBIAS_RUNG=60
run look-r72-b0 691x392 MSC_VGX_LODBIAS_RUNG=0
run look-r78-b60 749x424 MSC_VGX_LODBIAS_RUNG=60
run look-r86-b60 826x468 MSC_VGX_LODBIAS_RUNG=60
# GL validation of the picture shaders (screen-size dumps), stretch of 0.72: k*sqrt(.389)=0.62
run look-gl72 691x392 MSC_VGX_LODBIAS_RUNG=60 MSC_VGX_SCREEN=960x544 MSC_VITA_UPSCALE=combined MSC_VITA_SHARPEN=0.62 MSC_VITA_UPSCALE_EXTRA=linear,fast,smooth,cas
run look-r62-b0 595x337 MSC_VGX_LODBIAS_RUNG=0
run look-r78-b0 749x424 MSC_VGX_LODBIAS_RUNG=0
run look-r86-b0 826x468 MSC_VGX_LODBIAS_RUNG=0
run look-r72-msaa2 691x392 MSC_VGX_LODBIAS_RUNG=60 MSC_VGX_MSAA=2
run look-r62-msaa2 595x337 MSC_VGX_LODBIAS_RUNG=60 MSC_VGX_MSAA=2
run look-r78-msaa2 749x424 MSC_VGX_LODBIAS_RUNG=60 MSC_VGX_MSAA=2
run look-r72-b40 691x392 MSC_VGX_LODBIAS_RUNG=40
run look-r72-b80 691x392 MSC_VGX_LODBIAS_RUNG=80
run look-r62-b40 595x337 MSC_VGX_LODBIAS_RUNG=40
run look-r62-b80 595x337 MSC_VGX_LODBIAS_RUNG=80
run look-s72-b60 622x436 MSC_VGX_LODBIAS_RUNG=60
run look-s62-b60 535x375 MSC_VGX_LODBIAS_RUNG=60
echo ALLDONE
