#!/bin/bash
# (copied into the repo from /home/claude/msc/npw/SKIN.q1.sh; tools/heavy.sh -t 2400 tools/skin/qsurvey.sh before|after5)
# SKIN: armhf qemu per-thread instruction counts, survey script (menus, walk-out, celebrations, replays), before (switches off) / after (both on).
# The GL back end over the do-nothing GL (glnull) on its own thread (MSC_VGX_BACKEND_THREAD=1): t0 game (+ frame send, nofork), t1 player (decode D), t2 back end (B, without vitaGL's own work).
cd /home/claude/msc/npw/W5SKIN
E=/home/claude/msc/build/np/W5SKIN/armhf/np-armhf-full
C="MSC_VGX_BACKEND_THREAD=1 MSC_VGX_PROMOSMALL=3 MSC_VGX_UNIDUP=2"
case "${1:-before}" in
  before) TMO=${TMO:-2300} tools/skin/qrun.sh $E qbefor ${FR:-6760} $C MSC_SKIN_GPU=0 MSC_VGX_SOFTINST=0 | tail -2;;
  after)  TMO=${TMO:-2300} tools/skin/qrun.sh $E qafter ${FR:-6760} $C MSC_SKIN_GPU=1 MSC_VGX_SOFTINST=16 | tail -2;;
  after2) TMO=${TMO:-2300} tools/skin/qrun.sh $E qafter2 ${FR:-6760} $C MSC_SKIN_GPU=1 MSC_VGX_SOFTINST=16 | tail -2;;
  after3) TMO=${TMO:-2300} tools/skin/qrun.sh $E qafter3 ${FR:-6760} $C MSC_SKIN_GPU=1 MSC_VGX_SOFTINST=16 | tail -2;;
  after4) TMO=${TMO:-2300} tools/skin/qrun.sh $E qafter4 ${FR:-6760} $C MSC_SKIN_GPU=1 MSC_VGX_SOFTINST=16 | tail -2;;
  after5) TMO=${TMO:-2300} tools/skin/qrun.sh $E qafter5 ${FR:-6760} $C MSC_SKIN_GPU=1 MSC_VGX_SOFTINST=16 | tail -2;;
  before2) TMO=${TMO:-2300} tools/skin/qrun.sh $E qbefor2 ${FR:-6760} $C MSC_SKIN_GPU=0 MSC_VGX_SOFTINST=0 | tail -2;;
  lane)   TMO=${TMO:-2300} tools/skin/qrun.sh $E qlane1 ${FR:-6760} $C MSC_SKIN_GPU=0 MSC_VGX_SOFTINST=0 MSC_SKIN_LANE=1 | tail -2;;
esac
