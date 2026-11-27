#!/bin/bash
# Host test of np_fasttls.h (MSC_TLS_FAST). Usage: run_fasttls_test.sh [outdir]. The mutant (key compare <=) must FAIL, the real header must pass.
set -e
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_fasttls_test}; mkdir -p "$O"
g++ -O2 -std=gnu++17 "$H/test_fasttls.cpp" -o "$O/t_real"
g++ -O2 -std=gnu++17 '-DNP_FASTTLS_KEYEQ(a,b)=((a)<=(b))' "$H/test_fasttls.cpp" -o "$O/t_mutant"
"$O/t_real"
if "$O/t_mutant"; then echo "MUTANT NOT CAUGHT"; exit 1; else echo "mutant caught (expected)"; fi
