#!/bin/bash
# Trial of the renderer bridge (WITH_GX=1) in a scratch copy of HEAD with NPR's three source patches applied.
# Builds armhf and host32 (objects, stubs, link with libnpsdk + libnpgx + libvgx-native) and smoke-runs them.
# Nothing is committed from here; if it works the patches are applied to main by hand.
#   usage (inside a heavy.sh slot): tools/job_gx.sh
cd /home/claude/msc/np
SC=/tmp/claude-0/-home-claude/2e068ce2-50b7-5168-bd6a-083a34fe1600/scratchpad/gxtry
B=/home/claude/msc/build/np
rm -rf $SC; mkdir -p $SC; git archive HEAD | tar -x -C $SC
cd $SC
for p in platform/gx/patches/000[123]*.patch; do git apply --whitespace=nowarn $p 2>&1 | head -2 | sed "s|^|FAIL $(basename $p): |"; done
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$SC/rundata
mkdir -p $NP_DATA_DIR
VARS="OPT=-O1 DEBUG=-g0 BUILD_ROOT=$SC/build WITH_SDK=1 WITH_GX=1 EXESUF=-sdk-gx"
for T in armhf host32; do
  echo "== gx $T objs $(date +%T)"
  make -k -j2 TARGET=$T $VARS objs 2>&1 | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-\|^ar \|^make\[' | tail -30
  echo "== gx $T stubs $(date +%T)"
  make TARGET=$T $VARS stubs 2>&1 | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-\|^ar \|^make\[\|^readelf\|^rm ' | tail -10
  echo "== gx $T link $(date +%T)"
  make -j2 TARGET=$T $VARS 2>&1 | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-\|^ar \|^make\[' | tail -30
  ls -la $SC/build/$T/np-$T-sdk-gx 2>&1 | tail -1
done
QEMU="/home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf"
echo "== smoke gx armhf $(date +%T)"
( timeout 90 $QEMU $SC/build/armhf/np-armhf-sdk-gx > $B/run-armhf-sdk-gx.log 2>&1; echo "rc=$?" >> $B/run-armhf-sdk-gx.log ); head -c 2500 $B/run-armhf-sdk-gx.log; echo
echo "== smoke gx host32 $(date +%T)"
( timeout 40 $SC/build/host32/np-host32-sdk-gx > $B/run-host32-sdk-gx.log 2>&1; echo "rc=$?" >> $B/run-host32-sdk-gx.log ); head -c 2500 $B/run-host32-sdk-gx.log; echo
echo "== gx done $(date +%T)"
