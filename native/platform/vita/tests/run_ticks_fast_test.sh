#!/bin/bash
# Host test of np_ticks_fast.h (MSC_TICKS_FAST). Usage: run_ticks_fast_test.sh [outdir]. The mutant must FAIL, the real header must pass.
set -e
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_ticks_test}; mkdir -p "$O"
gcc -O2 "$H/test_ticks_fast.c" -o "$O/t_real"
gcc -O2 -DNP_TICKS_FAST_BREAK "$H/test_ticks_fast.c" -o "$O/t_mutant"
"$O/t_real"
if "$O/t_mutant" >/dev/null; then echo "MUTANT NOT CAUGHT"; exit 1; else echo "mutant caught (expected)"; fi
