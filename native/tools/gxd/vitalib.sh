#!/bin/bash
# GXD: the vgx library for the Vita (native/native-vita.mk with the flags of the package build) from a snapshot of a branch of the dev worktree;
# compiles all 18 vgx sources with VitaSDK's clang flags, prints the archive, deletes the objects afterwards (disk).
#   tools/heavy.sh -t 590 tools/gxd/vitalib.sh [branch, default w5gxd-cand]
BR=${1:-w5gxd-cand}
L=/home/claude/msc/build/np/W5GXD
SRC=${SRC:-/home/claude/msc/npw/W5GXD}
if [ -e $L/skip-vitalib ]; then echo "== vitalib skipped ($L/skip-vitalib)"; exit 0; fi
cd $SRC || exit 1
D=$(bash tools/gxd/snap.sh vlib $BR) || exit 1
echo "== vitalib $BR $(git -C ${DEV:-/home/claude/msc/npw/W5GXD-vgx-dev} rev-parse --short $BR) $(date +%T)"
rm -rf $L/vitalib; mkdir -p $L/vitalib
make -k -j2 -C $D -f native/native-vita.mk NOUT=$L/vitalib 'EXTRA=-fno-short-enums -funsigned-char -fshort-wchar' 2>&1 | grep -a -v '^In file\|warning:' | tail -${TAILN:-25}
rc=${PIPESTATUS[0]}
ls -la $L/vitalib/vita-vitagl/*.a 2>&1 | tail -2
du -sh $L/vitalib | tail -1
echo "== vitalib rc=$rc $(date +%T)"
rm -rf $L/vitalib $D
exit $rc
