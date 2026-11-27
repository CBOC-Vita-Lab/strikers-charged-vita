#!/bin/bash
# MSC_VGX_ASYNC_HEAPONLY: the worker's allocation rule against fake allocators (no GL, seconds). BREAK=1 builds the mutant (the worker is not recognised): must FAIL.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/vgx-worker-alloc}
mkdir -p "$OUT"
FLAGS=(-std=c++20 -O1 -Wall -Wextra -pthread -I"$VGX/src")
[[ ${BREAK:-0} == 1 ]] && FLAGS+=(-DVGX_WORKERALLOC_BREAK=1)
${CXX:-g++} "${FLAGS[@]}" "$VGX/tests/worker_alloc.cpp" -o "$OUT/worker_alloc"
if [[ ${BREAK:-0} == 1 ]]; then
    if "$OUT/worker_alloc" >/dev/null 2>&1; then echo "worker_alloc: FAILED (the mutant passed)"; exit 1; fi
    echo "worker_alloc: the mutant is caught"; exit 0
fi
"$OUT/worker_alloc"
