// The console output thread of np_vita_audio.cpp on the host (STR-133): the Vita is simulated by fakes of sceAudioOutOutput and of the thread calls,
// the code under test is the real np_vita_audio.cpp (included below with __vita__ defined).
//
// The device reads a buffer after sceAudioOutOutput returned, until the call that follows the NEXT one has returned, so the thread must not
// rewrite a handed-over buffer for two more periods: three buffers in turn (17 Oct, a single buffer rewritten while it played was heard as a
// buzz at the period rate). The checks: (1) the thread rotates over exactly three buffers, in order; (2) a handed-over buffer has the content it
// had at the call at each of the next two calls; (3) the same through pause and resume (silence is written into the rotation, not into the
// handed-over buffer); (4) the priority MSC_AUDIO_PRIO asked is what the thread is created with and the default is unchanged.
#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <vector>

#define __vita__ 1
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

static int g_failed = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- the fakes ------------------------------------------------------------------------------------------------------------------
static pthread_t g_pt;
static SceKernelThreadEntry g_entry;
static int g_createdPriority = -1;
static int g_failPriority = -1;   // a priority the fake kernel refuses (-1: none)
static int g_createCalls = 0;
extern "C" SceUID sceKernelCreateThread(const char*, SceKernelThreadEntry entry, int prio, SceSize, SceUInt, int, const SceKernelThreadOptParam*) {
    ++g_createCalls;
    if (prio == g_failPriority) return -1;
    g_entry = entry;
    g_createdPriority = prio;
    return 1;
}
static void* Run(void*) { g_entry(0, nullptr); return nullptr; }
extern "C" int sceKernelStartThread(SceUID, SceSize, void*) { return pthread_create(&g_pt, nullptr, Run, nullptr); }
extern "C" int sceKernelWaitThreadEnd(SceUID, int*, SceUInt*) { pthread_join(g_pt, nullptr); return 0; }
extern "C" int sceKernelExitDeleteThread(int) { return 0; }
extern "C" int sceKernelDelayThread(SceUInt us) { usleep(us); return 0; }


struct Call { const void* buf; uint64_t hash; std::vector<unsigned char> bytes; };
static std::vector<Call> g_calls;
static std::atomic<size_t> g_n{0};   // calls made (g_calls is read by the main thread only after the device thread was stopped)
static bool g_testBroken = false;   // set by the checks: a handed-over buffer changed under the device
static uint64_t Hash(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
// 1.2 (suspend / resume): each open hands out a new port number; a call on a port that is not the current one fails, as it does on the console after the
// system took the port away; g_failNext makes that many calls fail whatever the port; the volume calls are counted per port.
static std::atomic<int> g_currentPort{0}, g_opens{0}, g_releases{0}, g_failNext{0}, g_failedCalls{0}, g_volumeCalls{0}, g_openFailures{0};
static std::atomic<int> g_lastGoodPort{-1};
extern "C" int sceAudioOutOpenPort(int, int, int, int) {
    if (g_openFailures.load() > 0) { --g_openFailures; return static_cast<int>(0x80260005); }
    g_opens.fetch_add(1);
    g_currentPort.store(7 + g_opens.load());
    return g_currentPort.load();
}
extern "C" int sceAudioOutReleasePort(int) { g_releases.fetch_add(1); return 0; }
extern "C" int sceAudioOutSetVolume(int port, SceAudioOutChannelFlag, int*) { g_volumeCalls.fetch_add(1); return port == g_currentPort.load() ? 0 : static_cast<int>(0x80260001); }
extern "C" int sceAudioOutOutput(int port, const void* buf) {
    if (g_failNext.load() > 0) { --g_failNext; g_failedCalls.fetch_add(1); return static_cast<int>(0x80260001); }
    if (g_currentPort.load() != 0 && port != g_currentPort.load()) { g_failedCalls.fetch_add(1); return static_cast<int>(0x80260001); }
    g_lastGoodPort.store(port);
    // (the device and the audio thread are one thread here: the checks run in the call, between two periods)
    const size_t bytes = 512 * 4;
    const size_t n = g_calls.size();
    for (size_t back = 1; back <= 2 && back <= n; ++back) {
        const Call& c = g_calls[n - back];
        if (Hash(c.buf, bytes) != c.hash) {
            g_testBroken = true;
            std::fprintf(stderr, "call %zu: the buffer handed over %zu call(s) ago was rewritten under the device\n", n, back);
        }
    }
    Call c;
    c.buf = buf;
    c.hash = Hash(buf, bytes);
    g_calls.push_back(c);
    g_n.store(g_calls.size());
    usleep(300);   // a period is 16 ms on the console; the order of events is what matters here
    return 0;
}

// the code under test
#include "../src/np_vita_audio.cpp"

namespace VitaThreads { int CoreMask(Role) { return 0; } int PlainMask(Role) { return 0; } NpVitaCore3::Rules CurrentRulesNow() { return NpVitaCore3::Rules(); } }   // (the audio thread asks for the core-3 rules: all off)   // (VitaMem::RegisterThread is weak: absent here; 1.3 added PlainMask: the test no longer linked)

static void Wait(size_t calls) {
    for (int i = 0; i < 20000 && g_n.load() < calls; ++i) usleep(200);
}

int main() {
    setenv("MSC_AUDIO_QUEUE", "fixed", 1);
    unsetenv("MSC_AUDIO_PRIO");
    CHECK(NpAudioOpen(32000) == 1);
    CHECK(g_createdPriority == 0x10000100 - 32);   // the default priority is unchanged by 1.2
    // some sound: a ramp, pushed in blocks as the mixer does
    std::vector<int16_t> block(2 * 96);
    int phase = 1;
    auto feed = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<int16_t>((phase++ * 37) & 0x7FFF);
            NpAudioPushLE16(block.data(), 96);
        }
    };
    feed(300);
    Wait(40);
    // pause (Settings, movies): silence into the rotation, then resume
    CHECK(VitaAudio::Pause(true));
    size_t atPause = g_n.load();
    Wait(atPause + 8);
    CHECK(VitaAudio::Pause(false));
    feed(300);
    Wait(atPause + 8 + 40);
    NpAudioSetMuted(1);
    Wait(g_n.load() + 6);
    NpAudioSetMuted(0);
    feed(300);
    Wait(g_n.load() + 20);
    StopDevice();
    CHECK(!g_testBroken);

    // (1) exactly three buffers, in a fixed rotation
    std::vector<const void*> distinct;
    for (const Call& c : g_calls) {
        bool seen = false;
        for (const void* d : distinct) seen |= d == c.buf;
        if (!seen) distinct.push_back(c.buf);
    }
    CHECK(g_calls.size() >= 90);
    CHECK(distinct.size() == 3);
    bool rotates = g_calls.size() > 3;
    for (size_t i = 3; i < g_calls.size(); ++i) rotates &= g_calls[i].buf == g_calls[i - 3].buf && g_calls[i].buf != g_calls[i - 1].buf && g_calls[i].buf != g_calls[i - 2].buf;
    CHECK(rotates);
    // the sound reached the device and so did the silence of the pause (the test would pass an empty stream too)
    size_t silent = 0, loud = 0;
    static const unsigned char zeros[512 * 4] = {};
    const uint64_t zeroHash = Hash(zeros, sizeof zeros);
    for (const Call& c : g_calls) (c.hash == zeroHash ? silent : loud)++;
    CHECK(loud >= 10 && silent >= 8);
    std::printf("%zu periods handed over, %zu loud, %zu silent, %zu distinct buffers\n", g_calls.size(), loud, silent, distinct.size());

    // (4) the priority switch
    g_calls.clear();
    g_n.store(0);
    setenv("MSC_AUDIO_PRIO", "268435712", 1);   // 0x10000100
    NpAudioOpen(48000);   // another rate re-opens the device
    CHECK(g_createdPriority == 268435712);
    Wait(6);
    StopDevice();

    // (5) a value the kernel cannot take, or a thread that cannot be created with it, falls back to the default once
    struct PrioCase { const char* env; int expect; int failPrio; int creates; };
    const PrioCase cases[] = {
        {"112", 112, -1, 1},                       // absolute, in range: kept
        {"0x100000D0", 0x100000D0, -1, 1},         // relative (0x10000100 - 48): kept
        {"300", 0x10000100 - 32, -1, 1},           // above 191: default, no attempt with the bad value
        {"0", 0x10000100 - 32, -1, 1},             // below 64
        {"0x10000200", 0x10000100 - 32, -1, 1},    // relative offset +256: out of range
        {"abc", 0x10000100 - 32, -1, 1},           // not a number
        {"100", 0x10000100 - 32, 100, 2},          // in range but the kernel refuses it: one retry with the default
    };
    uint32_t rate = 20000;
    for (const PrioCase& c : cases) {
        setenv("MSC_AUDIO_PRIO", c.env, 1);
        g_failPriority = c.failPrio;
        g_createCalls = 0;
        g_createdPriority = -1;
        CHECK(NpAudioOpen(rate++) == 1);   // (another rate each time re-opens the device)
        if (g_createdPriority != c.expect || g_createCalls != c.creates)
            std::fprintf(stderr, "case MSC_AUDIO_PRIO=%s: created with %d (%d calls), wanted %d (%d calls)\n", c.env, g_createdPriority, g_createCalls, c.expect, c.creates);
        CHECK(g_createdPriority == c.expect);
        CHECK(g_createCalls == c.creates);
    }
    StopDevice();
    g_failPriority = -1;

    // (6) 1.2, music after the console slept (MSC_AUDIO_RESUME, default on): the system takes the port away, or says the console woke up. The thread opens the
    // port again, sets the volume again and the sound goes on; until then it drops periods at the real-time pace, so the mixer is not left racing ahead.
    // (The off switch cannot be tried in this process: the thread reads it once.)
    unsetenv("MSC_AUDIO_PRIO");
    CHECK(NpAudioOpen(32000) == 1);
    g_calls.clear();
    g_n.store(0);
    auto waitFor = [](auto pred, int ms) { for (int i = 0; i < ms && !pred(); ++i) usleep(1000); return pred(); };
    feed(200);
    CHECK(waitFor([] { return g_n.load() >= 10; }, 3000));
    const uint32_t reopens0 = VitaAudio::Reopens();
    const int volume0 = g_volumeCalls.load(), failed0 = g_failedCalls.load();
    g_currentPort.store(-5);                                    // the port number the thread holds is no longer valid
    CHECK(waitFor([] { return VitaAudio::Reopens() >= 1; }, 3000));
    size_t atRecovery = g_n.load();
    CHECK(waitFor([&] { return g_n.load() >= atRecovery + 10; }, 3000));
    feed(200);
    CHECK(VitaAudio::Reopens() == reopens0 + 1);
    CHECK(g_failedCalls.load() - failed0 >= 3 && g_failedCalls.load() - failed0 < 10);   // three bad periods, then the repair
    CHECK(g_lastGoodPort.load() == g_currentPort.load());       // the sound goes to the new port
    CHECK(g_volumeCalls.load() > volume0);                      // the volume was set on it again
    // the console woke up: one repair, though the system may say so twice at once
    const uint32_t reopens1 = VitaAudio::Reopens();
    const int failed1 = g_failedCalls.load();
    const int portBefore = g_currentPort.load();
    VitaAudio::NotifyResume();
    VitaAudio::NotifyResume();
    CHECK(waitFor([&] { return VitaAudio::Reopens() >= reopens1 + 1; }, 3000));
    usleep(150 * 1000);
    CHECK(VitaAudio::Reopens() == reopens1 + 1);
    CHECK(g_currentPort.load() != portBefore);
    atRecovery = g_n.load();
    feed(200);
    CHECK(waitFor([&] { return g_n.load() >= atRecovery + 10; }, 3000));
    CHECK(g_failedCalls.load() == failed1);                     // a wake-up repair needs no failed call
    CHECK(g_lastGoodPort.load() == g_currentPort.load());
    // the port cannot be opened at the first try: the thread keeps its pace and tries again
    const uint32_t reopens2 = VitaAudio::Reopens();
    g_openFailures.store(1);
    g_currentPort.store(-5);
    CHECK(waitFor([&] { return VitaAudio::Reopens() >= reopens2 + 2; }, 6000));
    atRecovery = g_n.load();
    feed(200);
    CHECK(waitFor([&] { return g_n.load() >= atRecovery + 10; }, 3000));
    CHECK(g_lastGoodPort.load() == g_currentPort.load() && g_openFailures.load() == 0);
    // the sound reached the new port: loud periods in the tail of the run
    size_t loudAfter = 0;
    const size_t total = g_n.load();
    StopDevice();
    for (size_t i = total > 40 ? total - 40 : 0; i < g_calls.size(); ++i) loudAfter += g_calls[i].hash != zeroHash;
    CHECK(loudAfter >= 5 && NpAudioIsOpen() == 1);
    // (1.3) NpAudioIsOpen: what the sink asks to tell "no device" (pace on the system clock, try again) from a device that is open
    g_openFailures.store(1);
    CHECK(NpAudioOpen(44100) == 0 && NpAudioIsOpen() == 0);     // the port could not be had: not open
    CHECK(NpAudioOpen(44100) == 1 && NpAudioIsOpen() == 1);     // the retry after 2 s (the sink's) opens it
    StopDevice();
    CHECK(!g_testBroken);
    std::printf("audio output: %d checks, %d failed (port reopened %u times)\n", g_checks, g_failed, VitaAudio::Reopens());
    return g_failed ? 1 : 0;
}
