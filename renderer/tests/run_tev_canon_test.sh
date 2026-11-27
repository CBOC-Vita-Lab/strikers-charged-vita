#!/bin/bash
# 1.3 MSC_VGX_TEV_CANON proof: for every fragment key of a warm-up list, the text with the switch off and with it on, evaluated by tests/glsl_eval.py over random and adversarial
# fragments (0, 1, 253..255, 127..129 / 255, off-grid values), 8-bit outputs and the discard decision compared. Mode 1: 0 differing; mode 2: at most 1 level.
#   LIST=<a vgx_shaderlist.bin / .shl; default the 1.3 catalogue> N=<fragments per key, default 20000> MODE=1|2|fc|both  BREAK=1 (a wrong fold in mode 1), BREAK=2 (a mode 2 threshold half a step off): the test must FAIL
# (No BREAK for FINCLAMP: every key of the catalogue ends in stage clamps, so its final range is inside 0..1 and a clamp dropped wrongly would not show; the analysis is FS_TRIM's, mutation-tested by run_fs_trim_test.sh.)
# Needs the listtext tool's sources (native/tools/gxpcount) and python3 with numpy.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
NATIVE=$VGX/../native
OUT=${TEST_BUILD:-$VGX/build/tests/tev-canon}
LIST=${LIST:-/home/claude/work/final/pack13.catalogue.shl}
N=${N:-20000}; MODE=${MODE:-both}
CXX=${CXX:-g++}
mkdir -p "$OUT"
SRCS=("$VGX/src/vgx_shadergen.cpp" "$VGX/src/vgx_state.cpp" "$VGX/src/vgx_stats.cpp" "$VGX/src/vgx_texcache.cpp" "$VGX/src/vgx_dlcache.cpp" "$VGX/src/vgx_texdecode.cpp" "$VGX/src/vgx_hdtex.cpp" "$VGX/src/vgx_interp.cpp" "$VGX/src/vgx_skin.cpp" "$VGX/src/vgx_guestmem.cpp" "$VGX/src/vgx_backend_null.cpp" "$VGX/src/vgx_capture.cpp" "$VGX/src/vgx_rec.cpp" "$VGX/src/vgx_play.cpp" "$VGX/src/vgx_present.cpp")
FLAGS=(-std=c++20 -O1 -w -I"$VGX/src" -I"$VGX/src/glstub" -I"$VGX/third_party")
[[ ${BREAK:-0} == 1 ]] && FLAGS+=(-DVGX_TEV_CANON_BREAK)
[[ ${BREAK:-0} == 2 ]] && FLAGS+=(-DVGX_TEV_CANON_BREAK2)
"$CXX" "${FLAGS[@]}" "$NATIVE/tools/gxpcount/listtext.cpp" "${SRCS[@]}" -o "$OUT/listtext"
[[ -f "$LIST" ]] || { echo "SKIP tev_canon: the shader catalogue $LIST (a list from real game runs, not in the repo) is not on this box; the test program built and linked. Set LIST=<a .shl>."; exit 77; }
# the Vita player's switches that change keys (native/tools/seed/env_player.sh), FS_TRIM on: the canon form is measured against what the player generates
set +u; . "$NATIVE/tools/seed/env_player.sh"; set -u
texts() { cp "$LIST" "$OUT/list.$1.shl"; MSC_VGX_TEV_CANON=$1 "$OUT/listtext" "$OUT/list.$1.shl" "$OUT/texts.$1.txt" >/dev/null; }
texts 0; texts 1; texts 2
# MSC_VGX_FINCLAMP=2 (no final clamp where the ranges prove it idle), alone and on top of mode 2
textsfc() { cp "$LIST" "$OUT/list.$1.shl"; MSC_VGX_FINCLAMP=${FCM:-2} MSC_VGX_TEV_CANON=$2 "$OUT/listtext" "$OUT/list.$1.shl" "$OUT/texts.$1.txt" >/dev/null; }
textsfc fc 0; textsfc fc2 2; FCM=3 textsfc fc3 2
run() { RAW=${3:-} python3 -I "$VGX/tests/tev_canon_prove.py" "$OUT/texts.0.txt" "$OUT/texts.$1.txt" "$2" "$N"; }
rc=0
if [[ $MODE == 1 || $MODE == both ]]; then run 1 0 || rc=1; fi
if [[ $MODE == 2 || $MODE == both ]]; then run 2 1 || rc=1; fi
if [[ $MODE == fc || $MODE == both ]]; then run fc 0 1 || rc=1; run fc2 1 1 || rc=1; run fc3 1 1 || rc=1; VCOL_OVER=0.05 run fc3 1 1 || true; fi
if [[ ${BREAK:-0} != 0 ]]; then
    if [[ $rc == 0 ]]; then echo "tev_canon: FAILED (the generator with a wrong fold passed)"; exit 1; fi
    echo "tev_canon: the break is caught"; exit 0
fi
[[ $rc == 0 ]] && echo "tev_canon: PASS" || { echo "tev_canon: FAIL"; exit 1; }
