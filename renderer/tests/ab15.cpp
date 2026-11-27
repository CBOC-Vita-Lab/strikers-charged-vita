// MSC_AB (src/vgx_ab.h): the in-run A/B harness. Spec parser, window schedule, the registry audit, the override that EnvInt answers, the VGX_KNOB words that a flip re-stores,
// the render thread's scene-boundary apply, and the "OFF arm is bit-exact" rule: with both arms equal every read answers what the environment gave.
#include "vgx_common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace vgx;
static int g_fail = 0;
static void Check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }

// the shapes of real knobs: PX_NOOP (a decision at every use), BEPREFETCH (clamped value), and one the harness must never touch
static int ReadNoop() { return static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_PX_NOOP", 1) != 0)) ? 1 : 0; }
static int ReadPrefetch() { return static_cast<int>(VGX_KNOB(std::min<long>(8, std::max<long>(0, EnvInt("MSC_VGX_BEPREFETCH", 0))))); }
static int ReadOther() { return static_cast<int>(VGX_KNOB(EnvInt("MSC_VGX_PX_SCISSOR", 1))); }
static long g_applied = -1; static int g_appliedN = 0;
static void Applied(long v) { g_applied = v; ++g_appliedN; }

int main() {
    // ---- spec
    ab::Spec s; char err[96];
    Check(ab::ParseSpec("MSC_VGX_PX_NOOP:1,0", s, err, sizeof err) && s.n == 1 && !std::strcmp(s.k[0].key, "MSC_VGX_PX_NOOP") && s.k[0].v[0] == 1 && s.k[0].v[1] == 0 && s.k[0].windowS == 6.0, "parse default window");
    Check(ab::ParseSpec("MSC_VGX_MIDFLUSH:0,30003:8.5;MSC_VGX_PX_NOOP:1,0", s, err, sizeof err) && s.n == 2 && s.k[0].v[1] == 30003 && s.k[0].windowS == 8.5 && s.k[1].windowS == 6.0, "parse two keys, a window, a long value");
    Check(ab::ParseSpec("MSC_VGX_PX_NOOP,1,0", s, err, sizeof err) && s.n == 1 && s.k[0].v[0] == 1 && s.k[0].v[1] == 0 && s.k[0].windowS == 6.0 &&
          ab::ParseSpec("MSC_VGX_MIDFLUSH,0,30003,8.5;MSC_VGX_PX_NOOP,1,0", s, err, sizeof err) && s.n == 2 && s.k[0].v[1] == 30003 && s.k[0].windowS == 8.5 && s.k[1].windowS == 6.0, "the env-file form: commas, no colon");
    Check(!ab::ParseSpec("", s, err, sizeof err) && !ab::ParseSpec("KEY", s, err, sizeof err) && !ab::ParseSpec("KEY:1", s, err, sizeof err) && !ab::ParseSpec("KEY:1,x", s, err, sizeof err) &&
          !ab::ParseSpec("KEY:1,0:0", s, err, sizeof err) && !ab::ParseSpec("KEY:1,0:6x", s, err, sizeof err) && !ab::ParseSpec(":1,0", s, err, sizeof err) &&
          !ab::ParseSpec("A:1,0;A:0,1", s, err, sizeof err) && !ab::ParseSpec("A:1,0;B:1,0;C:1,0;D:1,0;E:1,0;F:1,0;G:1,0", s, err, sizeof err), "parse refuses bad text, a key twice, seven keys");
    // ---- schedule: two keys, start 10, window 6, settle 1, 4 b windows each: a block is 9 windows = 54 s
    ab::Spec q; ab::ParseSpec("K:1,0:6;L:5,7:6", q, err, sizeof err);
    Check(ab::TotalS(q) == 10 + 54 + 54, "the plan's length: start + two blocks");
    ab::Phase p = ab::PhaseAt(q, 5);
    Check(p.ki == -1 && p.win == -1 && p.arm == 0, "before the start: pre, arm a");
    p = ab::PhaseAt(q, 10.5);  Check(p.ki == 0 && p.win == 0 && p.arm == 0 && !p.settle, "window 0 is arm a, no settle at the first one");
    p = ab::PhaseAt(q, 16.5);  Check(p.ki == 0 && p.win == 1 && p.arm == 1 && p.settle, "window 1 is arm b and starts with the settle");
    p = ab::PhaseAt(q, 17.5);  Check(p.win == 1 && p.arm == 1 && !p.settle, "window 1 measuring");
    p = ab::PhaseAt(q, 22.5);  Check(p.win == 2 && p.arm == 0 && p.settle, "window 2 is arm a again");
    p = ab::PhaseAt(q, 10 + 53.0);  Check(p.ki == 0 && p.wi == 8 && p.arm == 0, "the first block ends with an a window (9 windows: a b a b a b a b a)");
    p = ab::PhaseAt(q, 10 + 54.5);  Check(p.ki == 1 && p.wi == 0 && p.win == 9 && p.arm == 0 && !p.settle, "the second key's block starts with an a window, numbered on (9)");
    p = ab::PhaseAt(q, 10 + 54 + 12.5); Check(p.ki == 1 && p.win == 11 && p.arm == 0, "second key, window 2");
    p = ab::PhaseAt(q, 10 + 54 + 7.5); Check(p.ki == 1 && p.win == 10 && p.arm == 1 && !p.settle, "second key, window 1 is its first b");
    p = ab::PhaseAt(q, 200);   Check(p.ki == 2 && p.arm == 0, "after the plan: done, arm a");
    int bw = 0, aw = 0; for (double t = 10.1; t < 64.0; t += 0.5) { const ab::Phase x = ab::PhaseAt(q, t); if (x.ki == 0 && !x.settle) (x.arm ? bw : aw) += 1; }
    Check(bw > 0 && aw > bw, "more a than b measured time (a b a ... a)");
    char tag[96]; ab::FormatTag(tag, sizeof tag, q, ab::PhaseAt(q, 17.5));
    Check(!std::strcmp(tag, " ab K=0 b w1"), "tag of a measured window");
    ab::FormatTag(tag, sizeof tag, q, ab::PhaseAt(q, 16.5)); Check(!std::strcmp(tag, " ab-settle K=0 b w1"), "tag of a settle");
    ab::FormatTag(tag, sizeof tag, q, ab::PhaseAt(q, 3.0)); Check(!std::strcmp(tag, " ab-pre K=1 a w0"), "tag before the start");
    ab::FormatTag(tag, sizeof tag, q, ab::PhaseAt(q, 10 + 54 + 7.5)); Check(!std::strcmp(tag, " ab L=7 b w10"), "tag of the second key");
    ab::FormatTag(tag, sizeof tag, q, ab::PhaseAt(q, 500)); Check(!std::strcmp(tag, " ab-done"), "tag after the plan");
    // ---- registry audit: nothing is both flippable and refused; every key starts MSC_
    for (const auto& k : ab::kKnobs) {
        Check(ab::RefusalOf(k.key) == nullptr, "a flippable key is not also refused");
        Check(!std::strncmp(k.key, "MSC_", 4) && k.why && *k.why, "key and reason");
    }
    Check(ab::Find("MSC_VGX_PX_NOOP") && !ab::Find("MSC_VGX_SOFTXFORM") && ab::RefusalOf("MSC_VGX_SOFTXFORM") && ab::Find("MSC_VGX_MIDFLUSH")->kind == ab::kApply, "registry lookups");
    // ---- unset: nothing is answered, nothing changes
    unsetenv("MSC_VGX_PX_NOOP"); unsetenv("MSC_VGX_BEPREFETCH"); unsetenv("MSC_VGX_PX_SCISSOR");
    Check(!ab::Active() && !ab::Requested(), "inactive without MSC_AB");
    long dummy = 0; Check(!ab::Lookup("MSC_VGX_PX_NOOP", &dummy) && ab::Get("MSC_VGX_PX_NOOP", 7) == 7, "Lookup answers nothing while inactive");
    Check(ReadNoop() == 1 && ReadPrefetch() == 0 && ReadOther() == 1, "knobs read the defaults (they are now registered)");
    Check(ab::g_regN.load() == 3, "three knob words registered at their first read");
    Check(EnvInt("MSC_VGX_PX_NOOP", 1) == 1, "EnvInt unchanged");
    // ---- start with MSC_AB: PX_NOOP 1/0 then BEPREFETCH 0/3; the knob read BEFORE the start is re-stored, the other key's is not
    setenv("MSC_AB", "MSC_VGX_PX_NOOP,1,0,6;MSC_VGX_BEPREFETCH,0,3,6", 1);   // (the form an env file takes: no colon)
    setenv("MSC_AB_START_S", "10", 1);
    ab::Start();
    Check(ab::Active() && ab::Requested() && ab::g_spec.n == 2 && ab::g_ov.n.load() == 2, "Start reads MSC_AB");
    Check(ReadNoop() == 1 && ReadPrefetch() == 0 && ReadOther() == 1, "arm a: the values the environment gave (the OFF arm is the plain run)");
    // drive: 30 pictures a second; the first picture is t0
    ab::Driver d; uint32_t t = 1000000;
    int flips = 0, cuts = 0, arm = 0, noopB = -1, noopA = -1, pfB = -1, pfA = -1, noopWhilePf = -1;
    for (int i = 0; i < 30 * 130; ++i, t += 33333) {
        const ab::Cut c = d.OnPicture(t);
        if (c.boundary) ++cuts;
        const ab::Phase cp = d.Current();
        if (cp.arm != arm) { ++flips; arm = cp.arm; }
        if (cp.ki == 0 && !cp.settle && cp.win >= 0) { if (cp.arm) noopB = ReadNoop(); else noopA = ReadNoop(); }
        if (cp.ki == 1 && !cp.settle) { if (cp.arm) pfB = ReadPrefetch(); else pfA = ReadPrefetch(); noopWhilePf = ReadNoop(); }
    }
    Check(flips >= 16, "the arm alternated (4 b windows a key: 16 flips for two keys)");
    Check(noopB == 0 && noopA == 1, "key 1: the knob word follows the arm: 0 in b, 1 in a");
    Check(pfB == 3 && pfA == 0, "key 2: the knob word follows the arm: 3 in b, 0 in a");
    Check(noopWhilePf == 1, "a finished key is back at a while the next one is flipped");
    Check(ReadOther() == 1, "a knob that does not read a key is left alone");
    Check(ReadNoop() == 1 && ReadPrefetch() == 0, "after the plan every key is at a");
    Check(cuts > flips, "settle ends are boundaries too: windows are cut at every phase change");
    Check(EnvInt("MSC_VGX_BEPREFETCH", 0) == 0 && EnvInt("MSC_VGX_PX_NOOP", 1) == 1, "EnvInt answers the override (a)");
    // a clock that wraps (32-bit microseconds) does not stall the schedule
    { ab::Driver w; uint32_t u = 0xFFFFFFFFu - 5 * 1000000u; int fl = 0, a0 = 0;
      for (int i = 0; i < 30 * 60; ++i, u += 33333) { (void)w.OnPicture(u); if (w.Current().arm != a0) { ++fl; a0 = w.Current().arm; } }
      Check(fl >= 8, "the schedule runs across the wrap of the 32-bit clock"); }
    // ---- equal arms: a flip never changes any read (the bit-exact OFF arm)
    { int bad = 0; for (int i = 0; i < 6; ++i) { ab::SetValue(0, 1); ab::SetValue(1, 0); bad += ReadNoop() != 1 || ReadPrefetch() != 0 || ReadOther() != 1 || EnvInt("MSC_VGX_PX_NOOP", 1) != 1; }
      Check(bad == 0, "equal arms: every read is what the environment gave"); }
    // ---- kApply: the flip is requested, the render thread makes it at its scene boundary, once, with the value then in force
    ab::g_ov.n.store(2); std::snprintf(ab::g_ov.key[1], sizeof ab::g_ov.key[1], "%s", "MSC_VGX_MIDFLUSH");
    ab::SetApply("MSC_VGX_MIDFLUSH", &Applied);
    unsigned seen = ab::g_applyGen.load();
    ab::RenderTick(seen); Check(g_appliedN == 0, "no request: nothing applied");
    ab::SetValue(1, 30003);
    Check(g_appliedN == 0, "the flip itself applies nothing (it is not the render thread)");
    ab::RenderTick(seen); Check(g_appliedN == 1 && g_applied == 30003, "the render thread applies the requested value at its scene boundary");
    ab::RenderTick(seen); ab::RenderTick(seen); Check(g_appliedN == 1, "once per request");
    ab::SetValue(1, 0); ab::RenderTick(seen); Check(g_appliedN == 2 && g_applied == 0, "and the way back");
    // ---- several kApply keys, one function for two of them: made once per change of value, not on every request
    { g_appliedN = 0; ab::g_applyN.store(0);
      ab::g_ov.n.store(4);
      std::snprintf(ab::g_ov.key[2], sizeof ab::g_ov.key[2], "%s", "MSC_VGX_VGL_FRAGMEMO"); std::snprintf(ab::g_ov.key[3], sizeof ab::g_ov.key[3], "%s", "MSC_VGX_VGL_UNIREUSE");
      ab::g_ov.val[2].store(0); ab::g_ov.val[3].store(0);
      ab::SetApply("MSC_VGX_VGL_FRAGMEMO", &Applied); ab::SetApply("MSC_VGX_VGL_UNIREUSE", &Applied);
      unsigned sn = ab::g_applyGen.load();
      ab::g_applyGen.fetch_add(1); ab::RenderTick(sn);
      Check(g_appliedN == 2, "first tick: each key's value is made once (also the unchanged a)");
      ab::g_applyGen.fetch_add(1); ab::RenderTick(sn);
      Check(g_appliedN == 2, "a request with no value changed makes nothing");
      ab::g_ov.val[2].store(1); ab::g_applyGen.fetch_add(1); ab::RenderTick(sn);
      Check(g_appliedN == 3, "only the key that changed is made");
      Check(ab::Find("MSC_VGX_VGL_MASKKEEP") && ab::Find("MSC_VGX_VGL_MASKKEEP")->kind == ab::kApply && ab::Find("MSC_SHADOW_EVERY")->kind == ab::kCall && !ab::Find("MSC_FX_COMPOSITE") && ab::RefusalOf("MSC_FX_COMPOSITE"), "new registry rows and refusals");
      Check(ab::Get("MSC_SHADOW_EVERY", -1) == -1, "a kCall key not in force answers the caller's default");
    }
    std::printf(g_fail ? "FAILED\n" : "PASS\n");
    return g_fail ? 1 : 0;
}
