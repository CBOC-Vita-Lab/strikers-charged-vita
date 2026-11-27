// nxboot25 R2-11: vita/vgx_listmerge.h with record type 5 (MSC_VGX_WARM_ORDER's "drawn in a match" marks). Before this the parser refused every list that held one (the package's list does),
// so SeedShaderList printed "WRITE FAILED, 0 bytes now" at every boot. Made-up lists, no game data.   listmerge_marks <outdir>  (writes a.shl b.shl merged.shl for the Python cross-check)
#include "vgx_listmerge.h"   // (-I vita, or a mutant copy: run_listmerge_marks_test.sh)
#include <cstdio>
#include <string>
static int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
using vgx_listmerge::Put32;
static std::string Key(char c, uint32_t n) { return std::string(n, c); }
// a list: vs keys, fs keys (each a run of one letter), then records in the order given
struct B {
    std::string s;
    B(uint32_t vsSize = 4, uint32_t fsSize = 6) { s = "VGXSHL01"; Put32(s, vsSize); Put32(s, fsSize); Put32(s, 7); Put32(s, 0); vsz = vsSize; fsz = fsSize; }
    uint32_t vsz, fsz;
    B& vs(char c) { Put32(s, 1); s += Key(c, vsz); return *this; }
    B& fs(char c) { Put32(s, 2); s += Key(c, fsz); return *this; }
    B& rec(uint32_t t, uint32_t a, uint32_t b) { Put32(s, t); Put32(s, a); Put32(s, b); return *this; }
    B& var(uint32_t v, uint32_t f, uint32_t l, uint32_t bl) { Put32(s, 4); Put32(s, v); Put32(s, f); Put32(s, l); Put32(s, bl); return *this; }
};
static vgx_listmerge::List P(const std::string& s) { return vgx_listmerge::Parse(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
static void Dump(const char* dir, const char* name, const std::string& s) {
    std::string p = std::string(dir) + "/" + name;
    if (FILE* f = std::fopen(p.c_str(), "wb")) { std::fwrite(s.data(), 1, s.size(), f); std::fclose(f); }
}
int main(int argc, char** argv) {
    // package's list: vs A B C, fs a b, programs (A,a) (B,a) (C,b), a variant, marks on (B,a) (C,b) (and a repeat of (B,a): the reader accepts it)
    B pk;
    pk.vs('A').vs('B').vs('C').fs('a').fs('b').rec(3, 0, 0).rec(3, 1, 0).rec(3, 2, 1).var(1, 0, 5, 9).rec(5, 1, 0).rec(5, 2, 1).rec(5, 1, 0);
    const auto p = P(pk.s);
    CHECK(!vgx_listmerge::Parse(reinterpret_cast<const uint8_t*>(pk.s.data()), pk.s.size(), false).ok);   // MSC_SEED_MARKS=0: the old parser
    CHECK(p.ok);                                   // R2-11: the bug was ok = false here
    CHECK(p.marks.size() == 3 && p.prog.size() == 3 && p.var.size() == 1 && p.vs.size() == 3 && p.fs.size() == 2);
    CHECK(p.marks.size() >= 2 && p.marks[0] == std::make_pair(1u, 0u) && p.marks[1] == std::make_pair(2u, 1u));
    // the console's own list: other order of keys, one shared program, its own marks, one new key of each kind
    B cn;
    cn.vs('C').vs('D').vs('B').fs('b').fs('c').fs('a').rec(3, 0, 0).rec(3, 1, 1).rec(3, 2, 2).rec(5, 0, 0).rec(5, 1, 1).rec(5, 2, 2).var(0, 0, 5, 9).var(1, 1, 2, 3);
    const auto c = P(cn.s);
    CHECK(c.ok && c.marks.size() == 3);
    unsigned added = 0;
    const std::string m = vgx_listmerge::Merge(p, c, &added);
    const auto mm = P(m);
    CHECK(mm.ok);
    CHECK(mm.vs.size() == 4 && mm.fs.size() == 3);                       // A B C D / a b c : the package's order first, then the console's new keys
    CHECK(mm.vs.size() == 4 && mm.fs.size() == 3 && mm.vs[0] == Key('A', 4) && mm.vs[3] == Key('D', 4) && mm.fs[2] == Key('c', 6));
    // programs: package (A,a) (B,a) (C,b), console adds (D,c) only ((C,b) and (B,a) are shared): ids in the merged lists
    CHECK(mm.prog.size() == 4 && mm.prog.size() > 3 && mm.prog[3] == std::make_pair(3u, 2u));
    // marks: (B,a) (C,b) from the package; the console's (C,b) (D,c) (B,a): one each, the union is (B,a) (C,b) (D,c), written in the programs' order, remapped to the merged ids
    CHECK(mm.marks.size() == 3);
    CHECK(mm.marks.size() == 3 && mm.marks[0] == std::make_pair(1u, 0u) && mm.marks[1] == std::make_pair(2u, 1u) && mm.marks[2] == std::make_pair(3u, 2u));
    CHECK(added == 5);                                                  // vs D, fs c, program (D,c), variants (C,b,5,9) and (D,c,2,3): the marks are not counted as records
    // exact bytes of the expected result, built by hand
    B ex;
    ex.vs('A').vs('B').vs('C').vs('D').fs('a').fs('b').fs('c').rec(3, 0, 0).rec(3, 1, 0).rec(3, 2, 1).rec(3, 3, 2).var(1, 0, 5, 9).var(2, 1, 5, 9).var(3, 2, 2, 3).rec(5, 1, 0).rec(5, 2, 1).rec(5, 3, 2);
    CHECK(m == ex.s);
    // a merged list is stable: merging it with either input again changes nothing
    CHECK(vgx_listmerge::Merge(mm, p) == m && vgx_listmerge::Merge(mm, c) == m);
    // a list without marks, and a mark whose program is not listed (dropped, as tools/seed/shl.py merge does)
    B nm; nm.vs('A').fs('a').rec(3, 0, 0);
    CHECK(P(nm.s).ok && P(nm.s).marks.empty() && vgx_listmerge::Merge(P(nm.s), P(nm.s)) == nm.s);
    B orphan; orphan.vs('A').vs('B').fs('a').rec(3, 0, 0).rec(5, 1, 0);
    CHECK(P(orphan.s).ok && P(orphan.s).marks.size() == 1);
    B orphanOut; orphanOut.vs('A').vs('B').fs('a').rec(3, 0, 0);
    CHECK(vgx_listmerge::Merge(P(orphan.s), P(orphan.s)) == orphanOut.s);
    // damaged marks: an id out of range, a cut-off record, and a type nobody knows
    B bad1; bad1.vs('A').fs('a').rec(3, 0, 0).rec(5, 1, 0);       CHECK(!P(bad1.s).ok);
    B bad2; bad2.vs('A').fs('a').rec(3, 0, 0).rec(5, 0, 1);       CHECK(!P(bad2.s).ok);
    B bad3; bad3.vs('A').fs('a').rec(3, 0, 0).rec(5, 0, 0); bad3.s.resize(bad3.s.size() - 3); CHECK(!P(bad3.s).ok);
    B bad4; bad4.vs('A').fs('a').rec(6, 0, 0);                    CHECK(!P(bad4.s).ok);
    // a damaged list is not merged: the result is the first alone (SeedShaderList reports it, and no longer calls it a failed write)
    CHECK(vgx_listmerge::Merge(p, P(bad1.s)) == vgx_listmerge::Merge(p, vgx_listmerge::List()));
    if (argc > 1) { Dump(argv[1], "a.shl", pk.s); Dump(argv[1], "b.shl", cn.s); Dump(argv[1], "merged.shl", m); Dump(argv[1], "ex.shl", ex.s); }
    std::printf("listmerge marks test: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
