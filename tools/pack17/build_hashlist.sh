#!/bin/bash
# build_hashlist.sh <repo root> <out exe>: builds tools/pack17/hashlist.cpp as a 32-bit program (the shader lists' key structs are 32-bit).
# Needs g++ with -m32 (gcc-multilib). No shader compiler is involved: it only generates the GLSL text of each key and hashes it.
set -e
R=$(cd "${1:?repo root}" && pwd); OUT=${2:?out exe}
g++ -m32 -std=c++17 -O1 -w -I"$R/renderer/src" -I"$R/renderer/third_party" -DXXH_INLINE_ALL "$R/tools/pack17/hashlist.cpp" "$R/renderer/src/vgx_shadergen.cpp" -o "$OUT"
echo "built $OUT"
