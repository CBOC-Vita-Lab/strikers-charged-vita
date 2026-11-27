// Host test of the learned first-use list (vgx_cutlearn.h): text form round trip, damaged lines, duplicates, caps, budget plan.
#include "vgx_cutlearn.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx::cutlearn;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
int main() {
    Limits lim;
    List l;
    TexRec t{0xABCDEF0123ull, 0x80123456u, 128, 64, 14, 40};
    VarRec v{0x1111, 0x2222, 0x1ABu, 0x5Cu, 3};
    CHECK(l.AddTex(t, lim) && !l.AddTex(t, lim));          // a duplicate is refused
    CHECK(l.AddVar(v, lim) && !l.AddVar(v, lim));
    VarRec v2 = v; v2.cls = 2;
    CHECK(l.AddVar(v2, lim));                              // another target class is another variant
    VarRec v3 = v; v3.fsHash = 0x2223;
    CHECK(l.AddVar(v3, lim));
    CHECK(l.HasTex(t) && l.HasVar(v));
    // round trip through the text form
    std::string text = FormatTex(t) + FormatVar(v) + FormatVar(v2) + FormatVar(v3);
    List m;
    CHECK(ParseText(text.data(), text.size(), m, lim) == 4);
    CHECK(m.tex.size() == 1 && m.var.size() == 3 && m.tex[0].hash == t.hash && m.tex[0].addr == t.addr && m.tex[0].kib == 40 && m.var[1].cls == 2 && m.var[0].layout == 0x1AB);
    // damaged and foreign lines are skipped, good ones around them kept; a duplicate line does not count
    std::string bad = "garbage\nT zz\nV 1 2 99999 0 0\nT 1 2 0 0 0 0\n" + FormatTex(t) + FormatTex(t) + "V 0 0 1 1 1\n" + "T 5 6 32 32 14";   // (the last has no kib and no newline)
    List n;
    CHECK(ParseText(bad.data(), bad.size(), n, lim) == 1 && n.tex.size() == 1 && n.var.empty());
    // caps: the list never grows past them, the refusals are counted
    Limits small; small.maxTex = 2; small.maxVar = 1;
    List c;
    CHECK(c.AddTex(TexRec{1, 1, 8, 8, 1, 1}, small) && c.AddTex(TexRec{2, 2, 8, 8, 1, 1}, small) && !c.AddTex(TexRec{3, 3, 8, 8, 1, 1}, small) && c.dropped == 1);
    CHECK(c.AddVar(VarRec{5, 6, 1, 1, 0}, small) && !c.AddVar(VarRec{7, 8, 1, 1, 0}, small) && c.dropped == 2);
    // the plan: bits separate; the KiB budget skips what does not fit and goes on; the variant count is capped
    List p;
    Limits b; b.texKib = 100; b.varPerMatch = 2;
    p.AddTex(TexRec{10, 1, 64, 64, 14, 60}, b); p.AddTex(TexRec{11, 2, 64, 64, 14, 50}, b); p.AddTex(TexRec{12, 3, 32, 32, 14, 30}, b);
    for (uint64_t i = 1; i <= 5; ++i) p.AddVar(VarRec{i, i + 100, 1, 1, 0}, b);
    Plan a = MakePlan(p, kBitTex, b);
    CHECK(a.tex.size() == 2 && a.tex[0] == 0 && a.tex[1] == 2 && a.texKib == 90 && a.texSkipped == 1 && a.var.empty());
    a = MakePlan(p, kBitVar, b);
    CHECK(a.var.size() == 2 && a.varSkipped == 3 && a.tex.empty() && a.texKib == 0);
    a = MakePlan(p, 0, b);
    CHECK(a.tex.empty() && a.var.empty());
    a = MakePlan(p, kBitTex | kBitVar, b);
    CHECK(a.tex.size() == 2 && a.var.size() == 2);
    std::printf("cutlearn test OK\n");
    return 0;
}
