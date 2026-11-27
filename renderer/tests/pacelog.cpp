// Wave 9 (pacing): the MSC_PACE_LOG window figures (vgx_pacelog.h). -DVGX_PACELOG_BREAK=1..3 must fail (run_pacelog_test.sh).
#include "../src/vgx_pacelog.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace vgx::pacelog;
static int g_fail = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }

int main() {
    {   // the histogram
        Hist h;
        for (int i = 0; i < 90; ++i) h.Add(2.5f);
        for (int i = 0; i < 9; ++i) h.Add(40.2f);
        h.Add(250.0f);
        Check(h.n == 100 && h.max == 250.0f, "hist: count and maximum");
        Check(h.Percentile(0.5) == 2.0f, "hist: the median is the bin of 2.5 ms (2)");
        Check(h.Percentile(0.95) == 40.0f, "hist: p95 is the bin of 40.2 ms (40)");
        Check(h.Percentile(0.999) == 250.0f, "hist: a sample over 100 ms reports the maximum");
        Hist e;
        Check(e.Percentile(0.5) == 0.0f && e.Mean() == 0.0f, "hist: empty is 0");
    }
    {   // the display window: a steady 30 with an id gap, a repeat, a long hold at picture 1198 (picture % 600 = 598, bin 9) and one at 650 (bin 0)
        DisplayWin w;
        uint32_t shown = 1000, last = 0;
        bool have = false;
        const uint64_t ids[] = {1, 2, 3, 6, 6, 7, 8, 9, 10, 11};   // 3 -> 6: two ids never shown; 6 twice
        uint64_t pic = 1190;
        double t = 0;
        for (uint64_t id : ids) {
            const uint32_t hold = (pic == 1198) ? 3u : 2u;   // picture 1198 is held 3 vblanks
            if (have) shown += hold; else shown = 1000;
            w.Shown(t, shown - 1, shown, last, have, have ? 1u : 0u, id, pic, true);
            last = shown;
            have = true;
            ++pic;
            t += 33.4;
        }
        w.Shown(t, 2000, 2003, 2000, true, 2, 12, 650, true);   // a held-3 at picture 650 (bin 0), shown 3 vblanks after its first possible one... (earliest 2000)
        Check(w.pictures == 11 && w.ids == 11, "display: pictures and ids shown");
        Check(w.idGaps == 2, "display: the two ids that were never shown (3 -> 6) are counted");
        Check(w.idRepeats == 1, "display: an id shown twice is counted as a repeat, not as a gap");
        Check(w.longHolds == 2, "display: two holds of 3 vblanks or more");
        Check(w.fold[9] == 1 && w.fold[0] == 1, "display: the long holds are binned by picture % 600 in 60-wide bins (1198 -> bin 9, 650 -> bin 0)");
        Check(w.held[3] == 2 && w.held[2] >= 7, "display: the hold mix");
        Check(w.flipLate[3] == 1, "display: the picture shown 3 vblanks after its first possible vblank is counted");
        w.Delay(1001, 998);            // 3 vblanks = 50 ms
        w.Delay(70000u, (70000u - 3u) & 0xFFFFu);    // the stamp keeps 16 bits of the count: 3 vblanks
        w.Delay(1001, 1000);           // 1 vblank
        Check(w.toShown.n == 3 && std::fabs(w.toShown.Mean() - 38.93f) < 0.5f, "display: frame start to shown, the 16-bit stamp's wrap undone (mean of 3, 3, 1 vblanks)");
        char line[1200];
        const size_t n = w.Print(line, sizeof line, 5000.0, 1200, 2);
        Check(n > 100 && std::strstr(line, "2 never shown") && std::strstr(line, "1 shown twice"), "display: the line names the ids");
        Check(w.pictures == 0 && w.startMs == 5000.0, "display: the window is cleared by Print");
        char tiny[60];
        w.Shown(5001.0, 1, 1, 0, false, 0, 0, 1, false);
        const size_t m = w.Print(tiny, sizeof tiny, 5002.0, 1, 2);
        Check(m < sizeof tiny, "display: Print never writes past its buffer");
        for (int i = 0; i < 100; ++i) w.LongHoldLine();
        Check(!w.LongHoldLine(), "display: the long-hold lines are capped");
    }
    {   // the render window
        RenderWin r;
        for (int i = 0; i < 100; ++i) r.Swap(1000.0 + i * 33.4, i % 10 == 0 ? 66.0f : 0.5f, i % 5 == 0 ? 12.0f : 0.0f, i % 2 ? 28.0f : -1.0f);
        Check(r.swaps == 100 && r.limiterWaits == 20 && r.gpu.n == 50, "render: swaps, limiter waits, timed GPU frames");
        Check(r.queue.max == 66.0f && r.queue.Percentile(0.95) == 66.0f && r.queue.Percentile(0.5) == 0.0f, "render: display-queue call median 0 ms, p95 and max 66 ms");
        Check(r.Due(1000.0 + 5000.0, 5000.0) && !r.Due(1000.0 + 4000.0, 5000.0), "render: the window is due after its length");
        char line[800];
        r.Print(line, sizeof line, 4400.0, 100);
        Check(std::strstr(line, "limiter waited 20 times") && std::strstr(line, "(50 timed)"), "render: the line carries the counts");
    }
    std::printf("%d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
