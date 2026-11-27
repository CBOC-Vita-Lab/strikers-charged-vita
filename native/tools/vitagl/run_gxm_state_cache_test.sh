#!/bin/bash
# vitaGL's sceGxm state cache (patch 0013, utils/gxm_cache.h) against a mock of sceGxm: the hardware record at every draw equals the uncached run's, for random
# call streams (scene changes, a second context, repeats) with every group of the cache and with each group alone.   native/tools/vitagl/run_gxm_state_cache_test.sh
set -e
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$D/../../.."
OUT=${1:-$(mktemp -d)}
cc -O2 -Wall -Wno-unused-function -DGXM_STATE_CACHE -I"$ROOT/dependencies/vitaGL/source/utils" "$D/gxm_state_cache_test.c" -o "$OUT/gxm_state_cache_test"
"$OUT/gxm_state_cache_test"
