#!/bin/bash
# 1.3 MSC_VGX_FS_HALF, MSC_VGX_TEX2D: the generator (tests/shader_forms.cpp), the warm-up list on the real front end (tests/shader_forms_list.cpp), and the texts on Mesa and on vitaGL's
# translator built for the host (tests/shader_forms_gl.py). No console compiler is involved: docs/TECHNICAL.md says what that leaves unproven.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$VGX/.." && pwd)
BUILD=${TEST_BUILD:-$VGX/build/tests/shader-forms}
CXX=${CXX:-g++}
rm -rf "$BUILD/dump" "$BUILD/tr"
mkdir -p "$BUILD/dump" "$BUILD/tr/utils" "$BUILD/tr/shaders" "$BUILD/obj"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I"$VGX/src" -I"$VGX/src/glstub" -I"$VGX/third_party")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$CXX" "${FLAGS[@]}" "$VGX/tests/shader_forms.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/shader_forms"
"$BUILD/shader_forms"
for env in "MSC_VGX_FS_HALF=0" "MSC_VGX_FS_HALF=1" "MSC_VGX_FS_HALF=2" "MSC_VGX_TEX2D=1" "MSC_VGX_FS_HALF=2 MSC_VGX_TEX2D=1"; do env $env "$BUILD/shader_forms" switch; done

# The list. (The front end's files without the two that need the game's headers: nothing here reaches them.)
for f in vgx_rec vgx_play vgx_guestmem vgx_dlcache vgx_state vgx_shadergen vgx_texdecode vgx_texcache vgx_stats vgx_interp vgx_present vgx_capture vgx_skin vgx_hdtex vgx_backend_null; do
    "$CXX" -std=c++20 -O0 -w -I"$VGX/src" -I"$VGX/src/glstub" -I"$VGX/third_party" -c "$VGX/src/$f.cpp" -o "$BUILD/obj/$f.o"
done
"$CXX" -std=c++20 -O0 -w -I"$VGX/src" -I"$VGX/src/glstub" -I"$VGX/third_party" "$VGX/tests/shader_forms_list.cpp" "$BUILD"/obj/*.o -x c "$VGX/src/vgx_host_stubs.c" -o "$BUILD/shader_forms_list" -lpthread -ldl
L=$BUILD/list.bin
"$BUILD/shader_forms_list" make "$L"
cp "$L" "$L.orig"
"$BUILD/shader_forms_list" read "$L" "$BUILD/off.txt" | tee "$BUILD/off.log"
cp "$L" "$L.afteroff"
cp "$L.orig" "$L"
MSC_VGX_FS_HALF=1 MSC_VGX_TEX2D=1 "$BUILD/shader_forms_list" read "$L" "$BUILD/on.txt" | tee "$BUILD/on.log"
cmp "$L" "$L.afteroff"                                  # the list holds the same records whatever the switches: plain keys, the new one too
! grep -q 'HV4\|vec2 v_tex' "$BUILD/off.txt"            # off: the plain forms
grep -q 'HV4' "$BUILD/on.txt"                           # on: the listed shaders are generated in the new form, in the warm-up
grep -q 'vec2 v_tex0' "$BUILD/on.txt"
grep -q 'generated form 5' "$BUILD/on.log"              # a first use in play has it too
MSC_VGX_FS_HALF=1 "$BUILD/shader_forms_list" read "$L" "$BUILD/on2.txt" | grep -q 'warm-up: 2 vertex shaders, 4 fragment shaders, 4 programs'   # and it is a listed key on the next run

# vitaGL's real translator on the host, from the tree (a copy: it includes ../shared.h).
SRC=$ROOT/dependencies/vitaGL/source
cp -r "$SRC/utils/glsl_utils.c" "$SRC/utils/glsl_utils.h" "$SRC/utils/preprocessor" "$BUILD/tr/utils/"
cp "$SRC/shaders/glsl_translator_hdr.h" "$BUILD/tr/shaders/"
S=$VGX/tests/vitagl_translator_host
cp "$S"/shared.h "$S"/vitaGL.h "$S"/vitasdk.h "$BUILD/tr/"
cp "$S"/debug_utils.h "$BUILD/tr/utils/"
( cd "$BUILD/tr"
  gcc -std=gnu99 -w -I. -c utils/glsl_utils.c -o g.o
  gcc -std=gnu99 -w -I. -c "$S/drv.c" -o d.o
  g++ -std=gnu++17 -w -I. -c utils/preprocessor/preprocessor.cpp utils/preprocessor/expression.cpp
  g++ g.o d.o preprocessor.o expression.o -o drv )
"$BUILD/shader_forms" dump "$BUILD/dump" "${FORMS_N:-400}"
set +e
python3 -I "$VGX/tests/shader_forms_gl.py" "$BUILD/dump" "$BUILD/tr/drv"
rc=$?
set -e
[[ $rc == 0 || $rc == 77 ]] || exit 1
echo "shader forms tests: all passed"
