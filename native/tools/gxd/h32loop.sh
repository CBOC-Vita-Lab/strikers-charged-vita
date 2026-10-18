#!/bin/bash
# GXD: tools/gxd/h32loop.sh <variant> <vgx dir>: the host32 build (h32.sh) in short heavy turns (resumable); log build/np/W5GXD/job-h32-<variant>.t<i>.log
V=${1:?variant}; VGX=${2:?vgx dir}; shift 2
cd ${SRC:-/home/claude/msc/npw/W5GXD}
L=/home/claude/msc/build/np/W5GXD
for i in $(seq 1 8); do
  /home/claude/msc/tools/heavy.sh -t 590 tools/gxd/h32.sh $V $VGX "$@" > $L/job-h32-$V.t$i.log 2>&1
  rc=$?; echo "turn $i rc=$rc $(date +%T)"; tail -2 $L/job-h32-$V.t$i.log
  [ $rc = 124 ] || break
done
echo H32-DONE $V
