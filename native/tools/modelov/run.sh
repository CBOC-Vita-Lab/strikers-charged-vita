#!/bin/sh
# builds and runs the np/fxmodelov.h host test. MODELOV_BREAK=1 (built with the pad and margin cut), 2 (radius of one axis) or 3 (the final stream patch forgotten): live mutants, each must exit non-zero
set -e
R=$(cd "$(dirname "$0")/../../.." && pwd)
T=${TMPDIR:-/tmp}/modelov_test.$$
DEF=""; [ "${MODELOV_BREAK:-0}" = 1 ] && DEF="-DMODELOV_MUT1"
${CXX:-g++} -std=gnu++11 -O1 -Wall $DEF -I"$R/native/include" "$R/native/tools/modelov/modelov_test.cpp" -o "$T"
"$T"; rc=$?; rm -f "$T"; exit $rc
