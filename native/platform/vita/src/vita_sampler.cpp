// diag13: MSC_SAMPLER=1 (default OFF), an on-console pc sampling profiler for the game thread (and, by name, other threads).
//
// A sampler thread wakes every MSC_SAMPLER_US microseconds (default 1000, at least 500) and, for one target thread per tick (round robin), reads sceKernelGetThreadInfo: a thread that is waiting is
// counted by (wait type, wait id) and not touched; a running or ready thread is suspended with sceKernelSuspendThreadForVM, its registers are read with sceKernelGetThreadContextForVM, and it is
// resumed at once (nothing else is done while it is stopped: no log, no allocation, no lock). The pc and lr words go into fixed tables (np_sampler_core.h: no allocation after start). Every
// MSC_SAMPLER_SECS (default 10) the window is written to the log: the top 40 pcs, the top 12 lr values (callers of leaf functions), the top waits, and what the sampler cost. The host tool
// tools/console-1.2/symbolize_samples.py maps the pcs to function names with the ELF.
//
// The layout of the raw context is undocumented. At start a thread that spins in a known function and holds a sentinel in lr is suspended once and its context is read; the word that points into
// the function is the pc, the word that holds the sentinel is lr, the buffer sizes are probed until the kernel accepts them. If the suspend is refused (the NFS port says a capability plugin may be
// needed) the sampler logs it and goes on with the waits only: no thread is ever suspended.
//
// Which threads: the game thread (the one that calls Start) always; MSC_SAMPLER_THREADS=<name part>[,<name part>...] adds up to 3 more threads whose name contains the text (the list of live threads
// with their names is logged at each window until every name has been found). Threads sampled round robin: with k targets each gets 1/k of the ticks. The sampler thread runs on cores 1-2 (never the
// system core), at a priority above the default.
//
// gxp17 (23 Nov): MSC_SAMPLER_BIN (default 1 once the sampler is on; 0 = the text log only) also keeps a histogram of (target, pc, lr) pairs and writes it at the end of every window to
// ux0:data/mscharged/sampler-<n>.bin (np_sampler_bin.h: the first free number, so a new run never overwrites an old one), with the layout, run / wait counts, the suspend hold times and the
// sampler thread's own CPU. The text log keeps only the 40 biggest pcs; the .bin keeps every pair, which is what lets tools/console-1.2/sampler_report.py give the run / wait split, the share of
// every subsystem (by pc, and counting the caller found in lr) and the top functions of the unstripped ELF. MSC_SAMPLER_TAG=<text> labels the run (dump-play, replay, walkout) in the header.
// The method (suspend / read context / resume, self-calibrating layout, wait types) is the one of the NFS Underground 2 Vita port's sampler.cpp (danos-x5/NFSUG2_VITA, MIT licence, see
// THIRD_PARTY_NOTICES.md); this file is written from the description of that method, not copied.
#include "vita_sampler.h"

#include "np_sampler_bin.h"
#include "np_sampler_core.h"

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
// Not in the VitaSDK headers. WEAK imports (NIDs 0x1FF5960D, 0x7EB55DAC, 0x22C9595E) taken from the SDK's _weak stub members as loose objects (Makefile.vpk NP_FORVM_OBJS): a firmware or
// setup without them still loads the eboot. Makefile.vpk (NP_FORVM_REQUIRE) pulls the members in, so the pointers are non-null in the image; an import that did not resolve at load time leaves a stub that
// returns without doing anything, which the calibration below catches (no layout found, waits only).
int sceKernelSuspendThreadForVM(SceUID thid) __attribute__((weak));
int sceKernelResumeThreadForVM(SceUID thid) __attribute__((weak));
int sceKernelGetThreadContextForVM(SceUID thid, void* cpuRegisters, void* vfpRegisters) __attribute__((weak));
// The anchor the host tool finds in the ELF to work out the load base (its runtime address is logged).
__attribute__((noinline, used)) void msc_sampler_anchor(void) { __asm__ volatile("" ::: "memory"); }
// The calibration thread's body: sets lr to a sentinel and spins; the suspended pc lies inside this function.
volatile uint32_t msc_sampler_spin_stop = 0;
__attribute__((noinline, used)) void msc_sampler_spin(void) {
    __asm__ volatile("movw lr, #0x00FE\n\tmovt lr, #0xC0DE\n1:\tldr r1, [%0]\n\tcmp r1, #0\n\tbeq 1b\n" :: "r"(&msc_sampler_spin_stop) : "r1", "lr", "cc", "memory");   // (ends when the stop word is set)
}
}

namespace {

constexpr uint32_t kLrSentinel = 0xC0DE00FEu;
constexpr int kMaxTargets = 4;
constexpr size_t kCtxWords = 128;
constexpr size_t kPcSlots = 4096, kLrSlots = 2048;
constexpr uint32_t kWaitSlots = 64;

struct Target {
    SceUID tid = 0;
    char name[32] = {};
    char want[24] = {};   // the name part this target was found by (empty: the game thread)
    uint32_t samples = 0, running = 0, waiting = 0, failed = 0, gone = 0;
    NpSampler::Table<kPcSlots> pc;
    NpSampler::Table<kLrSlots> lr;
};
Target g_t[kMaxTargets];
int g_nt = 0;
struct WaitRow { uint32_t type, id, n; } g_wait[kWaitSlots];   // linear table: waits are few kinds
uint32_t g_waitUsed = 0, g_waitLost = 0;

uint32_t g_cpuBytes = 0, g_vfpBytes = 0;
NpSampler::Layout g_layout;
bool g_canSuspend = false;
uint32_t g_intervalUs = 1000, g_windowSecs = 10;
SceUID g_self = 0;

// cost bookkeeping of the window
uint64_t g_stopSumUs = 0, g_stopMaxUs = 0, g_stopN = 0, g_ticks = 0, g_lateTicks = 0;

// the .bin dump (gxp17)
constexpr uint32_t kHistCap = 32768;   // entries (512 KiB), a power of two
NpSampler::Hist g_hist;
bool g_bin = false;
char g_tag[32] = {};
struct SlotHold { uint32_t us = 0, stops = 0, maxUs = 0, fails = 0; };
SlotHold g_hold[kMaxTargets];
uint32_t g_runSamples = 0, g_waitSamples = 0, g_failedReads = 0, g_binIndex = 0, g_textBase = 0, g_textSize = 0;
uint64_t g_firstStartUs = 0;

// The running module's code segment (the one holding this function), for the file header.
void FindText() {
    const uint32_t me = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&FindText));
    SceUID ids[64];
    SceSize n = 64;
    if (sceKernelGetModuleList(0xFF, ids, &n) < 0) return;
    for (SceSize i = 0; i < n; ++i) {
        SceKernelModuleInfo mi;
        std::memset(&mi, 0, sizeof mi);
        mi.size = sizeof mi;
        if (sceKernelGetModuleInfo(ids[i], &mi) < 0) continue;
        for (int sg = 0; sg < 4; ++sg) {
            const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mi.segments[sg].vaddr));
            if (mi.segments[sg].memsz && me - base < mi.segments[sg].memsz) { g_textBase = base; g_textSize = mi.segments[sg].memsz; return; }
        }
    }
}

// Writes sampler-<n>.bin from the histogram and the interval's counters, logs one line, and starts the next interval. samplerUs: the sampler thread's CPU time in the interval (0 = unknown).
void DumpBin(double secs, uint64_t sinceStartUs, uint32_t samplerUs) {
    NpSampler::BinHeader h;
    std::memset(&h, 0, sizeof h);
    std::memcpy(h.magic, "MSCSAMP1", 8);
    h.version = 1;
    h.periodUs = g_intervalUs;
    h.intervalMs = static_cast<uint32_t>(secs * 1000.0);
    h.windowMs = static_cast<uint32_t>(sinceStartUs / 1000);
    h.textBase = g_textBase; h.textSize = g_textSize;
    h.anchorAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&msc_sampler_anchor)) & ~1u;
    h.runSamples = g_runSamples; h.waitSamples = g_waitSamples; h.failed = g_failedReads;
    h.pcWord = static_cast<uint32_t>(g_layout.pc); h.lrWord = static_cast<uint32_t>(g_layout.lr);
    h.ticks = static_cast<uint32_t>(g_ticks); h.lateTicks = static_cast<uint32_t>(g_lateTicks); h.samplerUs = samplerUs;
    std::snprintf(h.tag, sizeof h.tag, "%s", g_tag);
    std::snprintf(h.anchorName, sizeof h.anchorName, "msc_sampler_anchor");
    for (int i = 0; i < g_nt && i < NpSampler::kBinSlots; ++i) {
        NpSampler::BinSlot& b = h.slots[i];
        b.used = 1; b.uid = static_cast<uint32_t>(g_t[i].tid);
        std::snprintf(b.name, sizeof b.name, "%s", g_t[i].name);
        b.stops = g_hold[i].stops; b.holdUs = g_hold[i].us; b.holdMaxUs = g_hold[i].maxUs; b.fails = g_hold[i].fails;
    }
    char path[64] = "";
    bool wrote = false;
    for (int tries = 0; tries < 1000 && !wrote; ++tries) {   // (the first free number: a new run does not overwrite the last one's files)
        std::snprintf(path, sizeof path, "ux0:data/mscharged/sampler-%u.bin", ++g_binIndex);
        if (FILE* t = std::fopen(path, "rb")) { std::fclose(t); continue; }
        h.index = g_binIndex;
        if (FILE* f = std::fopen(path, "wb")) { wrote = NpSampler::WriteBin(f, h, g_hist); wrote = (std::fclose(f) == 0) && wrote; }
        break;
    }
    std::fprintf(stderr, "[sampler] bin #%u: %s, %u ms, tag \"%s\", %u run + %u wait samples, %u distinct pairs, %u dropped, %u failed reads, text base 0x%08X anchor 0x%08X\n", h.index,
                 wrote ? path : "NOT WRITTEN", h.intervalMs, g_tag, g_runSamples, g_waitSamples, g_hist.Used(), g_hist.Dropped(), g_failedReads, g_textBase, h.anchorAddr);
    g_hist.Clear();
    for (int i = 0; i < kMaxTargets; ++i) g_hold[i] = SlotHold();
    g_runSamples = g_waitSamples = g_failedReads = 0;
}

bool GetContext(SceUID tid, uint32_t cpuBytes, uint32_t vfpBytes, uint32_t* cpu, uint32_t* vfp) {
    cpu[0] = cpuBytes;
    if (vfpBytes) vfp[0] = vfpBytes;
    return sceKernelGetThreadContextForVM(tid, cpu, vfpBytes ? vfp : nullptr) >= 0;
}

// Probe the buffer sizes the kernel accepts (vfp 268 bytes first, then none; cpu 4..512 bytes).
bool FindSizes(SceUID tid, uint32_t* cpu, uint32_t* vfp) {
    static const uint32_t vfpTry[2] = {268, 0};
    for (uint32_t v : vfpTry)
        for (uint32_t c = 4; c <= kCtxWords * 4; c += 4)
            if (GetContext(tid, c, v, cpu, vfp)) { g_cpuBytes = c; g_vfpBytes = v; return true; }
    return false;
}

int SpinMain(SceSize, void*) { msc_sampler_spin(); return 0; }

// Returns true when the layout is known and suspending works.
bool Calibrate() {
    if (!sceKernelSuspendThreadForVM || !sceKernelResumeThreadForVM || !sceKernelGetThreadContextForVM) {
        std::fprintf(stderr, "[sampler] kernel calls not available (weak import not linked): waits only\n");
        return false;
    }
    const SceUID spin = sceKernelCreateThread("np_sampler_cal", SpinMain, 0x10000100 + 31,   // (samp18: +60 = 220 is past the least urgent user priority 191: 0x80028023 on the console)
                                               16 * 1024, 0, 0x60000, nullptr);
    if (spin < 0 || sceKernelStartThread(spin, 0, nullptr) < 0) { std::fprintf(stderr, "[sampler] the calibration thread did not start (0x%08X): waits only\n", static_cast<unsigned>(spin)); return false; }
    sceKernelDelayThread(10 * 1000);
    bool ok = false;
    const int s = sceKernelSuspendThreadForVM(spin);
    if (s < 0) {
        std::fprintf(stderr, "[sampler] sceKernelSuspendThreadForVM refused (0x%08X; a capability plugin may be needed): waits only\n", static_cast<unsigned>(s));
    } else {
        static uint32_t cpu[kCtxWords], vfp[kCtxWords * 4];
        std::memset(cpu, 0, sizeof cpu);
        if (!FindSizes(spin, cpu, vfp)) {
            std::fprintf(stderr, "[sampler] sceKernelGetThreadContextForVM accepted no buffer size: waits only\n");
        } else {
            const uintptr_t lo = reinterpret_cast<uintptr_t>(&msc_sampler_spin) & ~static_cast<uintptr_t>(1);
            g_layout = NpSampler::FindLayout(cpu, kCtxWords, lo, lo + 64, kLrSentinel);
            if (g_layout.Ok()) {
                ok = true;
                std::fprintf(stderr, "[sampler] context layout: pc word %d, lr word %d (cpu buffer %u bytes, vfp %u bytes)\n", g_layout.pc, g_layout.lr, g_cpuBytes, g_vfpBytes);
            } else {
                std::fprintf(stderr, "[sampler] context layout not found (cpu %u bytes, vfp %u bytes); first words:", g_cpuBytes, g_vfpBytes);
                for (int i = 0; i < 24; ++i) std::fprintf(stderr, " %08X", static_cast<unsigned>(cpu[i]));
                std::fprintf(stderr, "\n[sampler] kernel calls not available or the layout is unknown (an unresolved weak import returns without writing): waits only\n");
            }
        }
        sceKernelResumeThreadForVM(spin);
    }
    msc_sampler_spin_stop = 1;
    int exitStatus = 0;
    sceKernelWaitThreadEnd(spin, &exitStatus, nullptr);
    sceKernelDeleteThread(spin);
    return ok;
}

void AddWait(uint32_t type, uint32_t id) {
    for (uint32_t i = 0; i < g_waitUsed; ++i) if (g_wait[i].type == type && g_wait[i].id == id) { ++g_wait[i].n; return; }
    if (g_waitUsed < kWaitSlots) g_wait[g_waitUsed++] = {type, id, 1};
    else ++g_waitLost;
}

// Find live threads whose name holds the wanted text and that are not targets yet.
void ScanNames(const char* names) {
    if (!names || !*names) return;
    char list[96];
    std::snprintf(list, sizeof list, "%s", names);
    char* save = nullptr;
    for (char* part = strtok_r(list, ",", &save); part && g_nt < kMaxTargets; part = strtok_r(nullptr, ",", &save)) {
        const bool multi = !std::strcmp(part, "@core1") || !std::strcmp(part, "@core2") || !std::strcmp(part, "@core3");
        if (!multi) {
            bool have = false;
            for (int i = 1; i < g_nt; ++i) if (std::strcmp(g_t[i].want, part) == 0) have = true;
            if (have) continue;
        }
        int misses = 0;
        for (SceUID uid = 0x40010001; uid < 0x40020000 && misses < 500 && g_nt < kMaxTargets; ++uid) {
            if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++misses; continue; }
            SceKernelThreadInfo info;
            std::memset(&info, 0, sizeof info);
            info.size = sizeof info;
            if (sceKernelGetThreadInfo(uid, &info) != 0) { ++misses; continue; }
            misses = 0;
            if (uid == g_self) continue;
            bool hit = false;
            if (!std::strcmp(part, "@render")) hit = !std::strcmp(info.name, "pthread") && info.currentCpuAffinityMask == 0x20000 && info.stackSize >= 2 * 1024 * 1024;
            else if (!std::strcmp(part, "@send")) hit = !std::strcmp(info.name, "pthread") && info.currentCpuAffinityMask == 0x20000 && info.stackSize == 256 * 1024 && info.currentPriority == 144;
            else if (!std::strcmp(part, "@play")) hit = !std::strcmp(info.name, "pthread") && info.currentCpuAffinityMask == 0x40000 && info.stackSize >= 1024 * 1024;
            else if (!std::strcmp(part, "@core1")) hit = info.currentCpuAffinityMask == 0x20000;
            else if (!std::strcmp(part, "@core2")) hit = info.currentCpuAffinityMask == 0x40000;
            else if (!std::strcmp(part, "@core3")) hit = info.currentCpuAffinityMask == 0x80000;
            else hit = std::strstr(info.name, part) != nullptr;
            if (!hit) continue;   // (g_self: the sampler thread itself, once started)
            bool dup = false;
            for (int i = 0; i < g_nt; ++i) if (g_t[i].tid == uid) dup = true;
            if (dup) continue;
            Target& t = g_t[g_nt++];
            t.tid = uid;
            if (multi) std::snprintf(t.name, sizeof t.name, "%s:%x", info.name, static_cast<unsigned>(uid) & 0xFFFu);
            else std::snprintf(t.name, sizeof t.name, "%s", part[0] == '@' ? part + 1 : info.name);
            std::snprintf(t.want, sizeof t.want, "%s", part);
            std::fprintf(stderr, "[sampler] target %d: thread \"%s\" (kernel \"%s\", uid 0x%X, affinity 0x%X, priority %d, stack %u KiB) found by \"%s\"\n", g_nt - 1, t.name, info.name, static_cast<unsigned>(uid),
                         static_cast<unsigned>(info.currentCpuAffinityMask), info.currentPriority, static_cast<unsigned>(info.stackSize / 1024), part);
            if (!multi) break;
        }
    }
}

void PrintWindow(unsigned n, double secs) {
    for (int ti = 0; ti < g_nt; ++ti) {
        Target& t = g_t[ti];
        std::fprintf(stderr, "[sampler] window %u target %d \"%s\" %.1f s: %u samples (%u running, %u waiting, %u context reads failed, %u thread gone), %u distinct pcs, %u not tabled\n",
                     n, ti, t.name, secs, t.samples, t.running, t.waiting, t.failed, t.gone, t.pc.used, t.pc.overflow);
        size_t idx[40];
        const double run = t.pc.total ? static_cast<double>(t.pc.total) : 1.0;
        size_t got = t.pc.Top(idx, 40);
        for (size_t i = 0; i < got; ++i) std::fprintf(stderr, "[sampler]   pc 0x%08X %u %.2f\n", static_cast<unsigned>(t.pc.key[idx[i]]), static_cast<unsigned>(t.pc.cnt[idx[i]]), 100.0 * t.pc.cnt[idx[i]] / run);
        got = t.lr.Top(idx, 12);
        for (size_t i = 0; i < got; ++i) std::fprintf(stderr, "[sampler]   lr 0x%08X %u %.2f\n", static_cast<unsigned>(t.lr.key[idx[i]]), static_cast<unsigned>(t.lr.cnt[idx[i]]), 100.0 * t.lr.cnt[idx[i]] / run);
        t.samples = t.running = t.waiting = t.failed = t.gone = 0;
        t.pc.Reset();
        t.lr.Reset();
    }
    {   // waits, all targets together: the 8 most counted (type: SCE_KERNEL_TMID_* of the object waited on, id: its uid)
        for (int k = 0; k < 8; ++k) {
            uint32_t best = 0, at = 0;
            for (uint32_t i = 0; i < g_waitUsed; ++i) if (g_wait[i].n > best) { best = g_wait[i].n; at = i; }
            if (!best) break;
            std::fprintf(stderr, "[sampler]   wait type 0x%X id 0x%X %u\n", static_cast<unsigned>(g_wait[at].type), static_cast<unsigned>(g_wait[at].id), static_cast<unsigned>(best));
            g_wait[at].n = 0;
        }
        if (g_waitLost) std::fprintf(stderr, "[sampler]   wait table full: %u waits not tabled\n", g_waitLost);
        g_waitUsed = g_waitLost = 0;
    }
}

int SamplerMain(SceSize, void*) {
    static uint32_t cpu[kCtxWords], vfp[kCtxWords * 4];
    const char* names = std::getenv("MSC_SAMPLER_THREADS");
    uint64_t winStart = sceKernelGetSystemTimeWide();
    uint64_t nextTick = winStart;
    unsigned window = 0, rr = 0;
    uint64_t lastRun = 0;
    for (;;) {
        sceKernelDelayThread(g_intervalUs);
        const uint64_t now = sceKernelGetSystemTimeWide();
        ++g_ticks;
        if (now > nextTick + g_intervalUs * 3) ++g_lateTicks;   // (the wake-up came 3 intervals late: the sampler thread itself was kept from running)
        nextTick = now;
        Target& t = g_t[rr];
        rr = (rr + 1) % static_cast<unsigned>(g_nt);
        SceKernelThreadInfo info;
        std::memset(&info, 0, sizeof info);
        info.size = sizeof info;
        const bool haveInfo = sceKernelGetThreadInfo(t.tid, &info) >= 0;
        ++t.samples;
        if (haveInfo && (info.status & ~static_cast<uint32_t>(SCE_THREAD_RUNNING | SCE_THREAD_READY))) {
            ++t.waiting;
            AddWait(info.waitType, static_cast<uint32_t>(info.waitId));
            if (g_bin) { g_hist.Add(static_cast<uint32_t>(info.waitId), 0, NpSampler::MakeMeta(static_cast<uint32_t>(&t - g_t), NpSampler::kKindWait, info.waitType)); ++g_waitSamples; }
        } else if (g_canSuspend) {
            const uint64_t t0 = sceKernelGetSystemTimeWide();
            const int sr = sceKernelSuspendThreadForVM(t.tid);
            if (sr < 0) {
                ++t.gone;
            } else {
                const bool got = GetContext(t.tid, g_cpuBytes, g_vfpBytes, cpu, vfp);
                sceKernelResumeThreadForVM(t.tid);
                const uint64_t stop = sceKernelGetSystemTimeWide() - t0;   // (suspend to resume: what the target lost)
                g_stopSumUs += stop; ++g_stopN;
                if (stop > g_stopMaxUs) g_stopMaxUs = stop;
                SlotHold& hd = g_hold[&t - g_t];
                hd.us += static_cast<uint32_t>(stop); ++hd.stops;
                if (stop > hd.maxUs) hd.maxUs = static_cast<uint32_t>(stop);
                if (got) {
                    ++t.running; t.pc.Add(cpu[g_layout.pc] & ~1u); t.lr.Add(cpu[g_layout.lr] & ~1u);
                    if (g_bin) { g_hist.Add(cpu[g_layout.pc] & ~1u, cpu[g_layout.lr] & ~1u, NpSampler::MakeMeta(static_cast<uint32_t>(&t - g_t), NpSampler::kKindRun, 0)); ++g_runSamples; }
                } else { ++t.failed; ++hd.fails; ++g_failedReads; }
            }
        } else {
            ++t.running;   // (waits-only mode: a running thread is just counted)
            if (g_bin) { g_hist.Add(0, 0, NpSampler::MakeMeta(static_cast<uint32_t>(&t - g_t), NpSampler::kKindRun, 0)); ++g_runSamples; }
        }
        if (now - winStart >= static_cast<uint64_t>(g_windowSecs) * 1000000u) {
            const double secs = static_cast<double>(now - winStart) * 1e-6;
            ++window;
            ScanNames(names);
            PrintWindow(window, secs);
            SceKernelThreadInfo me;
            std::memset(&me, 0, sizeof me);
            me.size = sizeof me;
            double ownPct = -1.0;
            if (sceKernelGetThreadInfo(g_self, &me) >= 0) { const uint64_t r = me.runClocks; ownPct = static_cast<double>(r - lastRun) / (secs * 1e6) * 100.0; lastRun = r; }
            if (g_bin) DumpBin(secs, now - g_firstStartUs, ownPct >= 0.0 ? static_cast<uint32_t>(ownPct * secs * 1e4) : 0);
            std::fprintf(stderr, "[sampler] cost: %llu ticks (%.0f a second; %llu came 3 intervals late), %llu suspends, target stopped %.1f us on average (longest %llu us) = %.2f %% of one target's time over the window, sampler thread %.2f %% of a core (ESTIMATE from its own clocks)\n",
                         static_cast<unsigned long long>(g_ticks), g_ticks / secs, static_cast<unsigned long long>(g_lateTicks), static_cast<unsigned long long>(g_stopN),
                         g_stopN ? static_cast<double>(g_stopSumUs) / static_cast<double>(g_stopN) : 0.0, static_cast<unsigned long long>(g_stopMaxUs),
                         100.0 * static_cast<double>(g_stopSumUs) / (secs * 1e6) * (g_nt > 0 ? g_nt : 1), ownPct);
            g_stopSumUs = g_stopMaxUs = g_stopN = g_ticks = g_lateTicks = 0;
            winStart = now;
        }
    }
    return 0;
}

}  // namespace

namespace VitaSampler {

void Start() {
    const char* e = std::getenv("MSC_SAMPLER");
    if (!e || std::atoi(e) != 1) return;
    if (const char* u = std::getenv("MSC_SAMPLER_US")) { const long v = std::atol(u); if (v >= 500 && v <= 100000) g_intervalUs = static_cast<uint32_t>(v); }
    if (const char* s = std::getenv("MSC_SAMPLER_SECS")) { const long v = std::atol(s); if (v >= 1 && v <= 600) g_windowSecs = static_cast<uint32_t>(v); }
    if (const char* tg = std::getenv("MSC_SAMPLER_TAG")) std::snprintf(g_tag, sizeof g_tag, "%s", tg);
    const char* bin = std::getenv("MSC_SAMPLER_BIN");
    if (!bin || std::atoi(bin) != 0) {
        static NpSampler::BinEntry* store = static_cast<NpSampler::BinEntry*>(std::malloc(sizeof(NpSampler::BinEntry) * kHistCap));
        if (store) { g_hist.Init(store, kHistCap); g_bin = true; FindText(); }
        else std::fprintf(stderr, "[sampler] no memory for the .bin histogram (%u KiB): the text log only\n", static_cast<unsigned>(sizeof(NpSampler::BinEntry) * kHistCap / 1024));
    }
    g_firstStartUs = sceKernelGetSystemTimeWide();
    const SceUID game = sceKernelGetThreadId();
    g_t[0].tid = game;
    std::snprintf(g_t[0].name, sizeof g_t[0].name, "game");
    g_nt = 1;
    g_canSuspend = Calibrate();
    std::fprintf(stderr, "[sampler] on (MSC_SAMPLER=1): game thread uid 0x%X, every %u us, window %u s, %s, .bin dumps %s (tag \"%s\"); anchor msc_sampler_anchor at 0x%08X (the host tool reads the load base from it)\n",
                 static_cast<unsigned>(game), g_intervalUs, g_windowSecs, g_canSuspend ? "pc and lr sampled" : "waits only", g_bin ? "on" : "off", g_tag,
                 static_cast<unsigned>(reinterpret_cast<uintptr_t>(&msc_sampler_anchor) & ~static_cast<uintptr_t>(1)));
    ScanNames(std::getenv("MSC_SAMPLER_THREADS"));
    const SceUID th = sceKernelCreateThread("np_sampler", SamplerMain, 0x10000100 - 20, 16 * 1024, 0, 0x60000, nullptr);   // cores 1-2, never the system core
    g_self = th;   // (the sampler thread's own id: never a target, and its run clocks give the cost line)
    if (th < 0 || sceKernelStartThread(th, 0, nullptr) < 0) { std::fprintf(stderr, "[sampler] the sampler thread did not start (0x%08X)\n", static_cast<unsigned>(th)); return; }
}

}  // namespace VitaSampler
