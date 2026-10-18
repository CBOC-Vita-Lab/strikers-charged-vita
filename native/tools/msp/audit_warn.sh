#!/bin/bash
# MSP job (one heavy slot, ~10 min): signedness audit. Syntax-only pass over every game source with the host32 flags and the warnings that show where
# the signedness of char or of an enum (MWCC: signed int enums; clang: unsigned when no enumerator is negative) makes a comparison constant:
#   tautological unsigned/enum/char compared with 0, constants out of the type's range, bool and pointer compares, type limits.
#   tools/heavy.sh -t 900 tools/msp/audit_warn.sh        -> build/np/MSP/audit/warn.txt (all warnings of these kinds) and the summary on stdout
NP=$(cd "$(dirname "$0")/../.." && pwd); cd $NP || exit 1
B=${BUILD_ROOT:-/home/claude/msc/build/np/MSP}; OUT=$B/audit; mkdir -p $OUT
W="-Wno-everything -Wtautological-compare -Wtautological-type-limit-compare -Wtautological-constant-in-range-compare -Wtautological-unsigned-enum-zero-compare -Wtautological-unsigned-char-zero-compare"
FL="-m32 -msse2 -mfpmath=sse -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DNP_DISC_HOST_ORDER=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -DdNODEBUG=1 -DdSINGLE=1 -Iplatform/gx/include -I. -Iinclude -Isrc/ode -Isrc/zlib -Ilibs/RVL_SDK/include -Iref/RVL_SDK -Ilibs/Runtime/include -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -fno-delete-null-pointer-checks -include np/compat.h -fno-math-errno -std=gnu++11 -fno-exceptions -fno-rtti -Wno-c++11-narrowing -Wno-enum-constexpr-conversion -fno-strict-return -O0 -Iplatform/sdk/include -fsyntax-only"
one() {   # one <file>: char signedness as the Makefile gives it (tools/char_signed.txt = prefixes compiled -fsigned-char)
  f=$1; cs=-funsigned-char
  while read -r p; do [ -n "$p" ] && case "$f" in $p*) cs=-fsigned-char;; esac; done < tools/char_signed.txt
  clang++ $FL $cs $W $f 2>&1 | grep -E "warning:" | sed "s|^|[$cs] |"
}
export -f one; export FL W
find src -name '*.cpp' -o -name '*.c' | grep -v '^src/ode/' | sort | xargs -P2 -I{} bash -c 'one {}' > $OUT/warn.txt
find src/ode -name '*.cpp' -o -name '*.c' | sort | xargs -P2 -I{} bash -c 'one {}' >> $OUT/warn.txt
echo "files: $(find src -name '*.cpp' -o -name '*.c' | wc -l); warnings: $(wc -l < $OUT/warn.txt)"
sed 's/.*\[-W/[-W/' $OUT/warn.txt | sort | uniq -c | sort -rn
grep -E "unsigned-enum|unsigned-char|out-of-range|type-limit|unsigned-zero" $OUT/warn.txt | sort -u | head -80
