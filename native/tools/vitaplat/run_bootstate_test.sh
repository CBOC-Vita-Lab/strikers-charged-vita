#!/bin/bash
# The loading screen's life cycle (vita_bootstate.h): every path ends it for good; four mutants must each be caught. No game data, no device; a second.
#   tools/vitaplat/run_bootstate_test.sh
set -e
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
vita=native/platform/vita
"${CXX:-c++}" -std=c++17 -O1 -Wall -Wextra -I$vita/src $vita/tests/test_bootstate.cpp -o "$tmp/ok"
"$tmp/ok"
for b in 1 2 3 4; do
  "${CXX:-c++}" -std=c++17 -O1 -DBOOTSTATE_BREAK=$b -I$vita/src $vita/tests/test_bootstate.cpp -o "$tmp/mut$b"
  if "$tmp/mut$b" > "$tmp/mut$b.out"; then echo "mutant $b NOT caught"; exit 1; fi
  echo "mutant $b caught ($(grep -c '^FAIL' "$tmp/mut$b.out") checks failed)"
done
