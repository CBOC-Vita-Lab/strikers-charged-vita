// Model of the display gate of the vita host (vgx vita/vgx_host_vita.cpp GateDisplay, W5 FPS): a game that hands over a finished frame every `interval` refreshes, a GPU that
// finishes each one a variable time later, and the display that shows a buffer at the first vblank after sceDisplaySetFrameBuf. Prints how long each picture was shown (vblanks),
// without the gate and with it. The gate code below is a COPY of the algorithm in GateDisplay (keep in step).
//   clang++ -std=c++11 -O1 tools/fps/gate_sim.cpp -o gate_sim && ./gate_sim <interval> <latency min ms> <latency max ms> [frames] [seed]
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const double R = 1001.0 / 60000.0;   // one vblank, seconds

static uint32_t Vcount(double t) { return (uint32_t)floor(t / R); }   // vblanks that have started (the sceDisplayGetVcount of the model)
static double VblankTime(uint32_t v) { return (double)v * R; }

struct Result { uint64_t held[6]; uint64_t n; };

// gate: 0 = none, else the interval in vblanks
static Result Run(int interval, int gate, double lmin, double lmax, int frames, unsigned seed) {
    srand(seed);
    Result r = {{0, 0, 0, 0, 0, 0}, 0};
    bool have = false;
    uint32_t lastShown = 0;
    double handover = 10.0 * R + 0.2 * R;   // the game hands frame k over at the first retrace + a little, then every `interval` refreshes
    for (int k = 0; k < frames; ++k) {
        const double lat = (lmin + (lmax - lmin) * (rand() / (double)RAND_MAX)) * 1e-3;
        double t = handover + lat;                 // the GPU finished: the display-queue thread runs the callback now
        uint32_t shown = Vcount(t) + 1;            // SetFrameBuf(NEXTFRAME): shown at the next vblank
        if (gate && have) {                        // ---- copy of GateDisplay ----
            const uint32_t due = lastShown + (uint32_t)interval;
            int guard = 0;
            while ((int32_t)(due - shown) > 0 && guard++ < 8) {
                t = VblankTime(Vcount(t) + 1) + 20e-6;   // sceDisplayWaitVblankStart returns just after the vblank
                shown = Vcount(t) + 1;
            }
        }
        if (have) {
            const uint32_t held = shown - lastShown;
            ++r.held[held > 5 ? 5 : held];
            ++r.n;
        }
        lastShown = shown;
        have = true;
        // the game's next hand-over: `interval` retraces after this one (swap mode 4: retrace >= count at hand-over + interval)
        handover = VblankTime(Vcount(handover) + (uint32_t)interval) + 0.2 * R;
    }
    return r;
}

int main(int argc, char** argv) {
    const int interval = argc > 1 ? atoi(argv[1]) : 2;
    const double lmin = argc > 2 ? atof(argv[2]) : 14.0, lmax = argc > 3 ? atof(argv[3]) : 22.0;
    const int frames = argc > 4 ? atoi(argv[4]) : 3000;
    const unsigned seed = argc > 5 ? (unsigned)atoi(argv[5]) : 1;
    for (int gate = 0; gate <= 1; ++gate) {
        const Result r = Run(interval, gate, lmin, lmax, frames, seed);
        printf("interval %d, GPU latency %.0f..%.0f ms, gate %s: %llu pictures shown for 0/1/2/3/4/5+ vblanks: %llu/%llu/%llu/%llu/%llu/%llu  (%.1f%% as intended)\n", interval, lmin, lmax,
               gate ? "on " : "off", (unsigned long long)r.n, (unsigned long long)r.held[0], (unsigned long long)r.held[1], (unsigned long long)r.held[2], (unsigned long long)r.held[3],
               (unsigned long long)r.held[4], (unsigned long long)r.held[5], r.n ? 100.0 * r.held[interval > 5 ? 5 : interval] / r.n : 0.0);
    }
    return 0;
}
