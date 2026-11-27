#!/bin/bash
# 1.3 np/host_alloc.h + MSC_HEAP_GUARD: heapguard_test.cpp with the real src/NL/nlMemory.cpp and src/NL/MemAlloc.cpp, host32 flags (clang -m32), ASan off
# (the game's operator new replaces the library's). BREAK=1 must fail.
set -euo pipefail
NATIVE=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TEST_BUILD:-$(mktemp -d)}
FLAGS=(-m32 -std=gnu++11 -O1 -g -w -fno-exceptions -fno-rtti -fno-strict-aliasing -fwrapv -funsigned-char -include np/compat.h
       -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DNP_DISC_HOST_ORDER=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1
       -Iplatform/gx/include -I. -Iinclude -Ilibs/RVL_SDK/include -Iref/RVL_SDK -Ilibs/Runtime/include -Iplatform/sdk/include -idirafter libs/MSL_C/include)
[ -d /usr/include/x86_64-linux-gnu ] && FLAGS+=(-idirafter /usr/include/x86_64-linux-gnu)   # (Debian multiarch: asm/errno.h for -m32 without the i386 headers)
cd "$NATIVE"
for f in src/NL/nlMemory.cpp src/NL/MemAlloc.cpp tools/heapguard/heapguard_test.cpp tools/fps/gsplit_stub.cpp; do
  clang++ "${FLAGS[@]}" ${BREAK:+-DBREAK=$BREAK} -c "$f" -o "$OUT/$(basename "$f").o"
done
clang++ -m32 "$OUT"/*.o -o "$OUT/heapguard_test"
"$OUT/heapguard_test"
