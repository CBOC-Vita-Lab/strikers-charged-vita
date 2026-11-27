#!/bin/bash
# Actual whole vitaGL RT owner over explicit host kernel/GXM boundaries; no GPU/assets.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
RT="$VGX/../dependencies/vitaGL/source/rt.c"
CC=${CC:-clang}
command -v "$CC" >/dev/null 2>&1 || { printf 'C compiler not found: %s\n' "$CC" >&2; exit 1; }
[[ -f "$RT" && -f "$VGX/../dependencies/vitaGL/source/vitaGL.h" ]] || { printf 'vitaGL sources missing\n' >&2; exit 1; }
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/vgx-rt-validity.XXXXXX")
trap 'rm -rf -- "$BUILD"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
"$CC" -std=c11 -O1 -g -fno-inline -Wall -Wextra \
    -Wno-unused-parameter -Wno-int-to-pointer-cast \
    -I"$VGX/tests/rt_validity_platform" -I"$VGX/../dependencies/vitaGL/source" \
    "-DRT_SOURCE=\"$RT\"" "$VGX/tests/vitagl_rt_validity.c" -o "$BUILD/vitagl_rt_validity"
for case in healthy boundary47 boundary48 partial60 untimed drop300 independent-drop; do
    "$BUILD/vitagl_rt_validity" "$case"
done
printf 'vitaGL GPU timing validity: all seven cases passed (host contract only)\n'
