#!/bin/bash
# The clear planner of the GL back end (MSC_VGX_CLEARDEAD / MSC_VGX_CLEARMERGE) against a pixel simulation.   native/tools/render/run_clearplan_test.sh [outdir]
set -e
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$D/../../.."
OUT=${1:-$(mktemp -d)}
c++ -std=c++20 -O2 -Wall -I"$ROOT/renderer/src" "$D/clearplan_test.cpp" -o "$OUT/clearplan_test"
"$OUT/clearplan_test"
