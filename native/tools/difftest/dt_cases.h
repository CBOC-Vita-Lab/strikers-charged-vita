// the test driver, shared by dt_a and dt_b: Impl supplies make*/eval/findBest for its side
#pragma once
#include "dt_common.h"
namespace dt {

enum Kind { K_OL_SHOT, K_OL_PASS, K_OL_PASS_NOOWNER, K_GO, K_IS, K_RN, K_CB, K_BP_LEAD, K_BP_PERFECT, K_BP_PLAIN, K_COUNT };
inline const char* kindName(int k)
{
    static const char* n[] = { "OL_SHOT", "OL_PASS", "OL_PASS_NOOWNER", "GO", "IS", "RN", "CB", "BP_LEAD", "BP_PERFECT", "BP_PLAIN" };
    return n[k];
}

template <class Impl>
void runCases(Impl& I, int seed0, int nseeds)
{
    typedef typename Impl::H H;
    for (int seed = seed0; seed < seed0 + nseeds; seed++) {
        Rng r(seed);
        Desc d;
        genDesc(d, r);
        I.setup(d);
        for (int kind = 0; kind < K_COUNT; kind++) {
            int team = r.ri(2);
            int owner = team * 5 + r.ri(4);
            int target = team * 5 + r.ri(4);
            if (target == owner) target = team * 5 + ((owner % 5) + 1) % 4;
            if (kind != K_BP_LEAD && kind != K_BP_PERFECT && kind != K_BP_PLAIN && r.chance(0.2f)) { /* random goalie owner */ }
            float speed = r.rng(8, 16);
            H obj = I.make(kind, owner, target, speed);
            for (int t = 0; t < 4; t++) {
                V3 pos = { r.rng(-20, 20), r.rng(-10, 10), 0.0f };
                V3 center = { r.rng(-20, 20), r.rng(-10, 10), 0.0f };
                float v = I.eval(kind, obj, pos, center, 0, 0);
                printf("e %d %s %d %.7g\n", seed, kindName(kind), t, v);
            }
            for (int t = 0; t < 3; t++) {
                int dir = r.ri(6);
                V3 center = { r.rng(-20, 20), r.rng(-10, 10), 0.0f };
                V3 tgt = { r.rng(-20, 20), r.rng(-10, 10), 0.0f };
                float radius = r.chance(0.2f) ? 0.0f : r.rng(2, 9);
                int cone = (r.chance(0.5f) ? 0x8000 : (int)(r.next() & 0xFFFF));
                V3 dest = { 0, 0, 0 };
                float v = I.findBest(kind, obj, dest, center, dir, tgt, radius, cone);
                printf("f %d %s %d %.7g %.6g %.6g %.6g\n", seed, kindName(kind), t, v, dest.x, dest.y, dest.z);
            }
        }
    }
}
}  // namespace dt
