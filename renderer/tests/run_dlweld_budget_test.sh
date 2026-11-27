#!/bin/sh
# nxfly25: host test of the weld budget policy (vgx_dlweldbudget.h); --mutants: VGX_DLWELDBUDGET_BREAK 1-5 must each fail.
set -e
VGX=$(cd "$(dirname "$0")/.." && pwd); BUILD=${BUILD:-${TMPDIR:-/tmp}}
CXX=${CXX:-g++}
$CXX -std=c++17 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/dlweld_budget.cpp" -o "$BUILD/dlweld_budget"
"$BUILD/dlweld_budget"
if [ "$1" = "--mutants" ]; then
  for b in 1 2 3 4 5 6 7; do
    $CXX -std=c++17 -O1 -DVGX_DLWELDBUDGET_BREAK=$b "$VGX/tests/dlweld_budget.cpp" -o "$BUILD/dlweld_budget_m$b"
    if "$BUILD/dlweld_budget_m$b" > /dev/null; then echo "mutant $b NOT caught"; exit 1; else echo "mutant $b caught"; fi
  done
fi
