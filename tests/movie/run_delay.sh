#!/usr/bin/env bash
# Host test of the movie lanes' pacing and failure paths (MSC_THP_DELAY, MSC_THP_RECOVER, the pace line):
# native/platform/sdk/tests/test_thp_delay.cpp over native/platform/sdk/src/thp_native.cpp + thp_pacing.h. No game data, no libjpeg, no
# Vita build; a 32-bit host g++ with the address and undefined-behaviour sanitizers. About 40 s.
#
#   bash tests/movie/run_delay.sh              run the test
#   bash tests/movie/run_delay.sh --mutants    also build mutants of the pacing rules and of the failure paths; each must make the test FAIL
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
work=${TMPDIR:-/tmp}/thp-delay.$$
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1

build() {   # build <native dir> <out>
    local n=$1 out=$2 sd=$1/platform/sdk
    cat > "$work/stub.cpp" <<'STUB'
#include "thp_hwjpeg.h"
const NpJpegCodec* NpJpegCodecVita() { return nullptr; }
STUB
    nice "${CXX:-g++}" -std=gnu++11 -fno-exceptions -fno-rtti -m32 -msse2 -mfpmath=sse -idirafter /usr/include/x86_64-linux-gnu -O1 -g \
        -fsanitize=address,undefined -w -pthread -DNP_HOST=1 -DNP_TARGET_HOST32=1 -I"$sd/src" -I"$root/native/include" -I"$root/native/platform/sdk/include" \
        -I"$root/native/platform/sdk/shim" -I"$root/native/platform/sdk/shim/internal" -I"$root/native" -include np/compat.h \
        "$sd/tests/test_thp_delay.cpp" "$root/native/platform/sdk/host/np_plat_host.cpp" "$work/stub.cpp" -o "$out"
}

build "$root/native" "$work/base"
"$work/base"
[ "${1:-}" = "--mutants" ] || exit 0

# name | file | sed expression (applied to a copy); the test must fail for each.
# (Not listed: the first-picture cap of stuck limit + 100 ms, redundant with the watchdog break in the same loop; and dropping the `!(delay && sl.result != 0)` guard that keeps a dropped job out of the choice. It is equivalent in effect: a dropped job
# chosen as the picture shows nothing and the picture stays, at the cost of an older finished frame: no step back, no early picture.)
mutants=(
 "show-early|thp_pacing.h|s/if (!done\[i\] || seqs\[i\] < base || seqs\[i\] > due) continue;/if (!done[i] || seqs[i] < base) continue;/"
 "step-back|thp_pacing.h|s/if (anyShown \&\& seqs\[i\] <= lastShown) continue;//"
 "due-off-by-one|thp_pacing.h|s/return seq >= base + delay ? seq - delay : base;/return seq >= base + delay ? seq - delay + 1 : base;/"
 "pick-newest|thp_pacing.h|s/return oldestOk >= 0 ? oldestOk : newest;/return newest;/"
 "never-stale|thp_pacing.h|s/return jobSeq < Due(base, nextSeq, delay) || (anyShown \&\& jobSeq <= lastShown);/return false;/"
 "release-early|thp_native.cpp|s/const int64_t upTo = delay ? (due > g_lanes.lastShown ? due : g_lanes.lastShown) : frame;/const int64_t upTo = frame;/"
 "no-codec-watch|thp_native.cpp|s/if (g_lanes.recover <= 0 || g_hw.mode <= 0) return false;/return false;/"
 "no-pace-line|thp_native.cpp|s/if (a.calls == b.calls) return;/return;/"
 "no-starve-recovery|thp_native.cpp|s/g_lanes.starved >= (g_lanes.sticky \&\& g_lanes.recover > 2 ? 2 : g_lanes.recover) \&\& ret == 0/g_lanes.starved >= 1000000 \&\& ret == 0/"
 "recovery-threshold-1|thp_native.cpp|s/g_lanes.starved >= (g_lanes.sticky \&\& g_lanes.recover > 2 ? 2 : g_lanes.recover) \&\& ret == 0/g_lanes.starved >= 1 \&\& ret == 0/"
 "recovery-not-sticky|thp_native.cpp|s/(g_lanes.sticky \&\& g_lanes.recover > 2 ? 2 : g_lanes.recover)/g_lanes.recover/"
 "blocking-lock|thp_native.cpp|s/while (!np_mutex_trylock(\&g_hw.mu)) {/np_mutex_lock(\&g_hw.mu); while (false) {/"
 "first-wait-1ms|thp_native.cpp|s/uint32_t capMs = 1500u;/uint32_t capMs = 1u;/"
 "stuck-limit-1ms|thp_native.cpp|s/atoi(e) : 700;/atoi(e) : 1;/"
 "watch-wall-time|thp_native.cpp|s/if (d > 50u) d = 50u;//"
 "sw-watch-no-count|thp_native.cpp|s/++g_lanes.st.swStuck;/;/"
 "sw-stamp-not-cleared|thp_native.cpp|s/__atomic_store_n(\&sl.swSinceMs, 0u, __ATOMIC_RELEASE);/;/"
 "sw-watch-count-twice|thp_native.cpp|s/if (!since || since == sl.swFlagged || /if (!since || /"
 "no-heal|thp_native.cpp|s/if (g_hw.watchdogDead \&\& !__atomic_load_n/if (false \&\& !__atomic_load_n/"
)
bad=0
for m in "${mutants[@]}"; do
    IFS='|' read -r name file expr <<<"$m"
    rm -rf "$work/mut"; mkdir -p "$work/mut/platform/sdk"
    cp -r "$root/native/platform/sdk/src" "$root/native/platform/sdk/tests" "$work/mut/platform/sdk/"
    sed -i "$expr" "$work/mut/platform/sdk/src/$file"
    if cmp -s "$work/mut/platform/sdk/src/$file" "$root/native/platform/sdk/src/$file"; then echo "mutant $name: the edit did not apply"; bad=1; continue; fi
    build "$work/mut" "$work/mutbin"
    if "$work/mutbin" >"$work/mut.out" 2>&1; then echo "mutant $name: SURVIVED (the test did not notice)"; bad=1; else echo "mutant $name: caught ($(grep -c '^FAIL\|FAILED' "$work/mut.out") failing lines)"; fi
done
[ $bad = 0 ] && echo "all mutants caught" || { echo "SOME MUTANTS SURVIVED"; exit 1; }
