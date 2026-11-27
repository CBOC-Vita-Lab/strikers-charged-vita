#!/bin/bash
# The lobby's connection messages against sizes and counts the wire made up, under AddressSanitizer. Fails (heap-buffer-overflow) until lan-lobby-bounds.patch is applied to
# native/src/Game/LANConnectionMessages.cpp. Host only, 64-bit g++.   run.sh [outdir]
set -e
cd "$(dirname "$0")/../../.."
OUT=${1:-/tmp/np-lan-msg-test}
mkdir -p "$OUT"
echo '#include <string.h>' > "$OUT/mem.h"   # (the game's MSL header of that name does not mix with the host's libc)
g++ -std=gnu++11 -DNP_NATIVE=1 -DNP_TARGET_HOST32=1 -DVERSION_R4QE01 -DNDEBUG=1 -I. -Iinclude -Ilibs/RVL_SDK/include -Ilibs/Runtime/include -I"$OUT" \
  -fno-strict-aliasing -fwrapv -w -fshort-wchar -include np/compat.h -g -fsanitize=address,undefined \
  tools/mp/lan_lobby_bounds/lan_msg_test.cpp src/Game/LANConnectionMessages.cpp src/Game/NetworkMessageSerializer.cpp -o "$OUT/lan_msg_test"
"$OUT/lan_msg_test"
