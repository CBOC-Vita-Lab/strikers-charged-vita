// The stall notice (1.2, vita_notice.h): the words by quiet time, and the banner drawn into a frame buffer. Host test.
#include "vita_notice.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_failed = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    using namespace VitaNotice;
    // the levels: nothing before the delay, the calm message until three times it, then what to do
    CHECK(Level(0, 20) == 0 && Level(15, 20) == 0 && Level(20, 20) == 1 && Level(55, 20) == 1 && Level(60, 20) == 2 && Level(300, 20) == 2);
    CHECK(Level(100, 0) == 0 && Level(100, -5) == 0);   // 0 = off
    Text t;
    Compose(0, 10, t);
    CHECK(t.count == 0);
    Compose(1, 25, t);
    CHECK(t.count == 2 && std::strstr(t.line[0], "25 seconds") && !std::strstr(t.line[1], "close the game"));
    Compose(2, 65, t);
    CHECK(t.count == 3 && std::strstr(t.line[0], "65 seconds") && std::strstr(t.line[2], "close the game"));
    // every line fits the screen (the banner cuts a longer one, but the words are meant to be read whole)
    for (int level = 1; level <= 2; ++level) {
        Compose(level, 9999, t);
        for (int i = 0; i < t.count; ++i) CHECK(static_cast<int>(std::strlen(t.line[i])) <= Columns(960));
    }

    // the banner: a 960 x 544 buffer of a known colour; the band changes, the rest does not
    const int W = 960, H = 544, pitch = 1024;
    std::vector<uint32_t> fb(static_cast<size_t>(pitch) * H, 0xFF123456u);
    Compose(2, 70, t);
    const char* lines[kMaxLines] = {t.line[0], t.line[1], t.line[2]};
    CHECK(DrawBanner(fb.data(), pitch, W, H, lines, t.count) == 3);
    const Band band = BandFor(W, H, 3);
    CHECK(band.y0 > 0 && band.y1 < H && band.x0 > 0 && band.x1 < W);
    size_t outsideChanged = 0, insideChanged = 0, white = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < pitch; ++x) {
            const uint32_t v = fb[static_cast<size_t>(y) * pitch + x];
            const bool inBand = x >= band.x0 && x < band.x1 && y >= band.y0 && y < band.y1;
            if (v != 0xFF123456u) (inBand ? insideChanged : outsideChanged)++;
            if (v == Pack(255, 255, 255)) ++white;
        }
    CHECK(outsideChanged == 0);                                   // nothing outside the band (the padding columns of the pitch included)
    CHECK(insideChanged == static_cast<size_t>(band.x1 - band.x0) * (band.y1 - band.y0));   // the band is opaque: no pixel of the old picture shows through
    CHECK(white > 200);                                           // the first line is in white
    // the same text twice gives the same pixels (no state)
    std::vector<uint32_t> fb2(static_cast<size_t>(pitch) * H, 0xFF123456u);
    DrawBanner(fb2.data(), pitch, W, H, lines, t.count);
    CHECK(fb2 == fb);
    // fewer lines: a smaller band; no lines or a bad buffer: nothing drawn
    CHECK(BandFor(W, H, 2).y1 - BandFor(W, H, 2).y0 < band.y1 - band.y0);
    std::vector<uint32_t> fb3(static_cast<size_t>(pitch) * H, 7u);
    CHECK(DrawBanner(fb3.data(), pitch, W, H, lines, 0) == 0);
    const char* empty[1] = {""};
    CHECK(DrawBanner(fb3.data(), pitch, W, H, empty, 1) == 0);
    CHECK(DrawBanner(nullptr, pitch, W, H, lines, 3) == 0);
    CHECK(DrawBanner(fb3.data(), 100, W, H, lines, 3) == 0);      // a pitch smaller than the width
    CHECK(DrawBanner(fb3.data(), pitch, 100, 100, lines, 3) == 0); // a buffer too small for the text
    bool untouched = true;
    for (uint32_t v : fb3) untouched &= v == 7u;
    CHECK(untouched);
    // a smaller screen (640 x 448, the game's own size) is drawn inside its own bounds
    std::vector<uint32_t> small(static_cast<size_t>(640) * 448, 0xFF000000u);
    CHECK(DrawBanner(small.data(), 640, 640, 448, lines, 3) == 3);
    const Band sb = BandFor(640, 448, 3);
    CHECK(sb.x0 >= 0 && sb.x1 <= 640 && sb.y0 >= 0 && sb.y1 <= 448);
    // text with characters the font has not got is drawn as '?', not read past the table
    const char* odd[1] = {"caf\xC3\xA9 \x01 ok"};
    std::vector<uint32_t> fb4(static_cast<size_t>(pitch) * H, 0xFF000000u);
    CHECK(DrawBanner(fb4.data(), pitch, W, H, odd, 1) == 1);

    // 1.3 MSC_VITA_STALL_GUARD: no banner for anything short of a real hang, and no quiet time counted across a wake-up
    CHECK(NoticeDelay(20, true) == 20 && NoticeDelay(5, true) == kFloorSeconds && NoticeDelay(1, true) == kFloorSeconds && NoticeDelay(0, true) == 0 && NoticeDelay(-1, true) == 0);
    CHECK(NoticeDelay(5, false) == 5);                         // the guard off is the 1.2 behaviour
    {
        // a game that presents every look is never quiet, whatever the delay; one sub-second or several-second hitch changes the frame number at the next look
        Quiet q;
        for (uint64_t f = 1; f < 200; ++f) CHECK(q.Look(f * 3, 0, false, 5, true) == 0);
        // a hang: the seconds add up; the banner level follows (20 s default)
        int seconds = 0;
        for (int look = 0; look < 14; ++look) seconds = q.Look(600, 0, false, 5, true);
        CHECK(seconds == 65 && Level(seconds, NoticeDelay(20, true)) == 2 && Level(15, NoticeDelay(20, true)) == 0 && Level(5, NoticeDelay(5, true)) == 0 && Level(15, NoticeDelay(5, true)) == 1);
        // the next picture ends it
        CHECK(q.Look(601, 0, false, 5, true) == 0 && q.reset);
    }
    {
        // a wake-up of the console in the middle of a quiet stretch: the count starts again and two looks do not count
        Quiet q;
        q.Look(100, 0, false, 5, true);
        CHECK(q.Look(100, 0, false, 5, true) == 5 && q.Look(100, 0, false, 5, true) == 10);
        CHECK(q.Look(100, 1, false, 5, true) == 0 && q.reset);           // woken: reset
        CHECK(q.Look(100, 1, false, 5, true) == 0 && q.Look(100, 1, false, 5, true) == 0);   // grace
        CHECK(q.Look(100, 1, false, 5, true) == 5);                       // counting again from here
        // the same sequence with the guard off adds the wake-up time as 1.2 did
        Quiet o;
        o.Look(100, 0, false, 5, false);
        o.Look(100, 0, false, 5, false);
        o.Look(100, 0, false, 5, false);
        CHECK(o.Look(100, 1, false, 5, false) == 15);
        // Settings stops the game on purpose: never counted
        Quiet st;
        st.Look(7, 0, false, 5, true);
        CHECK(st.Look(7, 0, true, 5, true) == 0 && st.Look(7, 0, true, 5, true) == 0 && st.Look(7, 0, false, 5, true) == 5);
    }
    std::printf("stall notice: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
