#!/bin/bash
# nxboot25 R2-12: host test of np_env_cache.h with mutants. Usage: run_env_cache_test.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_env_cache_test}; mkdir -p "$O/m"
F="-std=gnu++17 -O1 -g -pthread"
g++ $F -I"$H/../src" "$H/test_env_cache.cpp" -o "$O/t_real"
"$O/t_real"
# mutants: a copy of the header with one rule broken; the test must fail for each
mut() {   # name, sed expression
    local d="$O/m/$1"; mkdir -p "$d"; sed "$2" "$H/../src/np_env_cache.h" > "$d/np_env_cache.h"
    cmp -s "$d/np_env_cache.h" "$H/../src/np_env_cache.h" && { echo "run_env_cache_test: mutant $1 did not change the header"; exit 1; }
    g++ $F -I"$d" "$H/test_env_cache.cpp" -o "$d/t"
    if "$d/t" > "$d/out" 2>&1; then echo "run_env_cache_test: MUTANT $1 SURVIVED: the test cannot fail for it"; exit 1; fi
    echo "  mutant $1 killed ($(grep -c FAIL "$d/out") checks)"
}
mut no_epoch_check   's/if (static_cast<uint32_t>(w >> 32) == e)/if (w != 0)/'
mut keep_unsettled   's/return g_settled.load(kGate) \&\& g_on/return g_on/'
mut ignore_off       's/g_on.store(!(off \&\& off\[0\] == .0.), std::memory_order_relaxed);/g_on.store(true, std::memory_order_relaxed);/'
mut no_epoch_bump    's/g_epoch.fetch_add(1, std::memory_order_acq_rel)/(void)0/'   # (both bumps: each alone is covered by the other, an unsettled epoch is never read)
mut drop_negative    's/return static_cast<int32_t>(static_cast<uint32_t>(w));/return static_cast<int32_t>(static_cast<uint32_t>(w)) < 0 ? read() : static_cast<int32_t>(static_cast<uint32_t>(w));/'
# nxr6a: memory-order litmus under ThreadSanitizer (x86 TSO hides a relaxed gate load from any value test; TSan models the happens-before edge)
if echo 'int main(){}' | g++ -x c++ -fsanitize=thread - -o "$O/tsan_probe" 2>/dev/null; then
    g++ $F -fsanitize=thread -I"$H/../src" "$H/test_env_cache_tsan.cpp" -o "$O/t_tsan"
    "$O/t_tsan"
    d="$O/m/relaxed_gate"; mkdir -p "$d"
    sed 's/kGate = std::memory_order_acquire/kGate = std::memory_order_relaxed/' "$H/../src/np_env_cache.h" > "$d/np_env_cache.h"
    cmp -s "$d/np_env_cache.h" "$H/../src/np_env_cache.h" && { echo "run_env_cache_test: mutant relaxed_gate did not change the header"; exit 1; }
    g++ $F -fsanitize=thread -I"$d" "$H/test_env_cache_tsan.cpp" -o "$d/t"
    if "$d/t" > "$d/out" 2>&1; then echo "run_env_cache_test: MUTANT relaxed_gate SURVIVED: TSan did not flag a relaxed gate load"; exit 1; fi
    echo "  mutant relaxed_gate killed by ThreadSanitizer ($(grep -c 'data race' "$d/out") reports)"
else echo "run_env_cache_test: no ThreadSanitizer here: memory-order litmus skipped"; fi
echo "run_env_cache_test: ok"
