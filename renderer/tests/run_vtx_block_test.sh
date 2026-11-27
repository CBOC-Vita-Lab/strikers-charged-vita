#!/bin/bash
# nxboot25 R2-9: the vertex arenas in memory blocks (MSC_VGX_VTX_BLOCK), host model with a capped heap and a fake provider, plus mutants.
#   tests/run_vtx_block_test.sh [outdir]
set -eu
V=$(cd "$(dirname "$0")/.." && pwd); O=${1:-/tmp/vgx_vtx_block}; mkdir -p "$O/m"
CXX=${CXX:-g++}; F="-std=c++17 -O1 -g -Wall -Wextra"
$CXX $F -I"$V/src" "$V/tests/vgx_vtx_block.cpp" -o "$O/t"
"$O/t" off; "$O/t" on; "$O/t" on fail
mut() {   # name, sed expression on vgx_cmd.h
    local d="$O/m/$1"; mkdir -p "$d"; cp "$V"/src/*.h "$d/"; sed "$2" "$V/src/vgx_cmd.h" > "$d/vgx_cmd.h"
    cmp -s "$d/vgx_cmd.h" "$V/src/vgx_cmd.h" && { echo "mutant $1 did not change the header"; exit 1; }
    $CXX $F -I"$d" "$V/tests/vgx_vtx_block.cpp" -o "$d/t" 2>/dev/null
    local ok=1; for a in "off" "on" "on fail"; do "$d/t" $a > "$d/out" 2>&1 && continue; ok=0; break; done
    [[ $ok == 0 ]] || { echo "MUTANT $1 SURVIVED"; exit 1; }
    echo "  mutant $1 killed"
}
mut always_on       's/NpHostBlockNew \&\& VtxBlockOn()/NpHostBlockNew/'
mut no_free         's/NpHostBlockFree \&\& NpHostBlockFree(p)) return;/false) return;/'
mut any_size        's/sizeof(T) == 1 \&\& n >= kVtxBlockMin \&\& NpHostBlockNew/sizeof(T) >= 1 \&\& n >= 1024 \&\& NpHostBlockNew/'
mut no_fallback     's/if (void\* b = NpHostBlockNew(n)) return static_cast<T\*>(b);/return static_cast<T*>(NpHostBlockNew(n));/'
mut free_always     's/::operator delete(p);\n    }/XX/; s/if (sizeof(T) == 1 \&\& n >= kVtxBlockMin \&\& NpHostBlockFree \&\& NpHostBlockFree(p)) return;/if (NpHostBlockFree) NpHostBlockFree(p);/'
echo "run_vtx_block_test: ok"
