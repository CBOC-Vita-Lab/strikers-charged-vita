#!/bin/bash
# vitaGL patch 0036 (MSC_VGL_GC_PAUSE_MS): the bounded recovery of a failed GPU allocation. vitagl_gc_retry.c runs dependencies/vitaGL/source/utils/gc_retry.h (as it is) over a
# model: a GPU that is busy until finished, a collector cycle that must not run on a busy GPU, memory that appears after k cycles or after some milliseconds of waiting,
# a clock that only the waits advance. Checks: finish once and first; stop at the first success; wait only after the cycles, in 2 ms steps, never over the limit (the old
# code waited 4 x 1 s); limit 0 = no wait; a failed or unacknowledged cycle adds no wait; counters exact, also from four threads.
# BREAK=1: no GPU finish; BREAK=2: the wait limit is 1000x (the old second); BREAK=3: a wait after an unacknowledged cycle too. Each must fail.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
SRC=$VGX/../dependencies/vitaGL/source/utils
BUILD=${TEST_BUILD:-$(mktemp -d)}
cp "$SRC/gc_retry.h" "$BUILD/gc_retry.h"
case "${BREAK:-0}" in
  1) sed -i 's/ops->finish(ops->ctx);/(void)0;/' "$BUILD/gc_retry.h" ;;
  2) sed -i 's/el >= pause_us/el >= pause_us * 1000/' "$BUILD/gc_retry.h" ;;
  3) sed -i 's/last == 2 && pause_us/pause_us/' "$BUILD/gc_retry.h" ;;
esac
cc -O1 -Wall -Werror -I"$BUILD" -o "$BUILD/gcretry" "$VGX/tests/vitagl_gc_retry.c" -lpthread
"$BUILD/gcretry"
