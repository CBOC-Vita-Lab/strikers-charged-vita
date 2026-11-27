#!/bin/bash
# The Vita makefile of the vgx library (renderer/native/native-vita.mk) must rebuild every object that includes the recorder's header when that header
# changes. vgx_rec.h holds the Chunk layout and the inline Reserve / Commit / Wpos: a stale vgx_api.o (its rule listed no headers) wrote records at one
# offset and published at another, and the player aborted with "corrupt record stream" in the first frame of an incrementally built package.
# Host test, no compiler needed (make -n): every object is created newer than its sources, then each header is "touched" with make -W and the dry run must
# list every .cpp that includes it. Usage: run_vitamk_deps_test.sh   (from anywhere; exit 0 = pass)
set -u
HERE=$(cd "$(dirname "$0")" && pwd); R=$(cd "$HERE/../../../renderer" && pwd)
MK=$R/native/native-vita.mk
T=$(mktemp -d "${TMPDIR:-/tmp}/vitamkXXXX"); trap 'rm -rf "$T"' EXIT
printf 'print-objs:\n\t@echo $(OBJS)\n' > "$T/wrap.mk"
OBJS=$(make -s -f "$MK" -f "$T/wrap.mk" NOUT="$T/out" VDEPS="$T/vdeps" print-objs) || { echo "FAIL: cannot read OBJS from $MK"; exit 1; }
[ -n "$OBJS" ] || { echo "FAIL: no objects listed"; exit 1; }
for o in $OBJS; do mkdir -p "$(dirname "$o")"; : > "$o"; done
: > "$T/out/vita-vitagl/libvgx-native-vita.a"
dry() { make -n -f "$MK" NOUT="$T/out" VDEPS="$T/vdeps" "$@" 2>&1; }
out=$(dry)
if echo "$out" | grep -q "CXX"; then echo "FAIL: with every object newer than its sources the dry run still compiles:"; echo "$out" | grep CXX; exit 1; fi
fail=0; checked=0
for h in "$R"/src/*.h; do
  hn=$(basename "$h")
  # the sources of the library that include this header directly
  for s in "$R"/src/*.cpp "$R"/native/*.cpp "$R"/vita/*.cpp; do
    base=$(basename "$s")
    echo "$OBJS" | tr ' ' '\n' | grep -q "/${base%.cpp}.o$" || continue
    grep -q "#include \"$hn\"" "$s" || continue
    out=$(dry -W "$h")
    if echo "$out" | grep -q "CXX ${base}"; then checked=$((checked + 1)); else echo "FAIL: touching $hn does not rebuild $base"; fail=1; fi
  done
done
[ $checked -gt 0 ] || { echo "FAIL: no header/source pair was checked"; exit 1; }
[ $fail = 0 ] && echo "vitamk deps: PASS ($checked header/source pairs rebuild when the header changes)"
exit $fail
