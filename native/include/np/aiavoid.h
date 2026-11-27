// Wave 10 (MSC_AI_AVOID): cheaper AvoidController::Update. Simulation-exact: every result is the one the plain code gives, bit for bit.
// MSC_AI_AVOID is a bit mask (code default 0, Vita plan 3):
//   1  CanAddAvoidance: the pure tests (not myself, does this fielder avoid that kind of thing) run before the AVL lookup of the already tracked
//      avoidances, so the lookup is only made for an object the later tests could still accept. Same conjunction, same order for every test that is
//      not pure (a polygon's IsWithinRange can draw nlRandom: it is still reached only for an untracked object that passed every earlier test).
//   2  AvoidableObject::IsWithinRange (the base one: powerups, patches, points; fielders and goalies read the cached pair distance table and
//      polygons have their own override, both untouched): a squared-distance reject before the emulated square root. The squared distance is
//      compared with (radii + range)^2 times 1.000244 (2^-12; the computed root is within 2^-22 of the true one), so only a pair that is surely
//      out of range is rejected; everything else takes the exact code.
//   4  verify (with 2): the exact code runs as well and a reject that the exact code would not make is counted ("wrong", must be 0); the exact result is used.
// MSC_AI_AVOID_BREAK=1 (proof mutant for bit 2): the reject test is far too eager (distance over half the range sum), the simulation changes.
// MSC_AI_AVOID_BREAK=2 (proof mutant for bit 1): an object that is already tracked is added again.
// [aiavoid] counters at exit.
#pragma once
#ifdef NP_NATIVE
int NpAiAvoidMode(void);
int NpAiAvoidBreak(void);
void NpAiAvoidCount(int which, int n);   // 0 CanAddAvoidance tests, 1 AVL lookups avoided, 2 base IsWithinRange calls, 3 square roots avoided, 4 wrong (verify)
#else
static inline int NpAiAvoidMode(void) { return 0; }
static inline int NpAiAvoidBreak(void) { return 0; }
static inline void NpAiAvoidCount(int, int) {}
#endif
