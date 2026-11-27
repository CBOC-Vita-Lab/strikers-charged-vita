// vgx_fepar.inc (included by vgx_draw.cpp, once, at namespace vgx scope): MSC_VGX_FEPAR, the vertex decode of plain stream draws on a worker thread.
//
//   MSC_VGX_FEPAR=0|1|2   (code default 0)  0: the player decodes every draw's vertices itself, as before.
//       1: a stream draw whose plan carries no matrix index (the particles, the HUD, the crowds' flat quads, the replay overlays: 48 draws a picture at Vice, the
//          skinned characters come through the display-list cache) is not decoded when DrawStream meets it: the player goes on with the state (ResolveState,
//          the record merge, the index buffer: everything in order, on its own thread) and, when the draw is made, hands the decode (and, for a draw the CPU
//          transforms, SoftTransform) to the worker as a job: a copy of the plan and of the array bindings, the source bytes, the arena place the vertices go to,
//          the two matrices. The worker writes only that place. The player waits for the worker (Join) before anything that reads or moves vertices:
//          the end of the picture (before the front end tells the game that the picture's guest memory may change), a hand-back of a record chunk (the
//          sources lie in the chunks), an arena that has to grow (the vertices would move). A draw that needs its vertices at once (Z-freeze followed,
//          a trace, a soft-instanced replay draw) is decoded in the player as before. A culled-away draw is not decoded at all (it leaves no trace).
//       2: verify: every such draw is decoded and transformed in the player as with 0, and the worker makes the same vertices into a separate buffer that Join compares
//          byte for byte (a mismatch is counted and logged: "[fepar] VERIFY").
//   MSC_VGX_FEPAR_MIN=<n>   (8)  draws of fewer vertices are decoded in the player (a job costs more than they do).
//   MSC_VGX_FEPAR_WAKE=<n>  (6)  the worker is woken when this many jobs wait (it polls for MSC_VGX_FEPAR_SPIN_US after a job before it sleeps); Join wakes it at once.
//   MSC_VGX_FEPAR_SPIN_US=<n> (30)
//   MSC_VGX_FEPAR_BREAK=1|2|3  (host mutant) 1: Join does not wait and the worker is slow (a race: the picture hash must change); 2: the verify job leaves out the last vertex; 3: no worker (the rescue: pictures unchanged).
//   MSC_VGX_FEPAR_BREAK=4  (host mutant) the worker sleeps 300 ms inside every 64th job it has claimed (the join must wait for it, the pictures stay the same, no job runs twice).
//   Claim protocol: every job carries a state word (index<<2 | OPEN/CLAIMED/DONE). The worker, the join and the chunk hand-back each run only the jobs they claimed with a
//   compare-exchange OPEN->CLAIMED, so no job runs twice and none is written after the join returned: the join runs the unclaimed jobs itself (it never waits for a worker that
//   has not started them) and waits WITHOUT a timeout for the one the worker holds. A worker that holds a job for 200 ms is logged once, and from then on gets no new claim (FEPAR is
//   inline for the rest of the run). FEPAR is valid only in the thread record mode (the sources of the other modes are reused at once): elsewhere it is off, a line says why.
//   Core 3 (core3x15, MSC_VITA_CORE3_WORK bit 2, native/platform/vita/src/np_vita_core3.h): the host may place the worker on the Vita's system core (FeparPlacedOnCore3). There the worker parks at
//   once (no poll), and a join that waits more than 1500 us for a job the worker holds is "late"; 3 late joins in 48 retire the placement (the worker goes back to its plan core through the host's
//   repin hook, once, between two jobs; the pictures do not change either way: the claim protocol above is untouched). Host proof: MSC_VGX_FEPAR_FAKE3=1 (the worker counts itself as on core 3)
//   with MSC_VGX_FEPAR_BREAK=5 (every 4th claimed job takes 3 ms: the placement must be retired, the pictures must stay the same).
// The worker thread is placed by the host (SetFeparThreadInit: Vita core 1 beside the back end, never the game's core 0 nor the player's core 2).
// No allocation per draw: a ring of kSlots jobs made once; a full ring makes the draw inline.
namespace fepar {
constexpr uint32_t kSlots = 128, kMask = kSlots - 1;
struct Job {
    std::atomic<uint32_t> st{0}, runs{0};   // st: index << 2 | 0 open, 1 claimed, 2 done; runs: how many times RunJob ran it (must stay 1)
    Plan plan;
    ArrayRef arrs[26];
    const uint8_t* src = nullptr;
    uint8_t* out = nullptr;
    uint32_t count = 0, mask = 0, arenaOff = 0;
    bool soft = false, verify = false, mtx = false;
    uint32_t mxScan = 0, mxWorker = 0;
    float m0[12], n0[9];
    std::vector<uint8_t> shadow;   // (mode 2)
};
struct Counters {
    uint64_t rescues = 0, chunkWaits = 0, queued = 0, verts = 0, inlineSmall = 0, inlineNow = 0, inlineFull = 0, culled = 0, joins = 0, joinWaits = 0, joinWaitUs = 0, wakes = 0;
    uint64_t checked = 0, bad = 0, verifyFull = 0, joinRan = 0;
    uint64_t checkedBytes = 0, mtxQueued = 0, mtxVerts = 0, mtxChecked = 0, mtxBad = 0;   // (nxcpu25) bytes the verify compared; deferred draws with a matrix index; their maximum checked against the decode's
    uint64_t ilN[4] = {}, ilV[4] = {};   // stream draws the player decoded itself, by reason: 0 small, 1 skin, 2 matrix index, 3 other (verify / census / switch off)
};
struct Q {
    Job* jobs = nullptr;
    alignas(64) std::atomic<uint32_t> pub{0};
    alignas(64) std::atomic<uint32_t> done{0};
    alignas(64) std::atomic<bool> sleeping{false};
    alignas(64) std::atomic<bool> joinWaiting{false};
    std::atomic<bool> dead{false};
    std::atomic<uint64_t> busyUs{0}, workerJobs{0}, doubles{0};
    bool copySeen = false;
    Sema* wake = nullptr;
    Sema* joined = nullptr;
    uint32_t wakeMark = 0, verified = 0, jumpMark = 0, claims = 0;
    bool started = false, failed = false;
    pthread_t thread;
    Counters c;
    uint64_t lastBusy = 0, lastJobs = 0;
    Counters last;
};
Q g_q;
void (*g_threadInit)() = nullptr;
// (core3x15) the placement on the system core and the lateness rule (the same numbers as NpVitaCore3::LaneGuard: kSlowUs 1500, 3 in a window of 48 reported joins)
std::atomic<bool> g_onCore3{false}, g_repin{false};
void (*g_repinFn)() = nullptr;
bool (*g_replaceFn)() = nullptr;   // (core60) the host's way back onto the system core (true: placed again)
// (core60, MSC_VGX_FEPAR_KEEP, default 0 = off, bits) 1: on the system core the worker runs above the core watch's spell (priority, vgx_host_vita.cpp); 2: the cost rule below replaces "3 late joins in 48";
// 4: a retired worker is put back on the system core after MSC_VGX_FEPAR_KEEP_RETRY_S seconds (20; doubles each time, at most MSC_VGX_FEPAR_KEEP_TRIES = 4 returns). 7 = all.
inline int KeepBits() {
    static const int v = static_cast<int>(VGX_KNOB(static_cast<int32_t>(EnvInt("MSC_VGX_FEPAR_KEEP", 0)))) & 7;
    return v;
}
struct Late3 {
    static constexpr uint32_t kSlowUs = 1500;
    static constexpr int kLimit = 3, kWindow = 48;
    // the cost rule (KEEP bit 2): over kCostWindow joins that waited, retire when the mean wait is over meanLimitUs (the worker then costs the player more than the ~2 ms it saves a picture)
    // or kHardLimit of them waited over kHardUs (a stall of that size is a missed picture at 60). One wait counts at most kClampUs.
    static constexpr int kCostWindow = 128, kHardLimit = 12;
    static constexpr uint32_t kHardUs = 3000, kClampUs = 12000;
    int calls = 0, slows = 0, costCalls = 0, hard = 0, episodes = 0;
    uint64_t total = 0, worst = 0, costSum = 0, lates = 0, reports = 0;
    uint32_t meanLimitUs = 900;
    bool retired = false;
    std::atomic<bool> resume{false};   // the worker put itself back on the system core: the next report starts a fresh window
    const char* why = "";
    bool Report(uint32_t lateUs) {   // true when this report retires the placement
        if (resume.exchange(false, std::memory_order_acq_rel)) { retired = false; calls = slows = costCalls = hard = 0; costSum = 0; ++episodes; }
        if (retired) return false;
        ++calls; ++reports;
        if (lateUs > worst) worst = lateUs;
        if (lateUs > kSlowUs) { ++total; ++lates; }
        if (KeepBits() & 2) {
            ++costCalls;
            costSum += lateUs < kClampUs ? lateUs : kClampUs;
            if (lateUs > kHardUs) ++hard;
            if (hard >= kHardLimit) { retired = true; why = "hard stalls"; return true; }
            if (costSum > static_cast<uint64_t>(meanLimitUs) * kCostWindow) { retired = true; why = "mean wait"; return true; }
            if (costCalls >= kCostWindow) { costCalls = hard = 0; costSum = 0; }
            return false;
        }
        if (lateUs > kSlowUs) {
            ++slows;
            if (slows >= kLimit) { retired = true; why = "3 late joins in 48"; return true; }
        }
        if (calls >= kWindow) { calls = 0; slows = 0; }
        return false;
    }
};
Late3 g_late3;
std::atomic<uint32_t> g_retryAtUs{0};
std::atomic<bool> g_retryArmed{false};
struct Staged { bool on = false; const Plan* plan = nullptr; const uint8_t* src = nullptr; uint32_t count = 0; uint32_t mx = 0; bool mtx = false; };
Staged g_stage;

// (microseconds, 32 bits: the console's process clock wraps every 71 minutes; compare with Before(), subtract as unsigned)
inline uint32_t NowUs() {
#if defined(__vita__)
    return sceKernelGetProcessTimeLow();
#else
    timespec ts;
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);   // (the system call itself: the navigation harness replaces the libc clock for the game, and a read here must not move its time)
    return static_cast<uint32_t>(static_cast<uint64_t>(ts.tv_sec) * 1000000ull + static_cast<uint64_t>(ts.tv_nsec) / 1000u);
#endif
}
inline bool Before(uint32_t now, uint32_t t) { return static_cast<int32_t>(now - t) < 0; }
inline uint32_t Clamp(int32_t v, int32_t lo, int32_t hi) { return static_cast<uint32_t>(v < lo ? lo : v > hi ? hi : v); }
inline uint32_t KeepRetryS() { return Clamp(VGX_KNOB(static_cast<int32_t>(EnvInt("MSC_VGX_FEPAR_KEEP_RETRY_S", 20))), 1, 3600); }
inline int KeepTries() { return static_cast<int>(Clamp(VGX_KNOB(static_cast<int32_t>(EnvInt("MSC_VGX_FEPAR_KEEP_TRIES", 4))), 0, 16)); }
inline uint32_t MinVerts() { return Clamp(VGX_KNOB(static_cast<int32_t>(EnvInt("MSC_VGX_FEPAR_MIN", 8))), 1, 4096); }
inline uint32_t WakeEvery() { return Clamp(VGX_KNOB(static_cast<int32_t>(EnvInt("MSC_VGX_FEPAR_WAKE", 6))), 1, 128); }
inline uint32_t SpinUs() { if (g_onCore3.load(std::memory_order_relaxed)) return 0; return Clamp(VGX_KNOB(static_cast<int32_t>(EnvInt("MSC_VGX_FEPAR_SPIN_US", 30))), 0, 2000); }
inline int Break() {
#ifndef __VGX_VITA__
    return static_cast<int>(VGX_KNOB(static_cast<int32_t>(EnvInt("MSC_VGX_FEPAR_BREAK", 0))));
#else
    return 0;
#endif
}

void RunJob(Job& j) {
    if (j.runs.fetch_add(1, std::memory_order_relaxed) != 0) g_q.doubles.fetch_add(1, std::memory_order_relaxed);
    const VtxLayout L = MakeLayout(j.mask);
    if (j.verify) {
        j.shadow.assign(static_cast<size_t>(j.count) * L.stride, 0);
        uint32_t n = j.count;
        if (Break() == 2 && n) --n;
        DecodeVerticesSnap(j.plan, j.src, n, j.shadow.data(), j.arrs, &j.mxWorker);
        if (j.soft) SoftTransformMat(j.shadow.data(), n, L, j.m0, j.n0);
        return;
    }
    DecodeVerticesSnap(j.plan, j.src, j.count, j.out, j.arrs);
    if (j.soft) SoftTransformMat(j.out, j.count, L, j.m0, j.n0);
}

void* WorkerMain(void*) {
    if (g_threadInit) g_threadInit();
    if (KeepBits()) std::fprintf(stderr, "[fepar] MSC_VGX_FEPAR_KEEP=%d: decode worker %s; priority above the core watch's spell %s, cost rule %s (retire on a mean join wait over %u us or %d of %d joins over %u us), return to the system core %s (after %u s, doubling, %d tries)\n",
                                 KeepBits(), g_onCore3.load() ? "on the system core" : "not on the system core now", (KeepBits() & 1) ? "on" : "off", (KeepBits() & 2) ? "on" : "off (3 late joins in 48)", g_late3.meanLimitUs,
                                 Late3::kHardLimit, Late3::kCostWindow, Late3::kHardUs, (KeepBits() & 4) ? "on" : "off", KeepRetryS(), KeepTries());
#ifndef __VGX_VITA__
    if (Break() == 3) return nullptr;   // (mutant: a worker that is not there; the player's rescue after 200 ms must give the same pictures)
    if (EnvInt("MSC_VGX_FEPAR_FAKE3", 0)) g_onCore3.store(true);   // (the host's fake core 3)
#endif
    Q& q = g_q;
    uint32_t w = q.done.load(std::memory_order_relaxed);   // (the next job to try; a stale value is harmless: the claim word carries the index)
    for (;;) {
        if (g_repin.exchange(false, std::memory_order_acq_rel)) {   // (core3x15) the placement was retired: back to the plan's core, between two jobs
            g_onCore3.store(false, std::memory_order_release);
            if (g_repinFn) g_repinFn();
        }
        if (g_retryArmed.load(std::memory_order_acquire) && !g_repin.load(std::memory_order_acquire) && !Before(NowUs(), g_retryAtUs.load(std::memory_order_relaxed))) {   // (core60, KEEP bit 4) the retired worker's turn on the system core
            bool placed = false;
            if (g_replaceFn) placed = g_replaceFn();
#ifndef __VGX_VITA__
            else if (EnvInt("MSC_VGX_FEPAR_FAKE3", 0)) placed = true;   // (the host's fake core 3)
#endif
            if (placed) {
                g_retryArmed.store(false, std::memory_order_release);
                g_onCore3.store(true, std::memory_order_release);
                g_late3.resume.store(true, std::memory_order_release);
                std::fprintf(stderr, "[fepar] KEEP: the decode worker is back on the system core (return %d of %d, frame %llu)\n", g_late3.episodes + 1, KeepTries(), static_cast<unsigned long long>(g_frame));
            } else {   // (the system core is not open now: an online session, or not proven) look again in 10 s
                g_retryAtUs.store(NowUs() + 10u * 1000000u, std::memory_order_relaxed);
            }
        }
        for (;;) {
            if (q.dead.load(std::memory_order_acquire)) return nullptr;   // (given up: no new claims)
            if (w == q.pub.load(std::memory_order_acquire)) break;
            Job& j = q.jobs[w & kMask];
            uint32_t exp = w << 2;
            if (j.st.compare_exchange_strong(exp, (w << 2) | 1, std::memory_order_acq_rel)) {
                const uint32_t t0 = NowUs();
#ifndef __VGX_VITA__
                if (Break() == 1) { timespec ts{0, 500000}; nanosleep(&ts, nullptr); }
                if (Break() == 4 && (++q.claims & 63) == 5) { timespec ts{0, 300000000}; nanosleep(&ts, nullptr); }
                if (Break() == 5 && (++q.claims & 3) == 1) { timespec ts{0, 3000000}; nanosleep(&ts, nullptr); }
#endif
                RunJob(j);
                q.busyUs.fetch_add(NowUs() - t0, std::memory_order_relaxed);
                q.workerJobs.fetch_add(1, std::memory_order_relaxed);
                j.st.store((w << 2) | 2, std::memory_order_seq_cst);
                if (q.joinWaiting.load(std::memory_order_seq_cst)) q.joined->Post();
            }
            ++w;
        }
        const uint32_t until = NowUs() + SpinUs();
        bool more = false;
        while (Before(NowUs(), until)) {
            if (w != q.pub.load(std::memory_order_acquire)) { more = true; break; }
#if defined(__arm__)
            __asm__ __volatile__("yield" ::: "memory");
#endif
        }
        if (more) continue;
        q.sleeping.store(true, std::memory_order_seq_cst);
        if (w != q.pub.load(std::memory_order_seq_cst)) { q.sleeping.store(false, std::memory_order_seq_cst); continue; }
        q.wake->Wait(3600000);   // (idle: no "still waiting" line every two seconds)
        q.sleeping.store(false, std::memory_order_seq_cst);
    }
    return nullptr;
}

bool Start() {
    Q& q = g_q;
    if (q.started) return true;
    if (q.failed) return false;
    q.jobs = new (std::nothrow) Job[kSlots];
    q.wake = new (std::nothrow) Sema(0, "vgx fepar wake");
    q.joined = new (std::nothrow) Sema(0, "vgx fepar joined");
    if (!q.jobs || !q.wake || !q.joined) { q.failed = true; return false; }
    (void)DecodeDeferrable(Plan());   // (the decoder's switches are read here, on the player, before the worker needs them)
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    const int rc = pthread_create(&q.thread, &attr, WorkerMain, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) { std::fprintf(stderr, "[fepar] could not start the decode worker (error %d): decoding stays in the player\n", rc); q.failed = true; return false; }
    pthread_detach(q.thread);
    q.started = true;
    std::fprintf(stderr, "[fepar] MSC_VGX_FEPAR=%d: vertex decode of stream draws of %u vertices or more on a worker thread (wake every %u jobs, poll %u us; ring of %u jobs, %u KiB)\n", FeparMode(), MinVerts(), WakeEvery(), SpinUs(), kSlots, static_cast<unsigned>(sizeof(Job) * kSlots / 1024));
    return true;
}
inline void WakeWorker() {
    Q& q = g_q;
    if (q.sleeping.exchange(false, std::memory_order_seq_cst)) { q.wake->Post(); ++q.c.wakes; }
}
void VerifyDone() {
    Q& q = g_q;
    for (; q.verified != q.pub.load(std::memory_order_relaxed); ++q.verified) {
        Job& j = q.jobs[q.verified & kMask];
        if (!j.verify) continue;
        ++q.c.checked;
        const VtxLayout L = MakeLayout(j.mask);
        const size_t bytes = static_cast<size_t>(j.count) * L.stride;
        q.c.checkedBytes += bytes;
        if (j.mtx) {   // (the worker's own decode of the matrix-index column and its maximum against the player's scan)
            ++q.c.mtxChecked;
            if (j.mxWorker != j.mxScan) { ++q.c.mtxBad; ++q.c.bad; std::fprintf(stderr, "[fepar] VERIFY: frame %llu matrix-index maximum of %u vertices: the player's scan %u, the worker's decode %u\n", static_cast<unsigned long long>(g_frame), j.count, j.mxScan, j.mxWorker); }
        }
        const size_t off = j.arenaOff;
        if (j.shadow.size() != bytes || off + bytes > g_buf.vtx.size() || std::memcmp(j.shadow.data(), g_buf.vtx.data() + off, bytes) != 0) {
            ++q.c.bad;
            size_t at = 0;
            if (j.shadow.size() == bytes && off + bytes <= g_buf.vtx.size()) while (at < bytes && j.shadow[at] == g_buf.vtx[off + at]) ++at;
            std::fprintf(stderr, "[fepar] VERIFY: frame %llu draw of %u vertices (layout %x, soft %d) at arena offset %lu differs from the player's decode (first differing byte %lu of %lu)\n",
                         static_cast<unsigned long long>(g_frame), j.count, j.mask, j.soft ? 1 : 0, static_cast<unsigned long>(off), static_cast<unsigned long>(at), static_cast<unsigned long>(bytes));
        }
    }
}
// `done` (the player's: every job before it is finished) moves on over the jobs that are.
inline void Advance() {
    Q& q = g_q;
    uint32_t d = q.done.load(std::memory_order_relaxed);
    const uint32_t pub = q.pub.load(std::memory_order_relaxed);
    while (d != pub && q.jobs[d & kMask].st.load(std::memory_order_acquire) == ((d << 2) | 2)) ++d;
    q.done.store(d, std::memory_order_release);
}
// Every job before `target` is finished when this returns: the unclaimed ones the player runs itself (it never waits for a worker that has not begun them), the one the worker has
// claimed it waits for, without a timeout (a job in progress cannot be given up: it is writing). After 200 ms a line says so once and the worker gets no new claim.
void WaitDone(uint32_t target) {
    Q& q = g_q;
    const uint32_t t0 = NowUs();
    for (uint32_t i = q.done.load(std::memory_order_relaxed); static_cast<int32_t>(i - target) < 0; ++i) {
        Job& j = q.jobs[i & kMask];
        uint32_t exp = i << 2;
        if (j.st.compare_exchange_strong(exp, (i << 2) | 1, std::memory_order_acq_rel)) {
            RunJob(j);
            j.st.store((i << 2) | 2, std::memory_order_release);
            ++q.c.joinRan;
            continue;
        }
        const uint32_t doneWord = (i << 2) | 2;
        const uint32_t waitT0 = NowUs();
        const uint32_t spinUntil = waitT0 + 20;
        while (j.st.load(std::memory_order_acquire) != doneWord && Before(NowUs(), spinUntil)) {
#if defined(__arm__)
            __asm__ __volatile__("yield" ::: "memory");
#endif
        }
        uint32_t waited = 0;
        while (j.st.load(std::memory_order_seq_cst) != doneWord) {
            q.joinWaiting.store(true, std::memory_order_seq_cst);
            if (j.st.load(std::memory_order_seq_cst) == doneWord) { q.joinWaiting.store(false, std::memory_order_seq_cst); break; }
            const bool got = q.joined->WaitFor(200000);
            q.joinWaiting.store(false, std::memory_order_seq_cst);
            if (!got && j.st.load(std::memory_order_seq_cst) != doneWord) {
                waited += 200;
                if (!q.dead.exchange(true, std::memory_order_seq_cst)) {
                    ++q.c.rescues;
                    std::fprintf(stderr, "[fepar] the decode worker has held a job for %u ms (frame %llu): the player waits for it, takes every other job itself and MSC_VGX_FEPAR is inline for the rest of the run\n",
                                 waited, static_cast<unsigned long long>(g_frame));
                }
            }
        }
        if (g_onCore3.load(std::memory_order_relaxed) && g_late3.Report(NowUs() - waitT0)) {   // (core3x15) the player waited for a job the worker held
            g_repin.store(true, std::memory_order_release);
            if (KeepBits() & 2) {
                std::fprintf(stderr, "[fepar] KEEP: the decode worker on the system core is retired (%s; mean wait %.0f us over %d joins, longest %llu us, frame %llu)", g_late3.why,
                             g_late3.costCalls ? static_cast<double>(g_late3.costSum) / g_late3.costCalls : 0.0, g_late3.costCalls, static_cast<unsigned long long>(g_late3.worst), static_cast<unsigned long long>(g_frame));
            } else {
                std::fprintf(stderr, "[fepar] the decode worker on the system core kept the player waiting more than %u us on %d joins in a window of %d (longest %llu us, frame %llu): it goes back to its plan core",
                             Late3::kSlowUs, Late3::kLimit, Late3::kWindow, static_cast<unsigned long long>(g_late3.worst), static_cast<unsigned long long>(g_frame));
            }
            if ((KeepBits() & 4) && g_late3.episodes < KeepTries()) {
                const uint32_t afterS = KeepRetryS() << (g_late3.episodes < 3 ? g_late3.episodes : 3);
                g_retryAtUs.store(NowUs() + afterS * 1000000u, std::memory_order_relaxed);
                g_retryArmed.store(true, std::memory_order_release);
                std::fprintf(stderr, "; it is tried on the system core again in %u s (return %d of %d)\n", afterS, g_late3.episodes + 1, KeepTries());
            } else {
                std::fprintf(stderr, "%s\n", (KeepBits() & 4) ? "; the returns are used up: it stays on its plan core for the run" : "");
            }
        }
    }
    Advance();
    q.c.joinWaitUs += NowUs() - t0;
}
} // namespace fepar

int FeparMode() {
    int m = static_cast<int>(VGX_KNOB(static_cast<int32_t>(EnvInt("MSC_VGX_FEPAR", 0))));
    if (m < 0 || m > 2) m = 0;
    if (m && rec::g_mode != rec::MODE_THREAD) {   // (the other record modes hand the sources by reference and reuse them at once)
        static bool said = false;
        if (!said) { said = true; std::fprintf(stderr, "[fepar] MSC_VGX_FEPAR=%d is off: it needs the thread record mode (MSC_VGX_PLAY=thread), the record mode is %d\n", m, rec::g_mode); }
        m = 0;
    }
    return m;
}
void FeparNoteArrayCopy() { fepar::g_q.copySeen = true; }
void SetFeparThreadInit(void (*fn)()) { fepar::g_threadInit = fn; }
void SetFeparRepin(void (*fn)()) { fepar::g_repinFn = fn; }
void SetFeparReplace(bool (*fn)()) { fepar::g_replaceFn = fn; }
int FeparKeepBits() { return fepar::KeepBits(); }
void FeparPlacedOnCore3(bool on) { fepar::g_onCore3.store(on, std::memory_order_release); }

void FeparNoteInline(const Plan& p, uint32_t count, bool skin) {
    using namespace fepar;
    unsigned r = 3;
    if (skin) r = 1;
    else if (count < MinVerts()) r = 0;
    else if (!FeparMtxOn() && PlanHasMatrixIndex(p)) r = 2;
    ++g_q.c.ilN[r];
    g_q.c.ilV[r] += count;
}
bool FeparStage(const Plan& p, const uint8_t* src, uint32_t count) {
    using namespace fepar;
    if (count < MinVerts() || !DecodeDeferrable(p)) { ++g_q.c.inlineSmall; return false; }
    if (!Start() || g_q.dead.load(std::memory_order_relaxed)) return false;
    g_stage.on = true;
    g_stage.plan = &p;
    g_stage.src = src;
    g_stage.count = count;
    g_stage.mtx = PlanHasMatrixIndex(p);
    g_stage.mx = g_stage.mtx ? StreamMatrixMax(p, src, count) : 0;
    return true;
}

uint32_t FeparStagedMtxMax() { return fepar::g_stage.mx; }

void FeparJoin() {
    using namespace fepar;
    Q& q = g_q;
    if (!q.started) return;
    if (Break() == 1) return;
    const uint32_t pub = q.pub.load(std::memory_order_relaxed);
    Advance();
    if (q.done.load(std::memory_order_relaxed) != pub) {
        ++q.c.joinWaits;
        WaitDone(pub);
    }
    ++q.c.joins;
    if (FeparMode() == 2) VerifyDone();
    q.verified = pub;
}

// The player is about to hand back the record chunk before the one it has just finished: the stream sources of every job made while it played that chunk (all published
// before the previous jump) must be done. Jobs of the chunk just finished may still run: its memory is kept one jump longer.
void FeparChunkJump() {
    using namespace fepar;
    Q& q = g_q;
    if (!q.started) return;
    if (Break() == 1) return;
    const uint32_t mark = q.copySeen ? q.pub.load(std::memory_order_relaxed) : q.jumpMark;   // (an array copied into a chunk may be used by a job of the next one: wait for all)
    q.copySeen = false;
    Advance();
    if (static_cast<int32_t>(q.done.load(std::memory_order_relaxed) - mark) < 0) {
        ++q.c.chunkWaits;
        WaitDone(mark);
    }
    q.jumpMark = q.pub.load(std::memory_order_relaxed);
}

void FeparReport(FILE* f) {
    using namespace fepar;
    if (!FeparMode()) return;
    const Counters& c = g_q.c;
    std::fprintf(f, "front end decode worker (MSC_VGX_FEPAR=%d): %llu draws (%llu vertices) decoded by the worker, %llu left to the player (small or not plain), %llu made inline because a consumer needed the vertices, %llu because the ring was full, "
                    "%llu culled draws not decoded, %llu joins (%llu waited, %llu chunk hand-backs waited, %.2f ms in all), %llu wake-ups, %llu rescues, %llu jobs run by the join, %llu RUN TWICE, worker busy %.1f ms in %llu jobs; verify: %llu checked, %llu MISMATCHING, %llu made inline for a full verify ring\n",
                 FeparMode(), static_cast<unsigned long long>(c.queued), static_cast<unsigned long long>(c.verts), static_cast<unsigned long long>(c.inlineSmall), static_cast<unsigned long long>(c.inlineNow),
                 static_cast<unsigned long long>(c.inlineFull), static_cast<unsigned long long>(c.culled), static_cast<unsigned long long>(c.joins), static_cast<unsigned long long>(c.joinWaits), static_cast<unsigned long long>(c.chunkWaits),
                 c.joinWaitUs / 1000.0, static_cast<unsigned long long>(c.wakes), static_cast<unsigned long long>(c.rescues), static_cast<unsigned long long>(c.joinRan), static_cast<unsigned long long>(g_q.doubles.load()), g_q.busyUs.load() / 1000.0, static_cast<unsigned long long>(g_q.workerJobs.load()),
                 static_cast<unsigned long long>(c.checked), static_cast<unsigned long long>(c.bad), static_cast<unsigned long long>(c.verifyFull));
    std::fprintf(f, "front end decode worker, matrix-index draws (MSC_VGX_FEPAR_MTX=%d): %llu draws (%llu vertices) queued; verify: %llu bytes compared in all, %llu matrix-index maxima checked, %llu MISMATCHING\n",
                 FeparMtxOn() ? 1 : 0, static_cast<unsigned long long>(c.mtxQueued), static_cast<unsigned long long>(c.mtxVerts), static_cast<unsigned long long>(c.checkedBytes), static_cast<unsigned long long>(c.mtxChecked), static_cast<unsigned long long>(c.mtxBad));
    std::fprintf(f, "front end decode, left to the player (draws / vertices): small %llu / %llu, skinned %llu / %llu, matrix index %llu / %llu, other %llu / %llu\n",
                 static_cast<unsigned long long>(c.ilN[0]), static_cast<unsigned long long>(c.ilV[0]), static_cast<unsigned long long>(c.ilN[1]), static_cast<unsigned long long>(c.ilV[1]),
                 static_cast<unsigned long long>(c.ilN[2]), static_cast<unsigned long long>(c.ilV[2]), static_cast<unsigned long long>(c.ilN[3]), static_cast<unsigned long long>(c.ilV[3]));
}

// Once a picture, after the join: every 300 pictures one line (per picture: jobs, vertices, worker busy ms, join wait ms) and the totals.
void FeparPicture(uint64_t frame) {
    using namespace fepar;
    Q& q = g_q;
    if (!q.started || frame == 0 || frame % 300 != 0) return;
    const Counters& c = q.c;
    const uint64_t busy = q.busyUs.load(), jobs = q.workerJobs.load();
    const double n = 300.0;
    std::fprintf(stderr, "[fepar] 300 pictures to %llu: per picture %.1f jobs (%.1f with a matrix index), %.0f vertices, worker busy %.2f ms, join wait %.3f ms (%.1f of %.1f joins waited), %.1f inline (consumer) %.1f full %.1f culled; mismatches %llu, run twice %llu\n",
                 static_cast<unsigned long long>(frame), (jobs - q.lastJobs) / n, (c.mtxQueued - q.last.mtxQueued) / n, (c.verts - q.last.verts) / n, (busy - q.lastBusy) / 1000.0 / n, (c.joinWaitUs - q.last.joinWaitUs) / 1000.0 / n,
                 (c.joinWaits - q.last.joinWaits) / n, (c.joins - q.last.joins) / n, (c.inlineNow - q.last.inlineNow) / n, (c.inlineFull - q.last.inlineFull) / n, (c.culled - q.last.culled) / n,
                 static_cast<unsigned long long>(c.bad), static_cast<unsigned long long>(q.doubles.load()));
    if (KeepBits()) std::fprintf(stderr, "[fepar] KEEP=%d: worker on the %s, %llu joins waited to now: %llu over %u us, longest %llu us, %d return(s)\n", KeepBits(), g_onCore3.load() ? "system core" : "plan core",
                                 static_cast<unsigned long long>(g_late3.reports), static_cast<unsigned long long>(g_late3.lates), Late3::kSlowUs, static_cast<unsigned long long>(g_late3.worst), g_late3.episodes);
    q.last = c;
    q.lastBusy = busy;
    q.lastJobs = jobs;
}

namespace fepar {
// In EmitDraw: the draw DrawStream staged (if it did), taken once.
inline Staged Take() {
    Staged s = g_stage;
    g_stage.on = false;
    return s;
}
// The decode in the player, now (as DrawStream did it).
inline void RunNow(const Staged& s, uint8_t* out) {
    ++g_q.c.inlineNow;
    workload::FeScope feDecode(workload::FeDecode);
    DecodeVertices(*s.plan, s.src, s.count, out, nullptr, 0);
    if (s.mtx && FeparMode() == 2) {   // (verify: the scan EmitDraw's state was resolved with against the decode's own maximum)
        ++g_q.c.mtxChecked;
        if (LastDecodedPosMtxMax() != s.mx) { ++g_q.c.mtxBad; ++g_q.c.bad; std::fprintf(stderr, "[fepar] VERIFY: frame %llu matrix-index maximum of %u vertices: the scan %u, the player's decode %u\n", static_cast<unsigned long long>(g_frame), s.count, s.mx, LastDecodedPosMtxMax()); }
    }
}
// The draw is made and nothing reads its vertices before the join: hand it over. `inlineDone`: verify mode, the player has decoded it already; the worker's copy is compared at the join.
inline void Queue(const Staged& s, uint8_t* out, uint32_t layoutMask, bool soft, bool inlineDone) {
    Q& q = g_q;
    const uint32_t n = q.pub.load(std::memory_order_relaxed);
    Advance();
    const bool verifyFull = inlineDone && n - q.verified >= kSlots;   // (the verify walk reads the slots from `verified`: an older one must not be overwritten unread)
    if (n - q.done.load(std::memory_order_relaxed) >= kSlots || verifyFull || q.dead.load(std::memory_order_relaxed)) {
        if (verifyFull) ++q.c.verifyFull;
        ++q.c.inlineFull;
        if (!inlineDone) {
            const VtxLayout L = MakeLayout(layoutMask);
            { workload::FeScope feDecode(workload::FeDecode); DecodeVertices(*s.plan, s.src, s.count, out, nullptr, 0); }
            if (soft) { workload::FeScope feSoft(workload::FeSoft); SoftTransform(out, s.count, L); }
        }
        return;
    }
    Job& j = q.jobs[n & kMask];
    j.runs.store(0, std::memory_order_relaxed);
    j.plan = *s.plan;
    std::memcpy(j.arrs, g.arrays, sizeof j.arrs);
    j.src = s.src;
    j.out = out;
    j.count = s.count;
    j.mask = layoutMask;
    j.soft = soft;
    j.verify = inlineDone;
    j.mtx = s.mtx;
    j.mxScan = s.mx;
    if (s.mtx) { ++q.c.mtxQueued; q.c.mtxVerts += s.count; }
    j.arenaOff = static_cast<uint32_t>(out - g_buf.vtx.data());
    if (soft) {
        const uint32_t pi = g.matIdxA & 0x3F;
        std::memcpy(j.m0, g.xfPos[pi < 62 ? pi : 61], sizeof j.m0);
        std::memcpy(j.n0, g.xfNrm[(pi & 31) < 30 ? (pi & 31) : 29], sizeof j.n0);
    }
    j.st.store(n << 2, std::memory_order_release);   // (open)
    ++q.c.queued;
    q.c.verts += s.count;
    q.pub.store(n + 1, std::memory_order_seq_cst);
    if (n + 1 - q.wakeMark >= WakeEvery()) { q.wakeMark = n + 1; WakeWorker(); }
}
} // namespace fepar
