#!/bin/bash
# armdet.sh <plain|pgo> <tag> <seed> [frames] [route] : one deterministic armhf run under qemu-arm; trace -> /home/claude/work/pgo13/det/<tag>.trace
K=$1; T=$2; SEED=$3; N=${4:-4800}; ROUTE=${5:-platform/vita/session/nav/match60.txt}
W=/home/claude/work/pgo13; D=$W/det; mkdir -p $D; X=$W/arm-$K/armhf/np-armhf-full
R=/home/claude/s13/wt/pgo13/native; DD=$(mktemp -d /tmp/pgo13-det.XXXXXX); trap 'rm -rf $DD' EXIT
cd $DD && env NP_DISC_DIR=/home/claude/work/disc/extract-strikers NP_DATA_DIR=$DD MSC_INPUT_SCRIPT=$R/$ROUTE MSC_MAX_FRAMES=$N MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=33000 \
  MSC_FRAMESEND=nofork MSC_WATCH_EVERY=600 NP_DET_TRACE=$D/$T.trace NP_DET_PIN_SEED=$SEED $(grep -v '^#' $W/rec-arm/root/pgo.env | grep -E '^MSC_' | grep -vE '^MSC_(FPS|FRAMESEND|VGX_BACKEND_THREAD|FS_PIPE)=' | tr '\n' ' ') \
  nice -n 10 qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf $X > $D/$T.log 2>&1
echo "$T exit $? lines $(wc -l < $D/$T.trace)"
