// MSC_GPU_IDLE_SKIP (vgx_gpuidle.h, the shipped header): which GPU samples are dropped. Pure.
//   gpuidle [--break=N]   N=1: the window is 1 picture (the sample of the picture before the idle one is wrongly kept), N=2: a picture with draws counts as idle (live play loses
//                         its samples), N=3: the ring never forgets (an idle picture silences the ladder for ever)
#include "vgx_gpuidle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using vgx::gpuidle::Ring;

namespace {
int g_fail = 0, g_pass = 0, g_break = 0;
void Check(bool ok, const char* what, const char* detail = "") {
    if (ok) { ++g_pass; std::printf("ok   %s %s\n", what, detail); }
    else { ++g_fail; std::printf("FAIL %s %s\n", what, detail); }
}
// A ring as the renderer drives it; the mutants change what it is fed or how it forgets.
struct Driver {
    Ring r;
    Driver() { if (g_break == 1) r.window = 1; }
    void Pic(unsigned draws) {
        if (g_break == 2) draws = draws ? 0 : 1;
        r.Picture(draws);
        if (g_break == 3 && draws == 0) r.bits |= 0x80000000u;   // (the idle picture is never forgotten)
    }
    bool Discard() const { return g_break == 3 ? (r.bits >> 31) != 0 || r.Discard() : r.Discard(); }
};
}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strncmp(argv[i], "--break=", 8)) g_break = std::atoi(argv[i] + 8);
    {   // live play: every picture draws, no sample is dropped
        Driver d;
        bool any = false;
        for (int i = 0; i < 1000; ++i) { d.Pic(500 + i % 7); any = any || d.Discard(); }
        Check(!any, "live play: no sample is dropped over 1000 pictures that draw");
    }
    {   // the boot of 6122: 40 pictures with draws 0, then pictures that draw
        Driver d;
        int dropped = 0;
        for (int i = 0; i < 40; ++i) { d.Pic(0); if (d.Discard()) ++dropped; }
        Check(dropped == 40, "boot: every sample read during 40 pictures of draws 0 is dropped");
        int firstOk = -1;
        for (int i = 0; i < 10; ++i) { d.Pic(8); if (!d.Discard() && firstOk < 0) firstOk = i; }
        char t[80];
        std::snprintf(t, sizeof t, "(first kept sample %d pictures after the last idle one)", firstOk);
        Check(firstOk == 2, "boot: the samples come back 3 pictures after the last picture that drew nothing (the GPU queue is at most 3 deep)", t);
    }
    {   // one idle picture in live play: dropped for exactly 3 reads, then back
        Driver d;
        for (int i = 0; i < 20; ++i) d.Pic(100);
        d.Pic(0);
        int dropped = 0;
        for (int i = 0; i < 10; ++i) { if (d.Discard()) ++dropped; d.Pic(100); }
        Check(dropped == 3, "a single picture of draws 0: three reads are dropped, no more (the ring forgets)");
    }
    {   // the sample read just before an idle picture was begun is kept
        Driver d;
        for (int i = 0; i < 10; ++i) d.Pic(100);
        Check(!d.Discard(), "the sample before an idle picture is kept (the picture it belongs to drew)");
    }
    Check(Ring{}.window == 3 && !Ring{}.Discard(), "a new ring drops nothing");
    std::printf("%d passed, %d failed%s\n", g_pass, g_fail, g_break ? " (deliberate break)" : "");
    return g_fail ? 1 : 0;
}
