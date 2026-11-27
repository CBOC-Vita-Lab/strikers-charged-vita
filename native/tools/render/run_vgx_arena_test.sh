#!/bin/bash
# The frame arenas' hold on the C heap (renderer/src/vgx_arena.h): the growth rule, the floors, and a scripted session of matches through the two
# circulating frame buffers in a capped first-fit heap, against the old doubling (renderer/tests/vgx_arena.cpp). Host compiler only, no game data.
#   tools/render/run_vgx_arena_test.sh      (out-of-tree build in $OUT, default /tmp/np-vgx-arena)
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${OUT:-${TMPDIR:-/tmp}/np-vgx-arena}
mkdir -p "$out"
cd "$root"
${CXX:-clang++} -std=c++17 -O2 -Wall -Wextra -Irenderer/src renderer/tests/vgx_arena.cpp -o "$out/arena"
"$out/arena"
echo "vgx_arena: PASS"
# 1.3 soft growth (MSC_VGX_ARENA_SOFT): the refusal of a capped heap leaves the vector as it was; with the switch off the old path is taken
${CXX:-clang++} -std=c++17 -O2 -Wall -Wextra -Irenderer/src renderer/tests/vgx_arena_soft.cpp -o "$out/arena_soft"
"$out/arena_soft"
"$out/arena_soft" off
