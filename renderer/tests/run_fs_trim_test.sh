#!/bin/bash
# 1.3 MSC_VGX_FS_TRIM: the range analysis (tests/fs_trim.cpp), the deliberate break (mutate; BREAK=1: a generator that drops the final clamp), every trimmed text keeps the final clamp, and with the switch off the text is the old one (tests/shader_forms.cpp pins the hashes, run_shader_forms_test.sh).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-$VGX/build/tests/fs-trim}
CXX=${CXX:-g++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I"$VGX/src" -I"$VGX/src/glstub" -I"$VGX/third_party")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$CXX" "${FLAGS[@]}" "$VGX/tests/fs_trim.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/fs_trim" 
if [[ ${BREAK:-0} == 1 ]]; then
    # the deliberate break of the final-clamp rule: a generator that drops it must fail the text check
    "$CXX" "${FLAGS[@]}" -DVGX_FS_TRIM_FINAL_BREAK "$VGX/tests/fs_trim.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/fs_trim_break"
    if "$BUILD/fs_trim_break"; then echo "fs_trim: FAILED (the generator without the final clamp passed)"; exit 1; fi
    echo "fs_trim: the break is caught"
    exit 0
fi
"$BUILD/fs_trim"
"$BUILD/fs_trim" mutate
echo "fs_trim: PASS"
