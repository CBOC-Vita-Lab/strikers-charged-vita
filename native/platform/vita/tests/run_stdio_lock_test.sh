#!/bin/bash
# Host test of np_stdio_lock.cpp (the fopen wrap of the Vita build and the start-up retry). Usage: run_stdio_lock_test.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_stdio_lock_test}; mkdir -p "$O"
SRC="$H/../src/np_stdio_lock.cpp"; INC="-I$H/../include"; F="-std=gnu++17 -O2 -g -Wall -Wextra -pthread"
# 1. the lock itself, with fakes in place of libc (an overlap of two calls fails the test)
g++ $F $INC -DFAKE -D__real_fopen=fake_fopen -D__real_freopen=fake_freopen -D__real_fdopen=fake_fdopen -D__real_tmpfile=fake_tmpfile -D__real_fclose=fake_fclose \
    "$H/test_stdio_lock.cpp" "$SRC" -o "$O/t_fake"
"$O/t_fake" threads
# 2. linked with the wraps the Vita build uses: plain fopen / fclose go through the lock
g++ $F $INC "$H/test_stdio_lock.cpp" "$SRC" -o "$O/t_wrap" -Wl,--wrap=fopen -Wl,--wrap=freopen -Wl,--wrap=fdopen -Wl,--wrap=tmpfile -Wl,--wrap=fclose
"$O/t_wrap" threads
# 3. the retry: injected failures, the bound, a missing file, the no-failure path
MSC_BOOTIO_INJECT=2 "$O/t_wrap" inject
MSC_BOOTIO_INJECT=100000 MSC_BOOTIO_MS=400 "$O/t_wrap" giveup
MSC_BOOTIO_MS=300 "$O/t_wrap" missing
"$O/t_wrap" now
echo "run_stdio_lock_test: all ok"
