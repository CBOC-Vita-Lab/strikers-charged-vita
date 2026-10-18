// Desire function tests: shared driver. Impl fills g_out (a sequence of numbers: effect logs from the helper stand-ins and
// the final state), the driver prints one line per test.
#pragma once
#include "dt_common.h"
namespace dt {
struct Out {
    float v[512]; int n = 0;
    void clear() { n = 0; }
    void add(float f) { if (n < 512) v[n++] = f; else abort(); }
};
inline Out g_out;
inline void lg(int code) { g_out.add((float)code); }
inline void lg(int code, float a) { lg(code); g_out.add(a); }
inline void lg(int code, float a, float b) { lg(code, a); g_out.add(b); }
inline void lg(int code, float a, float b, float c) { lg(code, a, b); g_out.add(c); }
inline void lg(int code, float a, float b, float c, float d) { lg(code, a, b, c); g_out.add(d); }
inline void lg(int code, float a, float b, float c, float d, float e) { lg(code, a, b, c, d); g_out.add(e); }
inline void lg(int code, float a, float b, float c, float d, float e, float f) { lg(code, a, b, c, d, e); g_out.add(f); }
enum { L_ADD = 1, L_AVOID, L_SETTIMER, L_ISTIMER, L_SETSEARCH, L_MALLOC, L_RESETSTEER, L_GETDESIRE, L_SKILL, L_DIST, L_SHOT, L_PASSACT, L_PREDICT, L_APPROACH, L_AIRH };
inline bool g_timer[8];
inline bool on(const char* n) { const char* e = getenv("DT_ONLY"); return !e || strstr(e, n); }

inline void printOut(int seed, const char* name)
{
    if (!on(name)) { g_out.clear(); return; }
    printf("d %d %s", seed, name);
    for (int i = 0; i < g_out.n; i++) printf(" %.7g", g_out.v[i]);
    printf("\n"); fflush(stdout);
    g_out.clear();
}

template <class Impl>
void runDesire(Impl& I, int seed0, int nseeds)
{
    for (int seed = seed0; seed < seed0 + nseeds; seed++) {
        Rng r(seed + 100000);
        Desc d;
        genDesc(d, r);
        I.setup(d);
        if (on("CUT_UPDATE")) { g_out.clear(); I.desireCut(d); printOut(seed, "CUT_UPDATE"); }
        if (on("GETOPEN_INIT")) { g_out.clear(); I.desireGetOpen(d); printOut(seed, "GETOPEN_INIT"); }
        if (on("RUN_UPDATE")) { g_out.clear(); I.desireRunUpdate(d); printOut(seed, "RUN_UPDATE"); }
        if (on("RUN_INIT")) { g_out.clear(); I.desireRunInit(d); printOut(seed, "RUN_INIT"); }
        if (on("INTERCEPT_INIT")) { g_out.clear(); I.desireIntercept(d); printOut(seed, "INTERCEPT_INIT"); }
        if (on("INTERCEPT_UPDATE")) { g_out.clear(); I.desireInterceptUpdate(d); printOut(seed, "INTERCEPT_UPDATE"); }
        if (on("TRANS_RUN")) { g_out.clear(); I.trans1(d); printOut(seed, "TRANS_RUN"); }
        if (on("TRANS_GETOPEN")) { g_out.clear(); I.trans2(d); printOut(seed, "TRANS_GETOPEN"); }
        if (seed == seed0 && on("WEATHER")) { g_out.clear(); I.weather(); printOut(seed, "WEATHER"); }
    }
}
}  // namespace dt
