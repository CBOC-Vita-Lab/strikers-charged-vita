// Deterministic tests of the real late-wait / frames-in-flight gate, with virtual
// retraces and a pipelined display. This is not a Vita performance measurement.
// From the repository root:
// clang++ -std=c++11 -O2 -Inative/include native/tools/fps/fps_gate_unit.cpp -o /tmp/fps_gate_unit && /tmp/fps_gate_unit
#define NP_NATIVE 1
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include "../../platform/compat/np_fps.cpp"

extern "C" int g_npGsOn = 0;
extern "C" int NpGsEnter(int) { return -1; }
extern "C" void NpGsLeave(int) {}

namespace {
double ticks;
uint64_t sent, shown;
bool frozen;
struct Due { double tick; uint64_t tag; };
std::deque<Due> pending;
int failures, checks;

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}

void UpdateDisplay() {
    while (!frozen && !pending.empty() && pending.front().tick <= ticks) {
        shown = pending.front().tag;
        pending.pop_front();
    }
}

int anchorMode = 0, retryMode = 0;   // MSC_LAT_ANCHOR / MSC_LAT_RETRY of the run (the tests of the 1.1 gate fix are run with both off: its own behaviour)

void Reset(int depth) {
    ticks = 0;
    sent = shown = 0;
    frozen = false;
    pending.clear();
    g_late = LateWait{};
    g_fifDepth = -1;
    g_fifCapHits = 0;
    g_fifWaits = g_fifFrames = g_fifVblanks = g_fifPaused = g_fifPauses = 0;
    g_fifHaveCheck = false;
    g_fifCalls = g_fifRetryAt = g_fifStrikes = 0;
    g_fifRetry = -1;
    g_lateAnchorMode = -1;
    setenv("MSC_LAT_BP", "0", 1);   // wave 11 made back-pressure default ON; this unit tests the frames-in-flight gate (MSC_LAT_DEPTH)
    setenv("MSC_LAT_ANCHOR", anchorMode ? "1" : "0", 1);
    setenv("MSC_LAT_RETRY", retryMode ? "1" : "0", 1);
    g.policy = npfps::kPolicy30;
    char text[16];
    std::snprintf(text, sizeof text, "%d", depth);
    setenv("MSC_LAT_DEPTH", text, 1);
    setenv("MSC_LATE_WAIT", "1", 1);
}

// One phase of a run: `frames` frames of `work` refreshes of game work each, the pipeline needing `latency` refreshes from the hand-over to the
// picture, the display stopped (`freeze`).
struct Phase { int frames; double latency; bool freeze; double work; };
double g_lastPhaseFps;

double RunPhases(int depth, const Phase* ph, int n, bool quiet = false) {
    Reset(depth);
    double totalTicks = 0; int totalFrames = 0;
    for (int k = 0; k < n; ++k) {
        const double t0 = ticks;
        const uint64_t waits0 = g_fifWaits;
        frozen = ph[k].freeze;
        if (frozen && shown == 0) shown = 1;
        if (!frozen && k > 0 && ph[k - 1].freeze) {   // the display wakes: it shows what it has
            while (!pending.empty()) { shown = pending.front().tag; pending.pop_front(); }
            shown = sent;
        }
        for (int i = 0; i < ph[k].frames; ++i) {
            ticks += ph[k].work;
            ++sent;
            double due = std::ceil((ticks + ph[k].latency) / 2.0) * 2.0;
            if (!pending.empty() && due < pending.back().tick + 2.0) due = pending.back().tick + 2.0;
            pending.push_back({due, sent});
            UpdateDisplay();
            Check(NpFpsLateWait(4, VIGetRetraceCount() + 2) == 1, "late wait remains enabled");
            NpFpsLateWaitAfterSend();
        }
        g_lastPhaseFps = ph[k].frames / ((ticks - t0) * (1001.0 / 60000.0));
        if (!quiet) std::printf("   phase %d: %d frames, work %.1f, latency %.1f%s: %.3f fps, gate waits %llu, cap hits %d, pauses %u\n", k, ph[k].frames, ph[k].work,
                    ph[k].latency, ph[k].freeze ? " (display frozen)" : "", g_lastPhaseFps, (unsigned long long)(g_fifWaits - waits0), g_fifCapHits, g_fifPauses);
        totalTicks += ticks - t0; totalFrames += ph[k].frames;
    }
    return totalFrames / (totalTicks * (1001.0 / 60000.0));
}

double RunFrames(int depth, double latency, int frames = 1200, bool freeze = false) {
    Reset(depth);
    frozen = freeze;
    if (frozen) shown = 1; // a display that stopped after its first numbered frame
    for (int i = 0; i < frames; ++i) {
        ticks += 0.4; // bounded game work, shorter than the 30 fps budget
        ++sent;
        // The downstream stages can overlap. Tags become available after a fixed
        // latency, at most one per two refreshes, independently of the gate.
        double due = std::ceil((ticks + latency) / 2.0) * 2.0;
        if (!pending.empty() && due < pending.back().tick + 2.0)
            due = pending.back().tick + 2.0;
        pending.push_back({due, sent});
        UpdateDisplay();
        Check(NpFpsLateWait(4, VIGetRetraceCount() + 2) == 1, "late wait remains enabled");
        NpFpsLateWaitAfterSend();
    }
    const double fps = frames / (ticks * (1001.0 / 60000.0));
    std::printf("depth=%d latency=%.1f: %.3f fps, pauses=%u, cap hits=%d%s\n",
                depth, latency, fps, g_fifPauses, g_fifCapHits, freeze ? " (display frozen)" : "");
    return fps;
}
}

extern "C" unsigned long VIGetRetraceCount() { return static_cast<unsigned long>(ticks); }
extern "C" void VIWaitForRetrace() { ticks = std::floor(ticks) + 1; UpdateDisplay(); }
extern "C" uint64_t NpLatHandedOverTag() { return sent; }
extern "C" uint64_t NpLatShownTag() { return shown; }
extern "C" int64_t NpMonotonicNs() { return static_cast<int64_t>(ticks * (1001.0 / 60000.0) * 1e9); }
extern "C" int64_t NpVirtualNowNs() { return NpMonotonicNs(); }
extern "C" int NpVirtualTimeActive() { return 0; }
extern "C" uint32_t NpVI_GetRetraceCount() { return static_cast<uint32_t>(VIGetRetraceCount()); }
extern "C" int NpFpsPlatformProbe(NpFpsProbeOut*) { return 0; }
extern "C" double NpFpsPlatformThreadClockMs() { return 0; }
extern "C" void NpLatLatch() {}
extern "C" float VgxGpuMinMs() { return 0; }
extern "C" int VgxMegaActive() { return 0; }

int main() {
    std::printf("== the 1.1 gate fix alone (MSC_LAT_ANCHOR=0, MSC_LAT_RETRY=0) ==\n");
    anchorMode = 0; retryMode = 0;
    const double noGate = RunFrames(0, 6.5);
    Check(noGate > 29.9 && g_fifWaits == 0 && g_fifPauses == 0, "disabled gate preserves 30 fps");
    const double gated = RunFrames(2, 6.5);
    Check(gated > noGate * 0.99, "a gate's own extra refresh cannot sustain a 30-to-24 fps slowdown");
    Check(g_fifPauses > 0, "self-induced slow cadence triggers the existing backoff");
    const double slower = RunFrames(2, 8.5);
    Check(slower > noGate * 0.99, "a successful wait each frame cannot sustain a 30-to-20 fps slowdown");
    const double healthy = RunFrames(2, 5.5);
    Check(healthy > 29.9 && g_fifWaits == 0 && g_fifPauses == 0, "healthy pipeline needs no waits or backoff");
    Check(RunFrames(3, 6.5) > 29.9 && g_fifWaits == 0 && g_fifPauses == 0, "an adequate depth does not back off");

    Reset(2);
    sent = 10;
    FramesInFlightGate();
    Check(ticks == 0 && g_fifFrames == 0 && !g_fifHaveCheck, "no numbered display tag leaves the gate inactive");

    Reset(2);
    g.inited = true;
    g.mode = kModeAuto;
    g.policy = npfps::kPolicy60;
    sent = 10;
    shown = 1;
    const int swapMode = NpFpsSwapMode(3);
    Check(swapMode == 1 && NpFpsDisplayInterval() == 1, "AUTO at 60 fps uses the one-refresh swap mode");
    Check(NpFpsLateWait(swapMode, 1) == 0 && !g_late.sendOwed, "60 fps cannot arm the 30 fps late wait");
    NpFpsLateWaitAfterSend();
    Check(ticks == 0 && g_fifDepth == -1, "60 fps does not enter the frames-in-flight gate");

    Reset(2);
    sent = shown = 1;
    FramesInFlightGate();
    ticks += 3; // ordinary work or the pipeline missed the previous two-refresh budget
    sent = 5;
    const double before = ticks;
    FramesInFlightGate();
    Check(ticks == before && g_fifPaused == 149 && g_fifPauses == 1, "existing heavy-frame backoff skips the gate");
    for (int i = 0; i < 149; ++i) { ticks += 2; FramesInFlightGate(); }
    Check(g_fifPaused == 0 && g_fifWaits == 0, "heavy-frame backoff retains its 150-frame duration");

    RunFrames(2, 8.5, 6000, true);
    Check(g_fifCapHits == 30, "repeated unresolved waits still disable the gate");

    // ---- 1.2: MSC_LAT_ANCHOR=1 (the anchor before the gate's refresh), MSC_LAT_RETRY=1 --------------------------------------------------------------
    std::printf("== 1.2: MSC_LAT_ANCHOR=1, MSC_LAT_RETRY=1 ==\n");
    anchorMode = 1; retryMode = 1;
    Check(RunFrames(0, 6.5) > 29.9 && g_fifWaits == 0, "anchor: disabled gate preserves 30 fps");
    const double gated1 = RunFrames(2, 6.5);
    Check(gated1 > 29.9, "anchor: a gate that waits every frame does not slow the 30 fps cadence");
    Check(g_fifWaits > 0 && g_fifPauses == 0, "anchor: the gate is not put to sleep by its own wait");
    Check(RunFrames(2, 8.5) > 29.9, "anchor: a successful wait each frame cannot sustain a 30-to-20 fps slowdown");
    Check(RunFrames(2, 5.5) > 29.9 && g_fifWaits == 0 && g_fifPauses == 0, "anchor: a healthy pipeline needs no waits or backoff");
    {
        // a frame whose work is longer than one refresh: the gate's refresh would make it late; the lateness test still stands aside
        const Phase heavy[] = {{1500, 5.5, false, 1.4}};
        const double fps = RunPhases(2, heavy, 1);
        Check(fps > 29.9 || fps > 19.9, "anchor: 1.4 refreshes of work stay at 30 or 20 per the work");   // (sanity; the real check is the next)
        const Phase mid[] = {{1500, 6.5, false, 1.1}};
        const double fm = RunPhases(2, mid, 1);
        Check(fm > 28.5, "anchor: 1.1 refreshes of game work under a gate that wants to wait: the lateness back-off keeps >= 28.5 fps");
    }
    {
        // GPU-heavy stretch: the display lags 3+ frames for 600 frames (latency 5.5 -> 12.5), then recovers
        const Phase gpu[] = {{600, 5.5, false, 0.4}, {600, 12.5, false, 0.4}, {1500, 5.5, false, 0.4}};
        const double fpsAnchor = RunPhases(2, gpu, 3);
        Check(fpsAnchor > 29.7, "anchor: a GPU-heavy stretch (display 3 frames behind) and its recovery stay at 30 fps");
        Check(g_lastPhaseFps > 29.9, "anchor: after the stretch the cadence is 30 again");
        anchorMode = 0; retryMode = 0;
        const double fpsOld = RunPhases(2, gpu, 3);
        std::printf("   (same scenario with the 1.1 fix alone: %.3f fps)\n", fpsOld);
        anchorMode = 1; retryMode = 1;
    }
    {
        // the gate switched itself off in the stretch: it is back for the calm part (retry), not off for good
        const Phase gpu[] = {{300, 5.5, false, 0.4}, {900, 14.5, true, 0.4}, {3000, 6.5, false, 0.4}};
        retryMode = 0;
        RunPhases(2, gpu, 3, true);
        const uint32_t waitsNoRetry = g_fifWaits;
        const int capNoRetry = g_fifCapHits;
        retryMode = 1;
        RunPhases(2, gpu, 3, true);
        std::printf("   after a frozen-display stretch: gate waits in the last phase, retry off cap hits %d / retry on %d (waits %u vs %u)\n", capNoRetry, g_fifCapHits, waitsNoRetry, g_fifWaits);
        Check(capNoRetry == 30, "retry off: a gate cut off by a frozen display stays off for the session (1.1)");
        Check(g_fifStrikes >= 1 && g_fifWaits > waitsNoRetry, "retry on: the cut-off gate tries again once the display runs");
    }
    {
        // the cut-off is unchanged and the retries are bounded on a display that stays frozen
        const Phase frozenAll[] = {{20000, 8.5, true, 0.4}};
        const double fps = RunPhases(2, frozenAll, 1, true);
        Check(fps > 29.7, "retry: a display frozen for 11 minutes costs at most a few refreshes of retries");
        Check(g_fifWaits <= 30u * (g_fifStrikes + 1), "retry: every retry probes at most 30 waits");
        std::printf("   frozen display: %.3f fps, %u strikes, %u waits in 20000 frames\n", fps, g_fifStrikes, g_fifWaits);
    }
    {
        // a policy change breaks the cadence record: 60 fps frames in between do not make the next 30 fps frame "late"
        Reset(2);
        g_fifHaveCheck = true;
        g_fifLastCheck = 0;
        ticks = 500;
        NpFpsLateWait(1, 0);
        Check(!g_fifHaveCheck, "a frame without the 30 fps wait clears the gate's cadence record");
    }
    std::printf("frames-in-flight gate: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
