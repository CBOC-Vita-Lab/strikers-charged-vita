#!/bin/bash
# GXD: host32 null-back-end executable of the np worktree for ONE vgx source tree (game objects seeded from W4's host32 tree, see GXD.NOTES.md).
#   tools/heavy.sh -t 590 tools/gxd/h32.sh <variant> <vgx source dir>        -> build/np/W5GXD/h/host32/np-host32-full-null-<variant>
# Resumable (make is incremental). Libraries of the variant: build/np/W5GXD/h/host32/gx-<variant>.
V=${1:?variant}; VGX=${2:?vgx dir}; shift 2
SRC=${SRC:-/home/claude/msc/npw/W5GXD}
H=${GXDH:-/home/claude/msc/build/np/W5GXD/h}
L=$(dirname $H)
# no-op marker as in build.sh: touch $L/skip-<variant>
if [ -e $L/skip-$V ]; then echo "== h32 $V skipped ($L/skip-$V)"; exit 0; fi
cd $SRC || exit 1
# Late binding as in build.sh: L/late-<variant> = "<branch or commit> <new variant name>"
if [ -f $L/late-$V ]; then
  read BR NV < $L/late-$V
  bash $SRC/tools/gxd/snap.sh $V $BR > /dev/null || exit 1
  echo "$BR $(git -C ${DEV:-/home/claude/msc/npw/W5GXD-vgx-dev} rev-parse --short $BR)" > $L/late-$V.done
  echo $NV > $L/variant-$V; rm -f $L/late-$V
fi
[ -f $L/variant-$V ] && V=$(cat $L/variant-$V)
echo "== h32 $V vgx=$VGX $(date +%T)"
make -k -j2 TARGET=host32 BUILD_ROOT=$H GX_VGX=$VGX WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 GX_BACKEND=null EXESUF=-full-null-$V GX_BUILD=$H/host32/gx-$V OPT=-O2 DEBUG=-g0 "$@" 2>&1 \
  | grep -a -v '^clang\|^mkdir\|^g++\|^gcc\|^make\[\|^CXX\|^In file\|warning:\|^ *[0-9]* |\|\^~' | tail -${TAILN:-15}
rc=${PIPESTATUS[0]}
ls -la $H/host32/np-host32-full-null-$V 2>&1 | tail -1
echo "== h32 done $V rc=$rc $(date +%T)"
if [ $rc = 0 ] && [ -f $L/post-$V.sh ]; then echo "== post-$V.sh $(date +%T)"; bash $L/post-$V.sh; fi
exit $rc
