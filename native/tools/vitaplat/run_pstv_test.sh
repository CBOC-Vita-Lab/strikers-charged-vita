#!/bin/bash
# The PS TV's pad-only defaults (vita_plan.cpp): set on a PS TV, never over the owner's own line, absent on a handheld. No game data, no device.
#   tools/vitaplat/run_pstv_test.sh
set -e
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
vita=native/platform/vita
"${CXX:-c++}" -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I$vita/src -I$vita/include $vita/tests/test_pstv.cpp $vita/src/vita_plan.cpp -o "$tmp/test_pstv" -pthread
"$tmp/test_pstv" 2>/dev/null
