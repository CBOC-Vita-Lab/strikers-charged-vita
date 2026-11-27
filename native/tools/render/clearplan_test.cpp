// The clear planner (renderer/src/vgx_clearplan.h) against a plain pixel simulation: for random lists of clears (rectangles, planes, values from a small
// set so that repeats and neighbours happen) the pixels of every plane after the planned clears equal the pixels after all the clears in order, for each
// combination of the two rules.   native/tools/render/run_clearplan_test.sh
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "vgx_clearplan.h"

using namespace vgx;
using namespace vgx::backend;

static const int W = 10, H = 7;
struct Px { float rgb[3]; float a; float z; };
struct Img { Px p[W * H]; };

static void Apply(Img& im, const ClrEnt& e) {
    for (int y = e.y0; y < e.y1; ++y)
        for (int x = e.x0; x < e.x1; ++x) {
            Px& q = im.p[y * W + x];
            if (e.c.color) { q.rgb[0] = e.c.rgba[0]; q.rgb[1] = e.c.rgba[1]; q.rgb[2] = e.c.rgba[2]; }
            if (e.c.alpha) q.a = e.c.rgba[3];
            if (e.c.depth) q.z = e.c.z;
        }
}
static unsigned rnd() {
    static unsigned long long s = 0x9E3779B97F4A7C15ull;
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<unsigned>(s >> 33);
}

int main() {
    const float vals[3] = {0.0f, 0.5f, 1.0f};
    unsigned long long total = 0, dead = 0, repeat = 0, fold = 0, cases = 0;
    for (int iter = 0; iter < 200000; ++iter) {
        const int n = 1 + static_cast<int>(rnd() % 8);
        std::vector<ClrEnt> list;
        for (int i = 0; i < n; ++i) {
            ClrEnt e;
            e.c.enable = true;
            e.c.color = rnd() % 3 != 0;
            e.c.alpha = rnd() % 3 != 0;
            e.c.depth = rnd() % 2 != 0;
            for (int k = 0; k < 4; ++k) e.c.rgba[k] = vals[rnd() % 3];
            e.c.z = vals[rnd() % 3];
            if (rnd() % 2) { e.x0 = 0; e.y0 = 0; e.x1 = W; e.y1 = H; } // often the whole target
            else {
                e.x0 = static_cast<int>(rnd() % W); e.x1 = e.x0 + 1 + static_cast<int>(rnd() % (W - e.x0));
                e.y0 = static_cast<int>(rnd() % H); e.y1 = e.y0 + 1 + static_cast<int>(rnd() % (H - e.y0));
            }
            e.planes = ClearPlanes(e.c);
            if (!e.planes) continue;
            list.push_back(e);
        }
        Img ref;
        std::memset(&ref, 0, sizeof ref);
        for (int y = 0; y < H * W; ++y) { ref.p[y].z = 7.0f; ref.p[y].a = 7.0f; ref.p[y].rgb[0] = ref.p[y].rgb[1] = ref.p[y].rgb[2] = 7.0f; }
        Img start = ref;
        for (const ClrEnt& e : list) Apply(ref, e);
        for (int mode = 1; mode < 4; ++mode) {
            std::vector<ClrEnt> plan = list;
            uint32_t d = 0, r = 0, f = 0;
            PlanClears(plan, (mode & 1) != 0, (mode & 2) != 0, &d, &r, &f);
            Img im = start;
            for (const ClrEnt& e : plan)
                if (!e.fate) Apply(im, e);
            if (std::memcmp(&im, &ref, sizeof im) != 0) {
                std::printf("MISMATCH mode %d iter %d n=%zu\n", mode, iter, list.size());
                for (const ClrEnt& e : list)
                    std::printf("  rect %d,%d-%d,%d %c%c%c rgba %.1f %.1f %.1f %.1f z %.1f\n", e.x0, e.y0, e.x1, e.y1, e.c.color ? 'C' : '-', e.c.alpha ? 'A' : '-', e.c.depth ? 'Z' : '-',
                                e.c.rgba[0], e.c.rgba[1], e.c.rgba[2], e.c.rgba[3], e.c.z);
                return 1;
            }
            ++cases;
            dead += d; repeat += r; fold += f;
        }
        total += list.size();
    }
    std::printf("clearplan: %llu plans over %llu clears, all pixel-identical (dead %llu, repeat %llu, folded %llu)\n", cases, total, dead, repeat, fold);
    return 0;
}
