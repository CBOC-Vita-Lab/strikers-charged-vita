#!/bin/bash
# Host test of VitaLog::FlushBlocking / NpVitaLogFlush (the log tail at the end of a run). Usage: run_log_tail_test.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_log_tail_test}; mkdir -p "$O/d"
g++ -std=gnu++17 -O1 -g -Wall -pthread -I"$H/../include" -I"$H/../src" -I"$H/../../compat" -I"$H/../../harness/include" \
    "$H/test_log_tail.cpp" "$H/../src/vita_log.cpp" -o "$O/t_tail"
"$O/t_tail" "$O/d"
