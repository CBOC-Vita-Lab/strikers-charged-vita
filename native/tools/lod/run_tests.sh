#!/bin/sh
# Host tests of np/lod.h and np/lod_simplify.h (x86-64 g++, ASan and UBSan). Run from anywhere.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
NATIVE=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-$(mktemp -d)}
g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I"$NATIVE/include" "$HERE/lod_test.cpp" -o "$OUT/lod_test"
"$OUT/lod_test"
