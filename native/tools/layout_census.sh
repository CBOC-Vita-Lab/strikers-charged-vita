#!/bin/bash
# usage: tools/layout_census.sh <host32|armhf> [--list]
# Compares the Wii layouts documented in the decomp headers (offset comments, "total size") with the
# native layouts clang computes for the target. One syntax-only TU (about 1 s), no objects.
T=${1:-armhf}; shift
cd /home/claude/msc/np
D=/home/claude/msc/build/np/layout; mkdir -p $D
python3 tools/layout_census.py --gen-tu $D/all_headers.cpp
COMMON="-DNP_NATIVE=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -DdNODEBUG=1 -DdSINGLE=1 -I. -Iinclude -Isrc/ode -Isrc/zlib -Ilibs/RVL_SDK/include -Iref/RVL_SDK -Ilibs/Runtime/include -idirafter libs/MSL_C/include -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -w -include np/compat.h -std=gnu++11 -fno-exceptions -fno-rtti -Wno-c++11-narrowing -funsigned-char -ferror-limit=0"
case $T in
 host32) clang++ -m32 -msse2 -mfpmath=sse -isystem /usr/include/x86_64-linux-gnu $COMMON -fsyntax-only -Xclang -fdump-record-layouts $D/all_headers.cpp > $D/layout_$T.txt 2>/dev/null;;
 armhf) clang++ --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb $COMMON -fsyntax-only -Xclang -fdump-record-layouts $D/all_headers.cpp > $D/layout_$T.txt 2>/dev/null;;
esac
python3 tools/layout_census.py $D/layout_$T.txt "$@"
