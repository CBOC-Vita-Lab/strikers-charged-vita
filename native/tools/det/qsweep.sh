#!/bin/bash
# armhf/qemu environment sweep: qrun.sh once per pad size, then the first differing [hash] line against the first run.
#   heavy.sh -t 900 tools/det/qsweep.sh -t tag [qrun options except -n/-p] -- pad1 pad2 ...
B=/home/claude/msc/build/np; cd /home/claude/msc/np
TAG=qs; OPTS=()
while [ $# -gt 0 ]; do case $1 in -t) TAG=$2; shift 2;; --) shift; break;; *) OPTS+=("$1"); shift;; esac; done
for P in "$@"; do tools/det/qrun.sh "${OPTS[@]}" -n $TAG-pad$P -p $P | tail -1; done
F=$1
for P in "$@"; do
  [ "$P" = "$F" ] && continue
  echo "== $TAG pad $F vs pad $P:"; diff <(grep -a '^\[hash\]' $B/B3/$TAG-pad$F.log) <(grep -a '^\[hash\]' $B/B3/$TAG-pad$P.log) | head -6
done
