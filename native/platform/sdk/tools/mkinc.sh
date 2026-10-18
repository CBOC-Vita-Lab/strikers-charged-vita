#!/bin/bash
# Generates the SDK include tree used to build platform/sdk (and usable by the game build):
#   mkinc.sh <outdir>
# Starts from the SDK headers (np/libs/RVL_SDK/include; override with SDK_INC),
# turns the hardware-address globals (`T NAME AT_ADDRESS(0x...);`) into plain `extern` declarations
# (np/platform/sdk/src/np_hw_globals.cpp defines them), fixes what a GNU compiler cannot parse, and
# lays the overlay headers of platform/sdk/shim/revolution on top. The result is written under the
# build directory only; nothing generated is committed.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?usage: mkinc.sh <outdir>}
# the headers of the main tree (np/libs/RVL_SDK/include, the decomp's with NPB's NP_NATIVE patches) when present
if [ -d "$HERE/../../libs/RVL_SDK/include/revolution" ]; then DEF_SRC="$(cd "$HERE/../../libs/RVL_SDK/include" && pwd)"; else DEF_SRC=/home/claude/research/decomp/mscharged-decomp/libs/RVL_SDK/include; fi
SRC=${SDK_INC:-$DEF_SRC}
STAMP="$OUT/.stamp"
NEW=$( (cat "$0"; find "$HERE/shim" -type f | sort | xargs cat; echo "$SRC") | sha1sum | cut -c1-16)
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$NEW" ]; then exit 0; fi
rm -rf "$OUT"; mkdir -p "$OUT"
cp -r "$SRC"/. "$OUT"/
# AT_ADDRESS globals -> extern declarations (skip lines that already say extern; `} NAME AT_ADDRESS(..);`
# closes an anonymous type whose `extern` was written on the first line by hand, see the sed below).
grep -rl "AT_ADDRESS" "$OUT" | while read -r f; do
  sed -i -E '/^[[:space:]]*\}/! { /extern /! s/^([[:space:]]*)(.*[^[:space:]])[[:space:]]+AT_ADDRESS\((0x[0-9A-Fa-f]+)\);/\1extern \2;/ }; s/[[:space:]]+AT_ADDRESS\((0x[0-9A-Fa-f]+)\);/;/' "$f"
  sed -i -E 's/^volatile union \{/extern volatile union {/' "$f"
done
# char16_t is a keyword in C++11 and later.
sed -i -E 's/^typedef unsigned short char16_t;/#ifndef __cplusplus\ntypedef unsigned short char16_t;\n#endif/; s/^typedef unsigned char char8_t;/#ifndef __cplusplus\ntypedef unsigned char char8_t;\n#endif/' "$OUT/revolution/types.h"
# typedefs and declarations that clash with the host C library (the game build needs the same).
sed -i -E '/^typedef unsigned long uintptr_t;/d; /^typedef unsigned long int register_t;/d' "$OUT/revolution/types.h"
sed -i -E '/^void exit\(void\);/d' "$OUT/revolution/os/__ppc_eabi_init.h"
sed -i -E '0,/#include <decomp.h>/ s//#include <decomp.h>\n#include <stdint.h>/' "$OUT/revolution/types.h"
cp -r "$HERE/shim/revolution/." "$OUT/revolution/"
echo "$NEW" > "$STAMP"
