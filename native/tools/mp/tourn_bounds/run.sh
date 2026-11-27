#!/bin/bash
# NetTournManager::HandleTournamentGameUpdate with game indices off the wire (tourn_test.cpp). Host only, 32-bit g++.   run.sh [outdir]
set -e
cd "$(dirname "$0")/../../.."
OUT=${1:-/tmp/np-tourn-test}
mkdir -p "$OUT"
echo '#include <string.h>' > "$OUT/mem.h"   # (the game's MSL header of that name does not mix with the host's libc)
FL="-m32 -std=gnu++11 -DNP_NATIVE=1 -DNP_TARGET_HOST32=1 -DVERSION_R4QE01 -DNDEBUG=1 -I. -Iinclude -Isrc/ode -Ilibs/RVL_SDK/include -Ilibs/Runtime/include -I$OUT \
  -fno-strict-aliasing -fwrapv -w -fshort-wchar -fpermissive -include np/compat.h -g -O0 -fno-pie"
g++ $FL -c src/Game/NetTournManager.cpp -o "$OUT/ntm.o"
g++ $FL -c tools/mp/tourn_bounds/tourn_test.cpp -o "$OUT/tourn_test.o"
# every other symbol the file refers to: a zeroed block (data reads 0; a function the test reaches would crash, and none is reached)
{ echo '.bss'; echo '.balign 16'; echo 'np_zero_block: .space 65536'
  printf '.section .note.GNU-stack,"",@progbits\n'
  nm -u "$OUT/ntm.o" | awk '{print $2}' | grep -v -E '^(_GLOBAL_OFFSET_TABLE_|__|_Unwind|memcmp|memset|memcpy|atof)' | while read -r s; do
    nm "$OUT/tourn_test.o" | grep -q " [TDBR] $s\$" || printf '.globl %s\n.set %s, np_zero_block\n' "$s" "$s"; done; } > "$OUT/zero.s"
g++ -m32 -c "$OUT/zero.s" -o "$OUT/zero.o"
g++ -m32 -no-pie "$OUT/tourn_test.o" "$OUT/ntm.o" "$OUT/zero.o" -o "$OUT/tourn_test"
"$OUT/tourn_test"
