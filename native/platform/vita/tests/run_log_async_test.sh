#!/bin/bash
# nxr6b host test of VitaLog::FlushAsync (the harness's mid-run flush) with mutants: the async flush that flushes nothing, and a crash-path / end-of-run flush that writes nothing,
# must each FAIL the test. Usage: run_log_async_test.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_log_async_test}; mkdir -p "$O/d"
INC=(-I"$H/../include" -I"$H/../src" -I"$H/../../compat" -I"$H/../../harness/include")
build() { g++ -std=gnu++17 -O1 -g -Wall -pthread "${INC[@]}" "$H/test_log_async.cpp" "$1" -o "$2"; }
build "$H/../src/vita_log.cpp" "$O/t_async"
rm -f "$O/d/async.log"; "$O/t_async" "$O/d"
# mutants (each a copy of vita_log.cpp with one function body emptied)
mut() {   # name, python regex of the signature line
    python3 - "$H/../src/vita_log.cpp" "$O/m_$1.cpp" "$2" <<'P'
import re, sys
src, out, sig = sys.argv[1:4]
s = open(src).read()
i = s.index(sig)
j = s.index("{", i)
k = j + 1; d = 1
while d:
    d += (s[k] == "{") - (s[k] == "}"); k += 1
body = "{ return false; }" if "bool" in sig else "{ }"
open(out, "w").write(s[:j] + body + s[k:])
P
    build "$O/m_$1.cpp" "$O/t_$1"
    rm -f "$O/d/async.log"
    if "$O/t_$1" "$O/d" >/dev/null 2>&1; then echo "MUTANT $1 SURVIVED"; exit 1; else echo "mutant $1 killed"; fi
}
mut asyncnoop "bool FlushAsync() noexcept"
mut flushnoop "void Flush() noexcept"
mut blockingnoop "bool FlushBlocking() noexcept"
echo "run_log_async_test: ok"
