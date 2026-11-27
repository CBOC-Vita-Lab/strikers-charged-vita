#!/bin/bash
# The run of MSC_VGX_HUDNATIVE (vgx_hudplan.h).   native/tools/render/run_hudplan_test.sh [outdir]
set -e
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$D/../../.."
OUT=${1:-$(mktemp -d)}
c++ -std=c++20 -O2 -Wall -I"$ROOT/renderer/src" "$D/hudplan_test.cpp" -o "$OUT/hudplan_test"
"$OUT/hudplan_test"
