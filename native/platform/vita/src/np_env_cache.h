// nxboot25 / R2-12: the fourth-core readers (vita_threads.cpp) asked getenv at EVERY call (c3def18: the plan's defaults may be set after an early reader, so nothing was kept).
// On a console that is 5.7-14.3 takes of the environment lock a picture (6032.log, 6027.log [env] lines). The environment cannot change once the plan is applied: the only writers
// are LoadEnvFile + ApplyPlan (np_vita_main.cpp, three call sites: start, "Save and start" re-read, lab RUN); the live settings rows use shared cells, never the environment.
//
// So a value is kept only between ApplyPlan's end (Settle) and the next LoadEnvFile (Unsettle). Until the first Settle nothing is kept: the answers are exactly the old ones.
// Each kept value carries the epoch it was read in; Settle/Unsettle bump the epoch, so a re-read of the settings file can never be answered from the old values.
// MSC_ENV_CACHE=0 (read at Settle) = the old behaviour: every call reads. The values are the same either way; only the number of lock takes differs.
// Header only and free of console includes: tests/test_env_cache.cpp runs it on the host (with mutants).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace NpEnvCache {

inline std::atomic<uint32_t> g_epoch{1};
inline std::atomic<bool> g_settled{false};
inline std::atomic<bool> g_on{true};
inline std::atomic<uint32_t> g_reads{0}, g_hits{0};   // statistics (reads of the environment made through Get, answers kept)

// A slot is one kept value: epoch in the high word, the int in the low word. Constant-initialised (no guard on the console: c3crash18).
struct Slot { std::atomic<uint64_t> w{0}; };

// Memory order (nxr6a, NEON r6 #5): the writer (LoadEnvFile/ApplyPlan: plain setenv writes) and the readers (cores 1-3) are on a Cortex-A9, where a release store
// without an acquire load on the reader side emits no dmb there. Every load that GATES a kept value is therefore acquire, and the epoch bumps are acq_rel:
//   Settle:   setenv writes ... g_on, g_epoch++ (release), g_settled=true (release)
//   reader:   g_settled (acquire) -> g_on, g_epoch (acquire) -> slot: a reader that sees the new epoch or settled=true also sees every environment write before Settle.
// Unsettle bumps the epoch and clears g_settled, then a full fence: both are visible before LoadEnvFile's first setenv, so a reader that is mid-answer
// can only cache under an epoch that Settle has already replaced (that pair is ignored: epoch differs). Statistics stay relaxed.
// tests/test_env_cache_tsan.cpp (ThreadSanitizer, run_env_cache_test.sh) fails if a gating load is made relaxed.
inline constexpr std::memory_order kGate = std::memory_order_acquire;
// The environment is about to be written again (LoadEnvFile): nothing kept stays valid.
inline void Unsettle() {
    g_epoch.fetch_add(1, std::memory_order_acq_rel);
    g_settled.store(false, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}
// The plan is applied; `off` = the value of MSC_ENV_CACHE (null when unset): "0" turns the keeping off.
inline void Settle(const char* off) {
    g_on.store(!(off && off[0] == '0'), std::memory_order_relaxed);
    g_epoch.fetch_add(1, std::memory_order_acq_rel);
    g_settled.store(true, std::memory_order_release);
}
inline bool Active() { return g_settled.load(kGate) && g_on.load(std::memory_order_relaxed); }

// `read` is a call that reads the environment and has no other effect on the answer (a pure function of the environment).
template <class F> inline int Get(Slot& s, F read) {
    if (!Active()) return read();
    const uint32_t e = g_epoch.load(kGate);
    const uint64_t w = s.w.load(std::memory_order_relaxed);
    if (static_cast<uint32_t>(w >> 32) == e) { g_hits.fetch_add(1, std::memory_order_relaxed); return static_cast<int32_t>(static_cast<uint32_t>(w)); }
    const int v = read();
    g_reads.fetch_add(1, std::memory_order_relaxed);
    // (a Settle/Unsettle between the epoch read and now would have bumped it: the stale pair is then ignored by the next reader)
    if (g_epoch.load(kGate) != e) return v;   // a Settle/Unsettle ran during the read: do not store the pair
    s.w.store((static_cast<uint64_t>(e) << 32) | static_cast<uint32_t>(v), std::memory_order_relaxed);
    return v;
}

}  // namespace NpEnvCache
