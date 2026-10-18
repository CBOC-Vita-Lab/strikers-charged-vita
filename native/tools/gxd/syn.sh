#!/bin/bash
# GXD: syntax-only check (armhf flags, as native/native.mk) of vgx sources; light (a few seconds per file, no object files).
#   tools/gxd/syn.sh <vgx dir> [file.cpp ...]     (default: the core files this workstream edits)
D=${1:?vgx dir}; shift
FILES=("$@"); [ ${#FILES[@]} = 0 ] && FILES=(vgx_rec vgx_play vgx_state vgx_draw vgx_dlcache vgx_texcache vgx_guestmem vgx_stats)
rc=0
for f in "${FILES[@]}"; do
  f=${f%.cpp}
  nice -n 15 clang++ -std=c++20 -fsyntax-only -O2 --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -Wall -Wextra \
    -Wno-unused-parameter -Wno-missing-field-initializers -DTARGET_PC -DNOMINMAX -DVGX_NATIVE -I$D/src -I$D/native -I$D/include -I$D/third_party $EXTRA \
    $D/src/$f.cpp 2>&1 | grep -v "^In file included" | head -${HEADN:-30} && true
  [ ${PIPESTATUS[0]} = 0 ] || rc=1
done
exit $rc
