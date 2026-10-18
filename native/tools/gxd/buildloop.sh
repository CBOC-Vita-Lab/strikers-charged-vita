#!/bin/bash
# GXD: build a variant in short heavy turns (resumable): tools/gxd/buildloop.sh <variant> <vgx dir>   (log: $B/job-build-<variant>.t<i>.log)
V=${1:?variant}; VGX=${2:?vgx dir}; shift 2
cd ${SRC:-/home/claude/msc/npw/W5GXD}
L=/home/claude/msc/build/np/W5GXD
for i in $(seq 1 12); do
  /home/claude/msc/tools/heavy.sh -t 590 tools/gxd/build.sh $V $VGX "$@" > $L/job-build-$V.t$i.log 2>&1
  rc=$?; echo "turn $i rc=$rc $(date +%T)"; tail -2 $L/job-build-$V.t$i.log
  [ $rc = 124 ] || break
done
echo BUILD-DONE $V
