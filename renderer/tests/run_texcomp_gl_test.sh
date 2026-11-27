#!/bin/bash
# MSC_VGX_TEX_COMPRESS through the GL back end over the stub GL (Vita build: S3TC on). BREAK=n: a mutant of the back end's part (1: TcApply says true whatever happened; 2: the draw's usage marks are not taken).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-/tmp/vgx-texcomp-gl}; mkdir -p "$OUT"
F=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -D__VGX_VITA__ -I"$VGX/src" -I"$VGX/src/glstub" -I"$VGX/third_party"); [[ ${BREAK:-0} != 0 ]] && F+=(-DVGX_TCGL_BREAK=${BREAK})
${CXX:-g++} "${F[@]}" "$VGX/tests/texcomp_gl.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$OUT/texcomp_gl"
if [[ ${BREAK:-0} != 0 ]]; then if "$OUT/texcomp_gl" 1 >/dev/null 2>&1; then echo "texcomp_gl: FAILED (mutant ${BREAK} passed)"; exit 1; fi; echo "texcomp_gl: mutant ${BREAK} caught"; exit 0; fi
"$OUT/texcomp_gl" 1
"$OUT/texcomp_gl" 0
