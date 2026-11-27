// MSC_FPS_BETWEEN and MSC_PRESENT_SYNC in the real np_fps.cpp, with virtual retraces, a virtual clock and a fake per-core probe.
//   fps_mid_unit behaviour   the switch's rules (run with MSC_FPS=auto MSC_FPS_BETWEEN=pace MSC_SLIP=20)
//   fps_mid_unit trace       a LAN session, one line: a hash of everything a frame asks this file (swap mode, display interval, policy, slip, the waits made). tools/fps/run_pace_tests.sh
//                            compares it with and without the switches: in a session they must not change one of them.
#define NP_NATIVE 1
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../platform/compat/np_fps.cpp"

namespace {
int64_t g_nowNs = 1000000000;
unsigned long g_retrace = 100;
uint64_t g_sent, g_shown;
uint32_t g_waits;
bool g_lan, g_mega;
int g_vcount = 5000;
double g_cpu[4];
int g_failures, g_checks;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("FAIL: %s\n", what); }
}
int Probe(NpFpsProbeOut* o) {
    for (int c = 0; c < 4; ++c) { o->cpuMs[c] = g_cpu[c]; o->timeMs[c] = 0; }
    o->gameCore = 0;
    return 1;
}
double ThreadMs() { return (double)g_nowNs * 1e-6; }
}  // namespace

extern "C" {
int64_t NpMonotonicNs(void) { return g_nowNs; }
unsigned long VIGetRetraceCount(void) { return g_retrace; }
void VIWaitForRetrace(void) { ++g_retrace; ++g_waits; g_nowNs += 16683333; }
void NpLatLatch(void) { ++g_sent; }
uint64_t NpLatHandedOverTag(void) { return g_sent; }
uint64_t NpLatShownTag(void) { return g_shown; }
int sceDisplayGetVcount(void) { return g_vcount; }
int VgxMegaActive(void) { return g_mega ? 1 : 0; }
}
bool NpLanActive() { return g_lan; }

namespace {
struct Seen { int swapMode, interval, policy, syncMode; float slip; };

// One frame of `ms` on the busiest core, the way glxSwap.cpp and glPlat.cpp call this file.
Seen Frame(unsigned state, double ms, int gameMode = 3) {
    g_nowNs += (int64_t)(ms * 1e6);
    g_retrace += 1;
    g_vcount += 1;
    g_cpu[1] += ms;
    g_cpu[0] += 8.0;
    NpFpsBeginFrame((float)(ms / 1000.0), state);
    Seen s;
    s.swapMode = NpFpsSwapMode(gameMode);
    NpFpsSwapWaitBegin();
    NpFpsSwapWaitEnd();
    if (s.swapMode == 4) { NpFpsLateWait(4, g_retrace + 2); }
    NpFpsLateWaitAfterSend();
    g_shown = g_sent > 2 ? g_sent - 2 : 0;
    s.interval = NpFpsDisplayInterval();
    s.policy = NpFpsPolicy();
    s.syncMode = NpFpsPresentSyncMode();
    s.slip = NpSlipDelta((float)(ms * 2.2 / 1000.0));
    return s;
}

void Behaviour() {
    NpFpsSetProbe(Probe);
    NpFpsSetThreadClock(ThreadMs);
    Seen s = Frame(2, 33.4);
    Check(s.swapMode == 4 && s.interval == 2, "auto starts held");
    for (int i = 0; i < 12; ++i) s = Frame(2, 33.4);
    Check(s.swapMode == 4 && !NpFpsPaced(), "not before the hold time");
    for (int i = 0; i < 60; ++i) s = Frame(2, 24.0);
    Check(NpFpsPaced() == 1, "a scene at 24 ms goes free");
    Check(s.swapMode == 3 && s.interval == 0 && s.policy == kPolicyUnlocked, "swap mode 3, no display gate; the policy reads as unlocked");
    Check(NpFpsSwapMode(2) == 2, "the game's cut-scene lock stays");
    Check(NpFpsSwapMode(0) == 0, "and its mode 0");
    Check(s.syncMode == 2, "unset MSC_PRESENT_SYNC: aligned while paced");
    Check(std::fabs(NpSlipDelta(0.030f) - 0.030f) < 1e-6f, "the slip keeps the two-refresh budget: a 30 ms frame is on time");
    Check(NpSlipDelta(0.060f) < 0.060f, "a really late one is slipped");
    {
        char line[400];
        NpFpsDescribe(line, sizeof line);
        Check(std::strstr(line, "between pace") != 0, "the run header names the switch");
    }
    {
        const uint64_t tag = g_sent;
        uint16_t v = 0;
        Check(NpFpsPictureStamp(tag, &v) == 1 && v == (uint16_t)g_vcount, "the handed-over frame carries the vblank it began at");
        Check(NpFpsPictureStamp(tag + 64, &v) == 0 && NpFpsPictureStamp(0, &v) == 0, "a frame number that is not in the slot has no stamp");
    }
    s = Frame(8, 24.0);
    Check(!NpFpsPaced() && s.swapMode == 4 && s.interval == 2, "a replay is held at once");
    s = Frame(1, 24.0);
    Check(!NpFpsPaced() && s.syncMode == 1, "the pause screen too; policy 30 is only measured");
    for (int i = 0; i < 60; ++i) s = Frame(2, 24.0);
    Check(NpFpsPaced() == 1, "back in play: free again");
    g_mega = true;
    s = Frame(2, 24.0);
    Check(!NpFpsPaced() && s.swapMode == 4, "a Mega Strike is held");
    g_mega = false;
    for (int i = 0; i < 60; ++i) s = Frame(2, 24.0);
    Check(NpFpsPaced() == 1, "and free after it");
    g_lan = true;
    s = Frame(2, 24.0);
    Check(!NpFpsPaced() && s.swapMode == 4 && s.interval == 2, "a LAN or online session ends it at the next frame");
    for (int i = 0; i < 200; ++i) s = Frame(2, 24.0);
    Check(!NpFpsPaced() && s.swapMode == 4 && s.syncMode == 1, "and it stays off in the session");
    g_lan = false;
    for (int i = 0; i < 60; ++i) s = Frame(2, 24.0);
    Check(NpFpsPaced() == 1, "free again after the session");
    for (int i = 0; i < 60; ++i) s = Frame(2, 34.0);
    Check(!NpFpsPaced() && s.swapMode == 4, "a scene over the leave threshold goes back to held 30");
    // AUTO's own 60 is not touched.
    bool sixty = false;
    for (int i = 0; i < 2000 && !sixty; ++i) { g_cpu[1] -= 3.0; g_cpu[0] -= 0.0; s = Frame(2, 11.0); sixty = s.policy == kPolicy60; }
    Check(sixty && !NpFpsPaced(), "a light scene still reaches 60, not paced");
}

void Trace() {
    g_lan = true;
    NpFpsSetProbe(Probe);
    NpFpsSetThreadClock(ThreadMs);
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
    for (int i = 0; i < 6000; ++i) {
        const double ms = i % 900 < 300 ? 24.0 : (i % 900 < 600 ? 11.0 : 34.0);
        const unsigned state = i % 700 > 660 ? 8u : 2u;
        const Seen s = Frame(state, ms);
        uint32_t slip;
        std::memcpy(&slip, &s.slip, 4);
        mix((uint32_t)s.swapMode); mix((uint32_t)s.interval); mix((uint32_t)s.policy); mix(slip); mix((uint32_t)g_waits);
        if (i % 500 == 0) mix(NpFpsPaced() ? 77u : 0u);
    }
    std::printf("trace %08x\n", h);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "trace") == 0) { Trace(); return 0; }
    Behaviour();
    std::printf("%s: fps_mid_unit, %d checks, %d failed\n", g_failures ? "FAILED" : "passed", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
