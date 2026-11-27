#!/bin/bash
# armbuild.sh plain|pgo : armhf harness builds for the det proof (same flags as the recording build: -O2, ThinLTO, NP_PGO_ALL=1)
K=${1:?plain|pgo}; W=/home/claude/work/pgo13; B=$W/arm-$K; mkdir -p $B/armhf
cd /home/claude/s13/wt/pgo13/native
X=; [ $K = pgo ] && X="NP_CLANG_PGO=$W/msc-arm.profdata NP_PGO_ALL=1"
nice -n 15 make -j2 TARGET=armhf NOPIC=1 "OPT=-O2 -ffunction-sections -fdata-sections" WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 GX_VGX=$(realpath ../renderer) \
  EXESUF=-full BUILD_ROOT=$B DEBUG=-g0 "LDEXTRA=$W/rec-arm/root/extra_stubs.o $W/rec-arm/root/pgo_stub.o" $X > $B/build.log 2>&1; echo rc $? >> $B/build.log; tail -2 $B/build.log | cut -c1-200
