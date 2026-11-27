// Recorder: game-thread side of the record stream (see vgx_rec.h).
#include "vgx_rec.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include "vgx_workload.h"
#include <ctime>
#include <cstdio>
#include <cstdlib>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include "vgx_sync.h"
#include <mutex>
#include <vector>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

extern "C" void NpGsWaitBegin(void) __attribute__((weak));   // (native/platform/compat/np_gsplit.cpp: absent in other builds)
extern "C" void NpGsWaitEnd(void) __attribute__((weak));
namespace vgx::rec {

Producer g_prod;
#if defined(VGX_PRODUCER_CHECK)
void (*g_producerHook)() = nullptr;
#endif
int g_mode = MODE_THREAD;
bool g_immOpen = false;
Shadow gs;

// ---------------------------------------------------------------- chunks
namespace {
constexpr uint32_t kChunkWords = 1u << 16; // 256 KiB
std::mutex g_freeMtx;
std::vector<Chunk*> g_free; // standard-size chunks handed back by the consumer

std::atomic<uint32_t> g_chunkSerial{0};
// 1.3: a chunk the C heap cannot give is waited for, not written through (1.1 and 1.2 wrote through the null: C2-12828-1 with no word about memory). The player
// hands standard chunks back (RecycleChunk) as it reads them and a pending frame frees its own memory, so for up to ~3 s the producer looks again for a spare
// standard chunk, frees the spares it holds for a large request, and asks malloc again; only then does it end the program, with a line saying why.
extern "C" void NpHostHeapExhausted(size_t) __attribute__((weak));   // nlMemory.cpp (native builds)
Chunk* NewChunk(uint32_t cap) {
    const size_t bytes = sizeof(Chunk) + static_cast<size_t>(cap) * 4;
    Chunk* c = nullptr;
    for (int attempt = 0;; ++attempt) {
        if (cap == kChunkWords) {
            std::lock_guard<std::mutex> lk(g_freeMtx);
            if (!g_free.empty()) {
                c = g_free.back();
                g_free.pop_back();
                c->next.store(nullptr, std::memory_order_relaxed);
                c->committed.store(0, std::memory_order_relaxed);
                c->serial.store(g_chunkSerial.fetch_add(1, std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
                c->state.store(0, std::memory_order_relaxed);
                return c;
            }
        }
        c = static_cast<Chunk*>(std::malloc(bytes));
        if (c) break;
        if (attempt == 0) std::fprintf(stderr, "[vgx] record chunk of %lu KiB: the C heap has no room; waiting for the player to hand chunks back\n", static_cast<unsigned long>(bytes >> 10));
        if (attempt >= 1500) {
            if (NpHostHeapExhausted) NpHostHeapExhausted(bytes);
            std::fprintf(stderr, "FATAL: host heap exhausted: a record chunk of %lu KiB found no room\n", static_cast<unsigned long>(bytes >> 10));
            std::abort();
        }
        if (cap != kChunkWords) { // (a large chunk cannot use a spare: give the spares back to the heap)
            std::lock_guard<std::mutex> lk(g_freeMtx);
            while (!g_free.empty()) { std::free(g_free.back()); g_free.pop_back(); }
        }
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, nullptr);
    }
    new (c) Chunk();
    c->cap = cap;
    c->serial.store(g_chunkSerial.fetch_add(1, std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    return c;
}

uint64_t (*g_probe)(const void*, size_t) = nullptr;
} // namespace

// MSC_VGX_POISON=1 (diagnostic, default off): a chunk handed back is filled with 0xA5 bytes (op 165, not a record), so that a reader that still looks at it
// (the player, or a by-value array binding) fails at once instead of reading whatever the next frame wrote.
static bool PoisonOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_POISON", 0) != 0));
    return on;
}
// Called by the consumer when it leaves a chunk.
void RecycleChunk(Chunk* c) {
    if (c->cap == kChunkWords) {
        std::lock_guard<std::mutex> lk(g_freeMtx);
        if (g_free.size() < 8) {
            c->state.store(1, std::memory_order_relaxed);
            if (PoisonOn()) std::memset(c->data, 0xA5, static_cast<size_t>(c->cap) * 4);
            g_free.push_back(c);
            return;
        }
    }
    c->~Chunk();
    std::free(c);
}
// Abort diagnostics: is `c` in the free list right now?
bool ChunkInFreeList(const Chunk* c) {
    std::lock_guard<std::mutex> lk(g_freeMtx);
    for (const Chunk* f : g_free)
        if (f == c) return true;
    return false;
}
// The chunk the stream starts with. Whichever side needs it first makes it, under a mutex,
// and only the producer ever writes g_prod.
//
// Until 5 Oct the consumer's start-up did `if (!g_prod.cur) { g_prod.cur = NewChunk(...);
// g_prod.wpos = 0; }` on the PLAYER thread while the game thread's first Reserve did the same
// test and went into Grow: two unsynchronised writers of g_prod.cur. Both hosts emit the game
// thread's first record (BackendFlags in aurora_initialize) straight after EnsureThread()
// returns, which is the very moment the render thread enters PlayPending for the first time,
// so the two tests are microseconds apart at every start. When both saw "no chunk yet", each
// side made its own: the player then read a chunk nothing is ever written to, the game thread
// waited in its first Present for a frame the player could not see, and the player span in
// PlayerWaitForData (presented > decoded). Seen once in about 45 starts in Vita3K (run gl13F2:
// log ends before "first frame presented", render thread 100 % busy, game thread asleep).
namespace {
std::mutex g_startMtx;
Chunk* g_start = nullptr; // under g_startMtx
} // namespace
Chunk* FirstChunk() {
    std::lock_guard<std::mutex> lk(g_startMtx);
    if (!g_start) g_start = NewChunk(kChunkWords);
    return g_start;
}

void Grow(uint32_t words) {
    if (!g_prod.cur) { // the producer's first record
        g_prod.cur = FirstChunk();
        g_prod.wp = g_prod.cur->data;
        if (words + 1u <= g_prod.cur->cap) { RefreshLimit(); return; } // (Reserve's own test; a larger record gets a chunk of its size below)
    }
    // One spare word is always kept for the jump marker.
    const uint32_t cap = words + 2u > kChunkWords ? words + 2u : kChunkWords;
    Chunk* n = NewChunk(cap);
    if (g_prod.cur) {
        Chunk* old = g_prod.cur;
        const uint32_t wpos = Wpos();
        old->data[wpos] = Hdr(R_JUMP);
        old->next.store(n, std::memory_order_release);
        old->committed.store(wpos + 1u, std::memory_order_release);
        g_prod.wordsBefore += wpos + 1u;
    }
    g_prod.cur = n;
    g_prod.wp = n->data;
    RefreshLimit();
}

// The slow path of Reserve: a primitive that is still open is closed first (its record is committed), then a record that does not fit gets a new chunk.
uint32_t* ReserveSlow(uint32_t words) {
    if (g_immOpen) CloseImm();
    if (!g_prod.cur || Wpos() + words + 1u > g_prod.cur->cap) Grow(words);
    else RefreshLimit();
    return g_prod.wp;
}

// ------------------------------------------------- player synchronisation
namespace {
std::mutex g_syncMtx;
std::condition_variable g_syncCv;   // player -> game thread: frame decoded / executed
std::atomic<uint64_t> g_presented{0};
uint64_t g_decoded = 0, g_executed = 0; // under g_syncMtx
alignas(64) std::atomic<uint32_t> g_wakeSeq{0};  // bumped (and signalled) at present

// The idle player sleeps for at most a millisecond of *real* time. This must
// not go through the C library's clock: under the navigation harness's
// virtual clock (which replaces clock_gettime and friends for the whole
// process) a timed condition-variable wait computes a deadline that is
// already in the past and returns at once, i.e. the thread spins. A futex
// wait takes a relative time-out and no clock at all.
#if defined(__linux__)
void IdleWait(uint32_t seen) {
    struct { long sec, nsec; } ts = {0, 1000000}; // the layout SYS_futex expects on both 32- and 64-bit Linux
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&g_wakeSeq), FUTEX_WAIT_PRIVATE, seen, &ts, nullptr, 0);
}
void IdleWake() { syscall(SYS_futex, reinterpret_cast<uint32_t*>(&g_wakeSeq), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0); }
#elif defined(__vita__)
// int29/int31: direct Vita kernel binary semaphore (maxCount=1) replaces pthread-embedded mutex/cond on IdleWait/IdleWake (zero g_syncMtx contention on Core 0/2), initialized in InitMode() rather than static init.
std::atomic<SceUID> g_idleSem{-1};
void InitIdleSem() {
    if (g_idleSem.load(std::memory_order_acquire) >= 0) return;
    static std::mutex initMtx;
    std::lock_guard<std::mutex> lk(initMtx);
    if (g_idleSem.load(std::memory_order_relaxed) < 0)
        g_idleSem.store(sceKernelCreateSema("vgx_rec_idle", 0, 0, 1, nullptr), std::memory_order_release);
}
void IdleWait(uint32_t seen) {
    if (g_wakeSeq.load(std::memory_order_seq_cst) != seen) return;
    const SceUID sem = g_idleSem.load(std::memory_order_acquire);
    if (sem >= 0) {
        SceUInt us = 1000;
        sceKernelWaitSema(sem, 1, &us);
    } else {
        sceKernelDelayThread(500);
    }
}
void IdleWake() {
    const SceUID sem = g_idleSem.load(std::memory_order_acquire);
    if (sem >= 0) sceKernelSignalSema(sem, 1);
}
#else
std::condition_variable g_dataCv;
void IdleWait(uint32_t seen) {
    std::unique_lock<std::mutex> lk(g_syncMtx);
    g_dataCv.wait_for(lk, std::chrono::milliseconds(1), [seen] { return g_wakeSeq.load(std::memory_order_relaxed) != seen; });
}
// Called by Present with g_syncMtx HELD (the predicate above reads g_wakeSeq under that mutex,
// so no wake-up can be lost). It must not take the mutex itself: std::mutex is not recursive,
// and on the Vita (pthread-embedded) locking it twice blocks the game thread for ever at its
// first present, with the player blocked behind it in PlayerWaitForData. That was the state
// of this function until the first run on something that is not Linux (Vita3K, 5 Oct).
void IdleWake() { g_dataCv.notify_one(); }
#endif
} // namespace

// Player side: nothing to do. Sleeps until the game thread presents, but at
// most a millisecond: records that arrive in between are picked up by polling
// (so the game thread never pays for a wake-up while it is submitting).
// MSC_VGX_PROF (VRS, 16 Oct): the time the player spent waiting for the game thread's records, so that the
// render thread's profile line can say how much of its frame interval it was idle (the back end's `all=` is
// only its GL part; what is left of the interval is the record decode and anything that blocks outside the
// back end). TakePlayerIdleMs() is read by the profile print and resets the sum.
static std::atomic<uint64_t> g_idleNs{0};
double TakePlayerIdleMs() { return static_cast<double>(g_idleNs.exchange(0, std::memory_order_relaxed)) / 1e6; }
// MSC_VGX_WAITSTAT=1 (1.2 audit): how often the idle player goes to sleep, and how many of those sleeps ended by the 1 ms time-out (the player polls for
// the game thread's records between presents; a sleep ended by a present counts as woken). One line every 600 presents.
static std::atomic<uint64_t> g_idleSleeps{0}, g_idleTimeouts{0};
static inline bool WaitStat() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_WAITSTAT", 0) != 0));
    return on;
}
bool PlayerHasPending();
alignas(64) std::atomic<bool> g_playerIdle{false};
bool g_wakeCheckArmed = true;
uint32_t g_commitSinceWake = 0;
void WakePlayerOnce() {
    if (!g_playerIdle.load(std::memory_order_seq_cst)) return;
    if (!g_playerIdle.exchange(false, std::memory_order_seq_cst)) return;
#if defined(__linux__) || defined(__vita__)
    g_wakeSeq.fetch_add(1, std::memory_order_seq_cst);
    IdleWake();
#else
    std::lock_guard<std::mutex> lk(g_syncMtx);
    g_wakeSeq.fetch_add(1, std::memory_order_release);
    IdleWake();
#endif
}
void PlayerWaitForData() {
    const uint32_t seen = g_wakeSeq.load(std::memory_order_acquire);
    {
        // Not while the game thread is already waiting for this frame to be consumed.
        std::lock_guard<std::mutex> lk(g_syncMtx);
        if (g_presented.load(std::memory_order_relaxed) > g_decoded) return;
    }
    g_playerIdle.store(true, std::memory_order_seq_cst);
    // int28 (Carbon M1): Dekker seq_cst recheck of both wakeSeq AND pending chunk words prevents StoreLoad lost wake.
    if (g_wakeSeq.load(std::memory_order_seq_cst) != seen || PlayerHasPending()) {
        g_playerIdle.store(false, std::memory_order_relaxed);
        return;
    }
    const bool prof = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_PROF", 0) != 0));
    if (!prof) {
        IdleWait(seen);
        g_playerIdle.store(false, std::memory_order_relaxed);
        if (WaitStat()) {
            g_idleSleeps.fetch_add(1, std::memory_order_relaxed);
            if (g_wakeSeq.load(std::memory_order_relaxed) == seen) g_idleTimeouts.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    IdleWait(seen);
    g_playerIdle.store(false, std::memory_order_relaxed);
    g_idleNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                       std::memory_order_relaxed);
}
// W5 PIPE (Vita): the game thread's wait in Present is a kernel semaphore kick plus the counters, not only a condition variable. The counters stay the truth
// (g_decoded >= n, tested under the mutex); each decode posts the semaphore after the counter was raised, the waiter takes one post per round and tests again. A post
// cannot be lost (a signal between the predicate test and the block can: VRS, 16 Oct), a surplus post only costs one more round. MSC_VGX_SYNC=cv: the old wait.
namespace {
inline bool SemaSync() {
    const bool on = static_cast<bool>(VGX_KNOB([] { const char* e = std::getenv("MSC_VGX_SYNC"); return !(e != nullptr && e[0] == 'c'); }()));
    return on;
}
#if defined(__vita__)
Sema& g_semDecoded = *new Sema(0, "vgx frame decoded");
#endif
} // namespace
void NotifyDecoded() {
    {
        std::lock_guard<std::mutex> lk(g_syncMtx);
        ++g_decoded;
    }
    g_syncCv.notify_all();
#if defined(__vita__)
    if (SemaSync()) g_semDecoded.Post();
#endif
}
void NotifyExecuted() {
    {
        std::lock_guard<std::mutex> lk(g_syncMtx);
        ++g_executed;
    }
    g_syncCv.notify_all();
}
bool WaitPresented(unsigned timeoutMs) {
    if (g_mode != MODE_THREAD) return true; // These modes submit synchronously.
    std::unique_lock<std::mutex> lk(g_syncMtx);
    const uint64_t n = g_presented.load(std::memory_order_relaxed);
    return g_syncCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [n] { return g_executed >= n; });
}
// MSC_VGX_ASYNCLOG=1 (VRS, 16 Oct): the counters of the game / player / back end hand-shake for a stall of MSC_VGX_BACKEND_THREAD=1 (read without the lock: a diagnostic).
void AsyncDump(const char* tag, uint32_t playerStage, uint32_t backStage, uint32_t execCmd, uint64_t handed, uint64_t drawn) {
    const bool got = g_syncMtx.try_lock();
    std::fprintf(stderr, "[vgx-async] %s: presented %llu decoded %llu executed %llu | handed over %llu drawn %llu | player stage %u, back end stage %u, executing cmd type %u #%u | sync mutex %s\n", tag,
                 static_cast<unsigned long long>(g_presented.load()), static_cast<unsigned long long>(g_decoded), static_cast<unsigned long long>(g_executed),
                 static_cast<unsigned long long>(handed), static_cast<unsigned long long>(drawn), playerStage, backStage, execCmd >> 24, execCmd & 0xFFFFFFu, got ? "free" : "HELD");
    if (got) g_syncMtx.unlock();
}

void AfterCommitSlow() { PlayPending(); } // immediate mode only
bool g_publishEach = true; // until InitMode has read the mode
bool g_prefetchRec = false; // MSC_PREFETCH bit 4
void PublishSlow() {
    g_prod.cur->committed.store(Wpos(), std::memory_order_release);
    if (g_mode == MODE_IMMEDIATE) AfterCommitSlow();
}

void InitMode() {
    static bool done = false;
    if (done) return;
    done = true;
    GxdInit();
    const char* m = std::getenv("MSC_VGX_PLAY");
    if (m && !std::strcmp(m, "immediate")) g_mode = MODE_IMMEDIATE;
    else if (m && !std::strcmp(m, "frame")) g_mode = MODE_FRAME;
    else g_mode = MODE_THREAD;
    g_publishEach = g_mode == MODE_IMMEDIATE || EnvInt("MSC_VGX_LAZYPUB", 1) == 0;
    g_prefetchRec = (EnvInt("MSC_PREFETCH", 0) & 4) != 0;
#if defined(__vita__)
    InitIdleSem();
#endif
}

// MSC_VGX_RECSTAT=1: every 600 presents one line with the size of the record stream the game
// thread wrote per frame (what crosses from its core to the player's).
static void RecordStreamStat() {
    static uint64_t last = 0, sum = 0, peak = 0, frames = 0;
    const uint64_t now = g_prod.wordsBefore + (g_prod.cur ? Wpos() : 0u);
    const uint64_t n = now - last;
    last = now;
    sum += n;
    if (n > peak) peak = n;
    if (++frames % 600 == 0) {
        std::fprintf(stderr, "[vgx] record stream, last 600 frames: mean %u KiB per frame, largest %u KiB (chunks of %u KiB)\n",
                     static_cast<unsigned>(sum * 4 / 600 / 1024), static_cast<unsigned>(peak * 4 / 1024), kChunkWords * 4u / 1024u);
        sum = peak = 0;
    }
}

void Present(bool waitExecuted, uint64_t latencyTag) {
    workload::ProducerTick(); // (MSC_VGX_WORKLOAD_PROBE: the send column)
    if(latencyTag){
        uint32_t* r = Reserve(3);
        r[0] = Hdr(R_PRESENT,1);
        r[1] = static_cast<uint32_t>(latencyTag);
        r[2] = static_cast<uint32_t>(latencyTag >> 32);
        Commit(3);
    }else Emit0(R_PRESENT);
    const bool recStat = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_RECSTAT", 0) != 0));
    if (recStat) RecordStreamStat();
    if (WaitStat()) {
        static uint64_t presents = 0, lastSleeps = 0, lastTimeouts = 0;
        if (++presents % 600 == 0) {
            const uint64_t sl = g_idleSleeps.load(std::memory_order_relaxed), to = g_idleTimeouts.load(std::memory_order_relaxed);
            std::fprintf(stderr, "[vgx] player sleeps, last 600 presents: %.1f per present, %.1f of them ended by the time-out (polls for records)\n",
                         static_cast<double>(sl - lastSleeps) / 600.0, static_cast<double>(to - lastTimeouts) / 600.0);
            lastSleeps = sl;
            lastTimeouts = to;
        }
    }
    if (g_mode != MODE_THREAD) {
        if (g_mode == MODE_FRAME) PlayPending();
        return; // FrontPresent handed the buffer to the render thread synchronously
    }
    std::unique_lock<std::mutex> lk(g_syncMtx);
    const uint64_t n = g_presented.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(__vita__)
    g_wakeSeq.fetch_add(1, std::memory_order_seq_cst);
    if (g_playerIdle.load(std::memory_order_seq_cst) && g_playerIdle.exchange(false, std::memory_order_seq_cst))
        IdleWake();
#else
    g_wakeSeq.fetch_add(1, std::memory_order_release);
    IdleWake();
#endif
    struct GsWait { GsWait() { if (NpGsWaitBegin) NpGsWaitBegin(); } ~GsWait() { if (NpGsWaitEnd) NpGsWaitEnd(); } } gsWait;   // (MSC_GSPLIT: the hand-over wait below is blocked time, not CPU)
    if (waitExecuted) {
        g_syncCv.wait(lk, [n] { return g_executed >= n; });
    } else {
#if defined(__vita__)
        if (SemaSync()) {
            while (g_decoded < n) {
                lk.unlock();
                g_semDecoded.Wait();
                lk.lock();
            }
            g_wakeCheckArmed = true;
            g_commitSinceWake = 0;
            return;
        }
#endif
        g_syncCv.wait(lk, [n] { return g_decoded >= n; });
    }
    g_wakeCheckArmed = true;
    g_commitSinceWake = 0;
}

// ---------------------------------------------------------------- shadow
namespace {
PlanCache g_shadowPlans;
} // namespace

namespace {
// Last R_VTXSTATE sent, while nothing else can have changed the player's
// descriptor / formats since (API descriptor calls, streams with CP writes).
struct { bool valid = false; int fmt = 0; uint8_t vcd[21]; hle::VtxFmtRow row[12]; } g_vtxSent;
} // namespace

static void ShadowReset() {
    static State* tmp = new State(); // large; only its register part is copied
    InitState(*tmp);
    std::memcpy(gs.bp, tmp->bp, sizeof gs.bp);
    gs.bpMask = tmp->bpMask;
    std::memcpy(gs.xfReg, tmp->xfReg, sizeof gs.xfReg);
    gs.matIdxA = tmp->matIdxA;
    gs.matIdxB = tmp->matIdxB;
    std::memcpy(gs.vcd, tmp->vcd, sizeof gs.vcd);
    std::memcpy(gs.vat, tmp->vat, sizeof gs.vat);
    gs.layoutDirty = true;
    gs.dispCopySrc = tmp->dispCopySrc;
    gs.texCopySrc = tmp->texCopySrc;
    gs.dispCopyDstW = tmp->dispCopyDstW; gs.dispCopyDstH = tmp->dispCopyDstH;
    gs.texCopyDstW = tmp->texCopyDstW; gs.texCopyDstH = tmp->texCopyDstH;
    gs.texCopyFmt = tmp->texCopyFmt;
    gs.texCopyHalf = tmp->texCopyHalf;
    gs.efbW = tmp->efbW; gs.efbH = tmp->efbH;
    for (ArrayRef& a : gs.arrays) a = ArrayRef{};
}
// The front end's register file starts in the same state without a record.
static const struct ShadowInit { ShadowInit() { ShadowReset(); } } g_shadowInit;

bool g_recFast = true;
int g_recBreak = 0;
uint64_t g_recCnt[4] = {0, 0, 0, 0};
static void RecFastStats() { std::fprintf(stderr, "[recfast] BPSlow calls %llu, (unused) %llu, fog memo hits %llu, fog computed %llu\n", (unsigned long long)g_recCnt[0], (unsigned long long)g_recCnt[1], (unsigned long long)g_recCnt[2], (unsigned long long)g_recCnt[3]); }

void Reset() {
    g_recFast = EnvInt("MSC_REC_FAST", 1) != 0;
    g_recBreak = EnvInt("MSC_REC_FAST_BREAK", 0);
    if (EnvInt("MSC_REC_FAST_STATS", 0)) std::atexit(RecFastStats);
    GxdInit();
    ShadowReset();
    g_vtxSent.valid = false;
    g_wakeCheckArmed = true;
    g_commitSinceWake = 0;
    Emit0(R_RESET);
}

// ------------------------------------------------- direct HLE interface
} // namespace vgx::rec

namespace vgx::hle {
using namespace vgx::rec;

RawCursor g_rawCur = {nullptr, nullptr};
namespace {
struct RawOpen { uint32_t* rec = nullptr; uint8_t* base = nullptr; uint32_t vsize = 0; uint8_t prim = 0; int fmt = 0; bool open = false; } g_raw;
}

bool Enabled() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_FAST", 1) != 0));
    return on;
}

void SetVertexState(int fmt, const uint8_t vcd[21], const VtxFmtRow row[12]) {
    fmt &= 7;
    if (g_vtxSent.valid && g_vtxSent.fmt == fmt) {
        // (GXD_WORDCMP: the same two compares as words)
        if (Gxd(GXD_WORDCMP) ? (SameWords(g_vtxSent.vcd, vcd, 5) && g_vtxSent.vcd[20] == vcd[20] && SameWords(g_vtxSent.row, row, 9))
                             : (std::memcmp(g_vtxSent.vcd, vcd, 21) == 0 && std::memcmp(g_vtxSent.row, row, 36) == 0))
            return;
    }
    // Shadow (GXGetVtxDesc, immediate-mode plans).
    for (int a = 0; a <= 20; ++a)
        if (gs.vcd[a] != vcd[a]) { gs.vcd[a] = vcd[a]; gs.layoutDirty = true; }
    for (int a = 9; a <= 20; ++a) {
        VtxAttrFmt& f = gs.vat[fmt][a];
        const VtxFmtRow& n = row[a - 9];
        if (f.cnt != n.cnt || f.type != n.type || f.frac != n.frac) { f.cnt = n.cnt; f.type = n.type; f.frac = n.frac; gs.layoutDirty = true; }
    }
    uint32_t* r = Reserve(16);
    r[0] = Hdr(R_VTXSTATE, static_cast<uint32_t>(fmt));
    r[15] = 0;
    uint8_t* b = reinterpret_cast<uint8_t*>(r + 1);
    std::memcpy(b, vcd, 21);
    std::memcpy(b + 21, row, 36);
    CommitState(16);
    g_vtxSent.valid = true;
    g_vtxSent.fmt = fmt;
    std::memcpy(g_vtxSent.vcd, vcd, 21);
    std::memcpy(g_vtxSent.row, row, 36);
}

// (The interface's contract: hostPtr is guest RAM. No classification of the pointer.)
void SetArray(int attr, const void* hostPtr, uint32_t sizeBound, uint32_t stride) { Array(attr, hostPtr, sizeBound, stride, false, true); }

void CallList(const void* hostPtr, uint32_t nbytes) {
    if (hostPtr && nbytes) Stream(static_cast<const uint8_t*>(hostPtr), nbytes, false);
}

bool BeginRaw(uint8_t prim, int fmt, uint32_t count, uint32_t vertexBytes) {
    if (count == 0 || vertexBytes == 0) return false;
    const uint32_t n = count * vertexBytes;
    g_raw.rec = Reserve(3 + WordsFor(n)); // closes a primitive that is still open
    g_raw.base = reinterpret_cast<uint8_t*>(g_raw.rec + 3);
    g_raw.vsize = vertexBytes;
    g_raw.prim = prim;
    g_raw.fmt = fmt & 7;
    g_raw.open = true;
    g_immOpen = true;
    g_prod.limit = 0; // the next Reserve takes the slow path and closes the primitive
    g_rawCur.ptr = g_raw.base;
    g_rawCur.end = g_raw.base + n;
    return true;
}

void FinishRaw() {
    if (!g_raw.open) return;
    const uint32_t count = g_raw.vsize ? static_cast<uint32_t>(g_rawCur.ptr - g_raw.base) / g_raw.vsize : 0u;
    g_raw.open = false;
    g_immOpen = false;
    RefreshLimit();
    g_rawCur.ptr = g_rawCur.end = nullptr;
    if (!count) return;
    const uint32_t n = count * g_raw.vsize;
    g_raw.rec[0] = Hdr(R_DRAW_RAW, static_cast<uint32_t>(g_raw.prim) | (static_cast<uint32_t>(g_raw.fmt) << 8));
    g_raw.rec[1] = count;
    g_raw.rec[2] = n;
    Commit(3 + WordsFor(n));
}

void LoadXFBE(uint32_t addr, uint32_t count, const void* be) {
    uint32_t* r = Reserve(2 + count);
    r[0] = Hdr(R_XF, count);
    r[1] = addr;
    // (a word load and a byte reversal per word; written out because the byte-wise be32
    // of vgx_common.h is not turned into one by the compiler on ARM)
    const uint8_t* s = static_cast<const uint8_t*>(be);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t w;
        std::memcpy(&w, s + 4 * i, 4);
        r[2 + i] = __builtin_bswap32(w);
    }
    CommitState(2 + count);
}

uint32_t ShadowStreamVertexBytes(int fmt) {
    const Plan& p = GetPlanIn(g_shadowPlans, gs.layoutDirty, gs.vcd, gs.vat, fmt & 7, false);
    return p.valid ? p.streamSize : 0u;
}

void LoadNrmMtxBE(uint32_t addr, const void* be) {
    uint32_t* r = Reserve(2 + 9);
    r[0] = Hdr(R_XF, 9);
    r[1] = addr;
    const uint8_t* s = static_cast<const uint8_t*>(be);
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 3; ++c) {
            uint32_t w;
            std::memcpy(&w, s + 16 * row + 4 * c, 4);
            r[2 + row * 3 + c] = __builtin_bswap32(w);
        }
    CommitState(2 + 9);
}

} // namespace vgx::hle

namespace vgx::rec {

void BPSlow(uint32_t rv) {
    ++g_recCnt[0];
    const uint8_t reg = static_cast<uint8_t>(rv >> 24);
    const uint32_t val = rv & 0x00FFFFFFu;
    if (reg == 0xFE) { // mask for the next write only
        gs.bpMask = val;
        return;
    }
    const uint32_t nv = (gs.bp[reg] & ~gs.bpMask) | (val & gs.bpMask);
    gs.bpMask = 0x00FFFFFFu;
    if (reg >= 0xE0 && reg <= 0xE7) {
        // TEV colour registers and konst colours share an address: always forwarded.
        gs.bp[reg] = nv;
        Emit1State(R_BP, 0, (static_cast<uint32_t>(reg) << 24) | nv);
        return;
    }
    switch (reg) {
    case 0x45: case 0x47: case 0x48: // PE done / tokens: frame sync is handled by the HLE
        return;
    case 0x52:
        VGX_LOG_N(4, "raw BP EFB-copy trigger 0x%06X ignored (copies arrive through GXCopyTex/GXCopyDisp)\n", nv);
        break;
    default:
        if ((reg >= 0x64 && reg <= 0x67) || (reg >= 0x80 && reg <= 0xBB)) {
            // Texture / TLUT hardware registers: textures are bound through GXLoadTexObj here.
            VGX_LOG_N(8, "raw BP texture register 0x%02X = 0x%06X ignored\n", reg, nv);
        }
        break;
    }
    // Not compared with the shadow: a display list may have changed the real
    // register since (the shadow, like the SDK's, only sees API calls). The
    // player drops writes that change nothing.
    gs.bp[reg] = nv;
    Emit1State(R_BP, 0, (static_cast<uint32_t>(reg) << 24) | nv);
}

void BPFieldSlow(uint8_t reg, int shift, int count, uint32_t value) {
    const uint32_t mask = ((1u << count) - 1u) << shift;
    const uint32_t nv = ((gs.bp[reg] & ~mask) | ((value << shift) & mask)) & 0x00FFFFFFu;
    gs.bp[reg] = nv;
    Emit1State(R_BP, 0, (static_cast<uint32_t>(reg) << 24) | nv);
}

void XF(uint32_t addr, uint32_t count, const uint32_t* w) {
    if (addr + count > 0x1000) { // registers; matrix / light memory (below) is not shadowed
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t a = addr + i;
            if (a >= 0x1000 && a < 0x1058) {
                if (a == 0x1018) gs.matIdxA = w[i];
                else if (a == 0x1019) gs.matIdxB = w[i];
                gs.xfReg[a - 0x1000] = w[i];
            }
        }
    }
    uint32_t* r = Reserve(2 + count);
    r[0] = Hdr(R_XF, count);
    r[1] = addr;
    CopyWords(r + 2, w, count);
    CommitState(2 + count);
}

void Vcd(int attr, int type) {
    if (attr < 0 || attr > 20) return; // see SetVcd: NBT and the XF arrays have no descriptor
    g_vtxSent.valid = false;
    if (gs.vcd[attr] != type) {
        gs.vcd[attr] = static_cast<uint8_t>(type);
        gs.layoutDirty = true;
    }
    Emit0State(R_VCD, static_cast<uint32_t>(attr) | (static_cast<uint32_t>(type) << 8));
}
void Vat(int fmt, int attr, int cnt, int type, int frac) {
    if (fmt < 0 || fmt > 7 || attr < 9 || attr > 20) return;
    g_vtxSent.valid = false;
    VtxAttrFmt& f = gs.vat[fmt][attr];
    if (f.cnt != cnt || f.type != type || f.frac != frac) {
        f.cnt = static_cast<uint8_t>(cnt);
        f.type = static_cast<uint8_t>(type);
        f.frac = static_cast<uint8_t>(frac);
        gs.layoutDirty = true;
    }
    Emit1State(R_VAT, static_cast<uint32_t>(fmt) | (static_cast<uint32_t>(attr) << 4) | (static_cast<uint32_t>(cnt & 15) << 12) |
                     (static_cast<uint32_t>(type & 15) << 16),
          static_cast<uint32_t>(frac));
}

void SetGuestMemoryProbe(uint64_t (*fn)(const void*, size_t)) { g_probe = fn; }
bool IsGuestMemory(const void* p, uint32_t n) {
    if (!p) return false;
    if (HaveGuestRam()) return IsGuestRam(p, n); // two range compares (banks registered by the HLE layer)
    if (!g_probe) return false;
    // The probe folds one counter per 64 KiB of the range: classify by the two ends only.
    if (g_probe(p, 1) == ~0ull) return false;
    return n <= 1 || g_probe(static_cast<const uint8_t*>(p) + (n - 1), 1) != ~0ull;
}

void Array(int attr, const void* data, uint32_t size, uint32_t stride, bool le, bool guestRam) {
    if (attr < 0 || attr > 25) return;
    const uint32_t aux = static_cast<uint32_t>(attr) | ((le ? 1u : 0u) << 8);
    ArrayRef& sh = gs.arrays[attr];
    // Only records change the player's binding, so an identical one need not be sent again.
    // (The shadow only ever holds a by-reference binding, i.e. one that passed the test below,
    // so this is the same decision as testing first; it is asked first because the game binds
    // the same arrays 1 700 times a frame and the classification is a call.)
    if (data && sh.data == data && sh.size == size && sh.stride == stride && sh.le == le) return;
    if (g_mode == MODE_IMMEDIATE || !data || size == 0 || guestRam || IsGuestMemory(data, size)) {
        sh = ArrayRef{static_cast<const uint8_t*>(data), size, stride, le};
        uint32_t* r = Reserve(5);
        r[0] = Hdr(R_ARRAY, aux);
        r[1] = size;
        r[2] = stride;
        PutPtr(r + 3, data);
        CommitState(5);
    } else {
        // A host-side buffer the HLE layer may reuse (repacked wide-stride arrays): by value.
        sh = ArrayRef{};
        const uint32_t n = WordsFor(size);
        uint32_t* r = Reserve(3 + n);
        r[0] = Hdr(R_ARRAY_COPY, aux);
        r[1] = size;
        r[2] = stride;
        std::memcpy(r + 3, data, size);
        CommitState(3 + n);
    }
}

void Tex(int texMap, const TexObj& obj, uint32_t loadSerial) {
    const uint32_t n = WordsFor(sizeof(TexObj));
    uint32_t* r = Reserve(2 + n);
    r[0] = Hdr(R_TEX, static_cast<uint32_t>(texMap));
    r[1] = loadSerial;
    std::memcpy(r + 2, &obj, sizeof(TexObj));
    CommitState(2 + n);
}
void Tlut(int idx, const TlutObj& obj) {
    const uint32_t n = WordsFor(sizeof(TlutObj));
    uint32_t* r = Reserve(1 + n);
    r[0] = Hdr(R_TLUT, static_cast<uint32_t>(idx));
    std::memcpy(r + 1, &obj, sizeof(TlutObj));
    CommitState(1 + n);
}

void Stream(const uint8_t* data, uint32_t nbytes, bool mayWriteCp) {
    if (mayWriteCp) g_vtxSent.valid = false;
    // Display lists in guest RAM go by reference. Everything else (register
    // packets cut out of the HLE's FIFO ring, flattened lists in scratch
    // buffers) is copied; so are short guest streams, for which a copy is
    // cheaper than the bookkeeping a reference would need later.
    if (g_mode == MODE_IMMEDIATE || (nbytes > 64 && IsGuestMemory(data, nbytes))) {
        uint32_t* r = Reserve(4);
        r[0] = Hdr(R_STREAM_REF);
        r[1] = nbytes;
        PutPtr(r + 2, data);
        Commit(4);
    } else {
        const uint32_t n = WordsFor(nbytes);
        uint32_t* r = Reserve(2 + n);
        r[0] = Hdr(R_STREAM_COPY);
        r[1] = nbytes;
        std::memcpy(r + 2, data, nbytes);
        Commit(2 + n);
    }
}

void DrawRaw(uint8_t prim, int fmt, const uint8_t* data, uint32_t count, uint32_t nbytes) {
    const uint32_t n = WordsFor(nbytes);
    uint32_t* r = Reserve(3 + n);
    r[0] = Hdr(R_DRAW_RAW, static_cast<uint32_t>(prim) | (static_cast<uint32_t>(fmt & 7) << 8));
    r[1] = count;
    r[2] = nbytes;
    std::memcpy(r + 3, data, nbytes);
    Commit(3 + n);
}

static inline uint32_t Pack16(int a, int b) { return (static_cast<uint32_t>(a) & 0xFFFFu) | (static_cast<uint32_t>(b) << 16); }

static bool g_nextCopyClearOnly = false;
void NextCopyClearOnly() { g_nextCopyClearOnly = true; }
void CopyTex(const void* dest, bool clear) {
    uint32_t* r = Reserve(7);
    const bool clearOnly = g_nextCopyClearOnly && clear;   // (nxr4b: only a copy that has a clear can be "clear only")
    g_nextCopyClearOnly = false;
    r[0] = Hdr(R_COPY_TEX, (clear ? 1u : 0u) | (gs.texCopyHalf ? 2u : 0u) | (clearOnly ? 4u : 0u));
    r[1] = gs.texCopyFmt;
    r[2] = Pack16(gs.texCopySrc.x, gs.texCopySrc.y);
    r[3] = Pack16(gs.texCopySrc.w, gs.texCopySrc.h);
    r[4] = Pack16(gs.texCopyDstW, gs.texCopyDstH);
    PutPtr(r + 5, dest);
    Commit(7);
}
void CopyDisp(bool clear) {
    uint32_t* r = Reserve(3);
    r[0] = Hdr(R_COPY_DISP, clear ? 1u : 0u);
    r[1] = Pack16(gs.dispCopySrc.x, gs.dispCopySrc.y);
    r[2] = Pack16(gs.dispCopySrc.w, gs.dispCopySrc.h);
    Commit(3);
}
void DestroyCopy(const void* dest) {
    uint32_t* r = Reserve(3);
    r[0] = Hdr(R_DESTROY_COPY);
    PutPtr(r + 1, dest);
    Commit(3);
}
void SetTarget(uint16_t w, uint16_t h) { Emit1(R_SET_TARGET, 0, Pack16(w, h)); }
void BackendFlags(bool capture, bool skipDraws) { Emit0(R_FLAGS, (capture ? 1u : 0u) | (skipDraws ? 2u : 0u)); }
// W5 SKIN: state for the draws that follow (like a register write: published by the next draw's Commit).
void Ext(uint32_t kind, const uint32_t* words, uint32_t n) {
    if (n > 0xFFFFu) return;
    uint32_t* r = Reserve(1 + n);
    r[0] = Hdr(R_EXT, (kind & 0xFFu) | (n << 8));
    if (n) std::memcpy(r + 1, words, n * 4u);
    CommitState(1 + n);
}

// --------------------------------------------------------- immediate mode
// Per-component GXPosition / GXColor / ... calls (the HLE layer's slow path
// for primitives it parses itself). Vertices are assembled straight into a
// R_DRAW_DEC record in the decoded layout.
namespace {
struct Imm {
    bool open = false;
    uint8_t prim = 0;
    int fmt = 0;
    uint32_t count = 0, done = 0;
    uint32_t* rec = nullptr;
    uint8_t* base = nullptr;
    uint8_t* cur = nullptr;
    const Plan* plan = nullptr;
    int callsLeft = 0, nrmCalls = 0, colorCalls = 0, texCalls = 0;
    int texAttr[8] = {};
    int numTex = 0;
} imm;

void ImmStartVertex() {
    const VtxLayout& L = imm.plan->layout;
    imm.cur = imm.base + static_cast<size_t>(imm.done) * L.stride;
    std::memset(imm.cur, 0, L.stride);
    imm.callsLeft = imm.plan->callsPerVertex;
    imm.nrmCalls = imm.colorCalls = imm.texCalls = 0;
}
inline bool ImmReady() { return imm.open && imm.done < imm.count; }
inline void ImmAdvance() {
    if (--imm.callsLeft <= 0) {
        ++imm.done;
        if (imm.done < imm.count) ImmStartVertex();
        else ImmEnd();
    }
}
} // namespace

void ImmBegin(uint8_t prim, int fmt, uint32_t count) {
    if (g_immOpen) CloseImm();
    if (count == 0) return;
    imm.plan = &GetPlanIn(g_shadowPlans, gs.layoutDirty, gs.vcd, gs.vat, fmt, false);
    const uint32_t words = 3u + WordsFor(count * imm.plan->layout.stride);
    imm.rec = Reserve(words);
    imm.base = reinterpret_cast<uint8_t*>(imm.rec + 3);
    g_immOpen = true; // from here on any other record closes the primitive first
    g_prod.limit = 0;
    imm.open = true;
    imm.prim = prim;
    imm.fmt = fmt & 7;
    imm.count = count;
    imm.done = 0;
    imm.numTex = 0;
    for (int t = 0; t < 8; ++t)
        if (gs.vcd[13 + t]) imm.texAttr[imm.numTex++] = t;
    ImmStartVertex();
}
void ImmEnd() {
    if (!imm.open) return;
    imm.open = false;
    g_immOpen = false;
    RefreshLimit();
    if (!imm.done) return; // nothing reserved is committed
    imm.rec[0] = Hdr(R_DRAW_DEC, imm.prim);
    imm.rec[1] = imm.plan->layoutMask;
    imm.rec[2] = imm.done;
    Commit(3u + WordsFor(imm.done * imm.plan->layout.stride));
}
void CloseImm() {
    hle::FinishRaw();
    ImmEnd();
}

void ImmPos(float x, float y, float z, bool raw) {
    if (!ImmReady()) return;
    float* p = reinterpret_cast<float*>(imm.cur);
    if (raw) {
        const float s = 1.0f / static_cast<float>(1u << gs.vat[imm.fmt][9].frac);
        x *= s; y *= s; z *= s;
    }
    p[0] = x; p[1] = y; p[2] = z;
    ImmAdvance();
}
void ImmNrm(float x, float y, float z, int rawFracBits) {
    if (!ImmReady()) return;
    if (imm.nrmCalls++ == 0 && (imm.plan->layoutMask & VL_NRM)) {
        if (rawFracBits > 0) {
            const float s = 1.0f / static_cast<float>(1u << rawFracBits);
            x *= s; y *= s; z *= s;
        }
        const float n[3] = {x, y, z};
        std::memcpy(imm.cur + imm.plan->layout.offNrm, n, 12);
    }
    ImmAdvance();
}
void ImmColor(uint8_t r, uint8_t gg, uint8_t b, uint8_t a) {
    if (!ImmReady()) return;
    const VtxLayout& L = imm.plan->layout;
    const uint8_t c[4] = {r, gg, b, a};
    const bool first = imm.colorCalls++ == 0;
    if (first && (L.mask & VL_COL0)) std::memcpy(imm.cur + L.offCol0, c, 4);
    else if (L.mask & VL_COL1) std::memcpy(imm.cur + L.offCol1, c, 4);
    ImmAdvance();
}
void ImmTex(float s, float t, bool raw) {
    if (!ImmReady()) return;
    if (imm.texCalls < imm.numTex) {
        const int tex = imm.texAttr[imm.texCalls];
        if (raw) {
            const float sc = 1.0f / static_cast<float>(1u << gs.vat[imm.fmt][13 + tex].frac);
            s *= sc; t *= sc;
        }
        const float st[2] = {s, t};
        std::memcpy(imm.cur + imm.plan->layout.offTex[tex], st, 8);
    }
    ++imm.texCalls;
    ImmAdvance();
}
void ImmMtxIdx(int attr, uint8_t idx) {
    if (!ImmReady()) return;
    if (attr == 0) reinterpret_cast<float*>(imm.cur)[3] = static_cast<float>(idx);
    else if (attr >= 1 && attr <= 8 && (imm.plan->layoutMask & VL_TMI)) imm.cur[imm.plan->layout.offTmi + attr - 1] = idx;
    ImmAdvance();
}

} // namespace vgx::rec
