#!/bin/bash
# Prints clang's record layouts (host32 flags of the main Makefile) of the structs the save files contain.
#   tools/save_layouts.sh [host32|armhf] > /tmp/layouts.txt ; python3 tools/gen_save_layout.py /tmp/layouts.txt platform/sdk/save/np_save_layout.inc
cd "$(dirname "$0")/.."
T=${1:-host32}
case $T in
 host32) TF="-m32 -msse2 -mfpmath=sse -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1";;
 armhf)  TF="--target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -DNP_TARGET_ARMHF=1";;
 *) echo "target?" >&2; exit 2;;
esac
clang++ $TF -DNP_NATIVE=1 -DNP_DISC_HOST_ORDER=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -DdNODEBUG=1 -DdSINGLE=1 \
  -Iplatform/gx/include -I. -Iinclude -Isrc/ode -Isrc/zlib -Ilibs/RVL_SDK/include -Iref/RVL_SDK -Ilibs/Runtime/include \
  -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar \
  -fno-delete-null-pointer-checks -w -include np/compat.h -std=gnu++11 -fno-exceptions -fno-rtti -Wno-c++11-narrowing \
  -Wno-enum-constexpr-conversion -fno-strict-return -funsigned-char -fsyntax-only -Xclang -fdump-record-layouts \
  tools/save_layout_probe.cpp 2>&1
