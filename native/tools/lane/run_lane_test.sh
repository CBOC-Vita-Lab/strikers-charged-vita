#!/bin/bash
# The helper lane (MSC_SKIN_LANE, platform/vita/src/np_framesend.cpp) on the host: the frame-send / lane unit test in all its configurations, including
# the Vita kernel semaphore's maximum of 1 (MSC_SEM_MAX1=1: the first lane hung on a dropped post) and a lane thread that does not run (MSC_LANE_FREEZE_MS).
# No game data; about 15 s on x86-64.   tools/lane/run_lane_test.sh [outdir]
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-${TMPDIR:-/tmp}/msc_lane_test}
mkdir -p "$OUT"
V=platform/vita
g++ -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-psabi -fno-strict-aliasing -I$V/include -I$V/src -Iplatform/harness/include \
  $V/tests/test_framesend.cpp $V/src/np_framesend.cpp platform/harness/src/np_vclock.cpp -o "$OUT/test_framesend" -pthread -ldl 2>"$OUT/build.err" || { head -40 "$OUT/build.err"; exit 1; }
BIN="$OUT/test_framesend" RUN= $V/tests/run_fs_tests.sh
