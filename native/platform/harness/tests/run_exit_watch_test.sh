#!/bin/bash
# Host test of src/np_exit_watch.h (the end-of-run step marks and watchdog). With "mutants" it also proves the test fails on three broken variants.
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_exit_watch_test}; mkdir -p "$O"
build() { g++ -std=gnu++17 -O1 -g -Wall -Wextra -I"$H/../src" "$@" "$H/test_exit_watch.cpp" -o "$O/t"; }
build; "$O/t"
if [ "${2:-}" = mutants ]; then
  for m in NOSTEP NORESET NOFORCE; do
    build -DNP_EXIT_WATCH_MUTANT_$m
    if "$O/t" >/dev/null 2>&1; then echo "MUTANT $m SURVIVED"; exit 1; else echo "mutant $m killed"; fi
  done
fi
