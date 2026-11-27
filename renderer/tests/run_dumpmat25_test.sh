#!/bin/bash
# renderer/tests/run_dumpmat25_test.sh: builds and runs the Dump material rewrite test, then a mutated header (the layer-2 constant replaces the layer-1 fetch) that must fail it
set -e
VGX=$(cd "$(dirname "$0")/.." && pwd); BUILD=${BUILD:-/tmp/vgx_dumpmat25_test}; mkdir -p "$BUILD"
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -I"${DUMPMAT_SRC:-$VGX/src}" "$VGX/tests/dumpmat25.cpp" -o "$BUILD/dumpmat25"
"$BUILD/dumpmat25" "$VGX/tests/data/dumpmat25_fs.txt"
M=$(mktemp -d "$BUILD/mut.XXXXXX"); cp "$VGX/src/vgx_dumpmat25.h" "$M/"
sed -i 's/std::snprintf(call, sizeof call, "texture2D(u_tex%d, uv%d).rgba", n, n);/std::snprintf(call, sizeof call, "texture2D(u_tex%d, uv%d).rgba", n == 1 ? 2 : n, n == 1 ? 2 : n);/' "$M/vgx_dumpmat25.h"
cmp -s "$M/vgx_dumpmat25.h" "$VGX/src/vgx_dumpmat25.h" && { echo "FAIL: the mutation did not apply"; exit 1; }
${CXX:-clang++} -std=c++20 -O1 -I"$M" "$VGX/tests/dumpmat25.cpp" -o "$M/dumpmat25_mut"
if "$M/dumpmat25_mut" "$VGX/tests/data/dumpmat25_fs.txt"; then echo "FAIL: the mutated rewrite passed the test"; rm -rf "$M"; exit 1; fi
echo "dumpmat25: the mutated rewrite fails the test, as it must"; rm -rf "$M"
