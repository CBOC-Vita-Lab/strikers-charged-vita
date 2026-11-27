#!/bin/bash
# MSC_VGX_VS_NOBRANCH and MSC_VGX_FS_PROJ proof on Mesa (tests/shader_text_gl.cpp). The texts of a warm-up list (default: the shipped pack's list) are generated with the Vita player's
# switches, switch off and on (native/tools/gxpcount/listtext, record i of both files is one key); every text that changed is built on Mesa and drawn.
#   VS_NOBRANCH: vertex texts, random + zero + underflow + overflow vectors, outputs compared by bit pattern: must be 0 differing words.
#   FS_PROJ=1: fragment texts drawn with random textures, 8-bit pixels compared: at most 1 level (regimes 0..2), z == 0 identical. FS_PROJ=2 (no z == 0 rule) must FAIL (the mutant).
#   MODE=vs|fs|both   BREAK=1|2|3 (a wrong vertex text, see VGX_NOBRANCH_BREAK in vgx_shadergen.cpp: the run must FAIL)   BREAK=4 (FS_PROJ=2 instead of 1: must FAIL)
#   LIST=<vgx_shaderlist.bin / .shl>  OUT=<dir>
# Needs g++, Mesa's libEGL.so.1 / libGLESv2.so.2 and the GLES headers (exit 77 without a context).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
NATIVE=$VGX/../native
OUT=${OUT:-$VGX/build/tests/shader-text}
LIST=${LIST:-/home/claude/work/final/pack19/vgx_shaderlist.bin}
MODE=${MODE:-both}; BREAK=${BREAK:-0}
CXX=${CXX:-g++}
LIBDIR=${LIBDIR:-/usr/lib/x86_64-linux-gnu}
mkdir -p "$OUT"
SRCS=("$VGX/src/vgx_shadergen.cpp" "$VGX/src/vgx_state.cpp" "$VGX/src/vgx_stats.cpp" "$VGX/src/vgx_texcache.cpp" "$VGX/src/vgx_dlcache.cpp" "$VGX/src/vgx_texdecode.cpp" "$VGX/src/vgx_hdtex.cpp" "$VGX/src/vgx_interp.cpp" "$VGX/src/vgx_skin.cpp" "$VGX/src/vgx_guestmem.cpp" "$VGX/src/vgx_backend_null.cpp" "$VGX/src/vgx_capture.cpp" "$VGX/src/vgx_rec.cpp" "$VGX/src/vgx_play.cpp" "$VGX/src/vgx_present.cpp")
FLAGS=(-std=c++20 -O1 -w -I"$VGX/src" -I"$VGX/src/glstub" -I"$VGX/third_party")
[[ $BREAK =~ ^[123]$ ]] && FLAGS+=(-DVGX_NOBRANCH_BREAK=$BREAK)
"$CXX" "${FLAGS[@]}" "$NATIVE/tools/gxpcount/listtext.cpp" "${SRCS[@]}" -x c "$VGX/src/vgx_host_stubs.c" -x c++ -lpthread -ldl -o "$OUT/listtext"
"$CXX" -std=c++17 -O1 -Wall "$VGX/tests/shader_text_gl.cpp" -o "$OUT/shader_text_gl" "$LIBDIR/libEGL.so.1" "$LIBDIR/libGLESv2.so.2"
# the Vita player's switches that change keys and text (native/tools/seed/env_player.sh, the Vita plan's TEV_CANON / FINCLAMP)
set +u; . "$NATIVE/tools/seed/env_player.sh"; set -u
export MSC_VGX_TEV_CANON=${MSC_VGX_TEV_CANON:-2} MSC_VGX_FINCLAMP=${MSC_VGX_FINCLAMP:-3} LISTTEXT_VS=1
texts() { local tag=$1; shift; cp "$LIST" "$OUT/list.$tag.shl"; env "$@" "$OUT/listtext" "$OUT/list.$tag.shl" "$OUT/texts.$tag.txt" >/dev/null 2>&1; rm -f "$OUT/list.$tag.shl"; }
texts off MSC_VGX_VS_NOBRANCH=0 MSC_VGX_FS_PROJ=0
rc=0
if [[ $MODE == vs || $MODE == both ]] && [[ $BREAK != 4 ]]; then
    texts vs MSC_VGX_VS_NOBRANCH=1 MSC_VGX_VS_NOBRANCH_LIGHT=1 MSC_VGX_FS_PROJ=0   # (the light part is off by default, see vgx_shadergen.h; proved exact here all the same)
    texts vs0 MSC_VGX_VS_NOBRANCH=1 MSC_VGX_FS_PROJ=0
    # default form: no per-light change (the Vita3K 85-temporary-register program, 22 Nov): no `step(d1`, no `step(0.0, dot(n, ld))` in any text
    if grep -q -E 'step\(d1|step\(0\.0, dot\(n, ld\)\)' "$OUT/texts.vs0.txt"; then echo "shader_text: FAILED (a per-light branchless text with MSC_VGX_VS_NOBRANCH_LIGHT unset)"; rc=1; fi
    grep -q 'step(d1' "$OUT/texts.vs.txt" || { echo "shader_text: FAILED (MSC_VGX_VS_NOBRANCH_LIGHT=1 changed no light text)"; rc=1; }
    "$OUT/shader_text_gl" vs "$OUT/texts.off.txt" "$OUT/texts.vs.txt" || rc=$?
    "$OUT/shader_text_gl" vs "$OUT/texts.off.txt" "$OUT/texts.vs0.txt" || rc=$?
fi
if [[ $MODE == fs || $MODE == both ]] && { [[ $BREAK == 0 || $BREAK == 4 ]]; }; then
    texts fs MSC_VGX_VS_NOBRANCH=0 MSC_VGX_FS_PROJ=$([[ $BREAK == 4 ]] && echo 2 || echo 1)
    "$OUT/shader_text_gl" fs "$OUT/texts.off.txt" "$OUT/texts.fs.txt" 1 || rc=$?
fi
if [[ $rc == 77 ]]; then echo "shader_text: no GL on this machine"; exit 77; fi
if [[ $BREAK != 0 ]]; then
    if [[ $rc == 0 ]]; then echo "shader_text: FAILED (the break $BREAK passed)"; exit 1; fi
    echo "shader_text: the break $BREAK is caught"; exit 0
fi
[[ $rc == 0 ]] && echo "shader_text: PASS" || { echo "shader_text: FAIL"; exit 1; }
