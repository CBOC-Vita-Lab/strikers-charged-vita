#!/bin/bash
# crash26b host stress test of np_log_stdio.h through vita_silent_stdio.cpp's real wrappers, linked with the Vita's --wrap list.
# Usage: run_log_stdio_test.sh [seconds] [outdir]. Runs the fix (must PASS) and the no-lock mutant (must be caught).
set -eu
H=$(cd "$(dirname "$0")" && pwd); S=${1:-20}; O=${2:-/tmp/np_log_stdio_test}; mkdir -p "$O"
W="-Wl,--wrap=printf -Wl,--wrap=vprintf -Wl,--wrap=fprintf -Wl,--wrap=vfprintf -Wl,--wrap=puts -Wl,--wrap=putchar -Wl,--wrap=fputs -Wl,--wrap=fputc -Wl,--wrap=putc -Wl,--wrap=fwrite"
F="-std=gnu++17 -O1 -g -Wall -pthread -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DNP_LOGGING=1 -DNP_LOGSTDIO_HOST_TEST=1 -I$H/../include -I$H/../src -I$H/../../compat -I$H/../../harness/include"
g++ $F "$H/test_log_stdio.cpp" "$H/../src/vita_silent_stdio.cpp" $W -static-libstdc++ -lrt -o "$O/t_log_stdio"
g++ $F -DNP_LOGSTDIO_MUTANT_NOLOCK=1 "$H/test_log_stdio.cpp" "$H/../src/vita_silent_stdio.cpp" $W -static-libstdc++ -lrt -o "$O/t_log_stdio_mutant"
# the binaries must really call the wraps (the test is void otherwise)
nm "$O/t_log_stdio" | grep -q " T __wrap_vfprintf" && nm "$O/t_log_stdio" | grep -q " T __wrap_fputc" || { echo "[log_stdio] FAIL: the wraps are not linked"; exit 1; }
"$O/t_log_stdio" "$S"
if "$O/t_log_stdio_mutant" "$S" mutant; then :; else echo "[log_stdio] FAIL (mutant run)"; exit 1; fi
