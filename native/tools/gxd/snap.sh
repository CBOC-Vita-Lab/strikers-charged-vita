#!/bin/bash
# GXD: stable source snapshot of a vgx commit for one build variant (builds queue for a long time; the development worktree keeps changing).
#   tools/gxd/snap.sh <variant> [commit-ish in the dev worktree, default HEAD]   -> prints the directory
V=${1:?variant}; C=${2:-HEAD}
DEV=${DEV:-/home/claude/msc/npw/W5GXD-vgx-dev}
OUT=/home/claude/msc/build/np/W5GXD/vgx-$V-src
rm -rf $OUT; mkdir -p $OUT
git -C $DEV archive $C | tar -x -C $OUT || exit 1
echo $OUT
