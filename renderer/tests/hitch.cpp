// MSC_HITCH_MS (vgx_hitch.h): a long gap is charged to the frame before it, once, to one cause.
#include "../src/vgx_hitch.h"
#include <cstdio>
#include <cstring>
using namespace vgx::hitch;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    { Frame f; f.be = 5; CHECK(Classify(f, 60) == CUpstream); }
    { Frame f; f.compiles = 3; f.packHits = 3; f.be = 5; CHECK(Classify(f, 60) == CUpstream); }   // pack hits, no time: not shader
    { Frame f; f.compiles = 3; f.packHits = 2; CHECK(Classify(f, 60) == CShader); }
    { Frame f; f.shaderMs = 15; CHECK(Classify(f, 60) == CShader); }
    { Frame f; f.made = 1; f.texKib = 500; f.syncReads = 1; CHECK(Classify(f, 60) == CTarget); }  // order
    { Frame f; f.texKib = 64; f.syncReads = 1; CHECK(Classify(f, 60) == CTexture); }
    { Frame f; f.texKib = 63; f.syncReads = 1; CHECK(Classify(f, 60) == CWait); }
    { Frame f; f.be = 40; f.swap = 30; CHECK(Classify(f, 60) == CDisplay); }
    { Frame f; f.be = 40; f.swap = 10; CHECK(Classify(f, 60) == CBackend); }
    {
        Ledger l; l.limitMs = 45; Cause c; Frame b;
        Frame a; a.frame = 1; a.made = 2;
        CHECK(!l.Note(a, 500, &c, &b));     // the first frame has no frame before it
        Frame z; z.frame = 2;
        CHECK(l.Note(z, 80, &c, &b));       // the gap of frame 2 is charged to frame 1
        CHECK(b.frame == 1 && c == CTarget);
        Frame y; y.frame = 3;
        CHECK(!l.Note(y, 44.9f, &c, &b));   // under the limit
        CHECK(l.Note(y, 120, &c, &b) && b.frame == 3 && c == CUpstream);
        char s[300];
        l.Summary(s, sizeof s);
        CHECK(std::strstr(s, "2 at 45 ms or more (1 at 100+; 0 more were only the held cadence), longest 120.0 ms") && std::strstr(s, "target 1") && std::strstr(s, "upstream 1"));
        l.Summary(s, sizeof s);
        CHECK(std::strstr(s, "0 frames: 0 at 45"));  // a summary starts the window again
    }
    { Ledger off; Cause c; Frame b, f; off.Note(f, 0, &c, &b); CHECK(!off.Note(f, 9999, &c, &b)); CHECK(!off.WindowDone()); }
    { Ledger l; l.limitMs = 1; l.lineCap = 2; CHECK(l.WantLine() && l.WantLine() && !l.WantLine()); }
    { Ledger l; l.limitMs = 1; l.lineCap = 1; l.playCap = 2; CHECK(l.WantLine() && !l.WantLine()); CHECK(l.WantLine(true) && l.WantLine(true) && !l.WantLine(true)); }  // boot lines used up: live play keeps its own
    {   // wave 11: held scenes have their own budget, a quarter of it before the first live-play picture, all of it after
        Ledger l; l.limitMs = 1; l.lineCap = 3; l.playCap = 5; l.heldCap = 8;
        CHECK(l.WantLineClass(0) && l.WantLineClass(0) && l.WantLineClass(0) && !l.WantLineClass(0));   // menus and loading used their three
        CHECK(l.WantLineClass(2) && l.WantLineClass(2) && !l.WantLineClass(2));                        // the walk-out: 8/4 = 2
        CHECK(l.WantLineClass(1) && l.playLines == 1);                                                  // kick-off
        for (int i = 0; i < 6; ++i) CHECK(l.WantLineClass(2));                                          // goal cuts and replays find the rest
        CHECK(!l.WantLineClass(2));
    }
    { Ledger l; l.limitMs = 45; char s[40]; l.count[0] = 1; const size_t n = l.Summary(s, sizeof s); CHECK(n < sizeof s); }  // a short buffer stays in bounds
    {   // wave 9: the cadence in force. A held 30 (period 33.4) with MSC_HITCH_MS=25: a 33.4 ms gap is not a hitch, a 60 ms gap is; at 60 (16.7) the 25 ms limit stands
        Ledger l; l.limitMs = 25; Cause c; Frame b;
        Frame f; f.frame = 1199;   // frame % 600 = 599: bin 9
        l.Note(f, 33.4f, &c, &b, 33.4f);
        Frame g; g.frame = 1200;
        CHECK(!l.Note(g, 33.4f, &c, &b, 33.4f));
        CHECK(!l.Note(g, 48.0f, &c, &b, 33.4f));
        CHECK(l.Note(g, 60.0f, &c, &b, 33.4f) && b.frame == 1200);
        Frame h; h.frame = 1201;
        CHECK(l.Note(h, 26.0f, &c, &b, 16.7f));          // at 60 the limit is MSC_HITCH_MS
        CHECK(l.Note(h, 26.0f, &c, &b, 0.0f));           // period unknown: as before
        Ledger k; k.limitMs = 25; Frame p; p.frame = 599; k.Note(p, 0, &c, &b);
        Frame q; q.frame = 600; CHECK(k.Note(q, 90.0f, &c, &b, 33.4f));   // the hitch is charged to frame 599: bin 9
        char s[400];
        k.Summary(s, sizeof s);
        CHECK(std::strstr(s, "by frame % 600 in 60-frame bins: 0 0 0 0 0 0 0 0 0 1"));
        char t2[400];
        l.Summary(t2, sizeof t2);
        CHECK(std::strstr(t2, "2 more were only the held cadence"));
    }
    std::printf(fails ? "hitch: %d FAILED\n" : "hitch: passed\n", fails);
    return fails != 0;
}
