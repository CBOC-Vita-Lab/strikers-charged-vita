// Host test of np/stillback.h: the policy of MSC_MENU_STILL and MSC_PAUSE_STILL.
#include "np/stillback.h"
#include <cstdio>

static int gFailures;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); gFailures++; } } while (0)

namespace {
struct Rig
{
    NpStillState s;
    NpStillIn in;
    Rig() : s(NpStillState())
    {
        in.active = 1; in.stamp = 7; in.scale = 4096; in.done = 0; in.failed = 0; in.room = 1;
    }
    int step(int settle = 3, int refresh = 0) { return NpStillStep(s, in, settle, refresh); }
    // to the first copy, the frames before it live
    void toCopy(int settle = 3)
    {
        int guard = 0;
        int a;
        while ((a = step(settle)) == NPSTILL_LIVE && guard++ < 1000) {}
        CHECK(a == NPSTILL_CAPTURE);
    }
    void toHeld()
    {
        toCopy();
        in.done++;
        CHECK(step() == NPSTILL_STILL);
    }
};
}

int main()
{
    {   // settles first: exactly settle frames of the same stamp after the first
        Rig r;
        CHECK(r.step() == NPSTILL_LIVE);
        CHECK(r.step() == NPSTILL_LIVE);
        CHECK(r.step() == NPSTILL_LIVE);
        CHECK(r.step() == NPSTILL_CAPTURE);
    }
    {   // a moving camera never captures
        Rig r;
        for (int i = 0; i < 100; i++) { r.in.stamp++; CHECK(r.step() == NPSTILL_LIVE); }
    }
    {   // live until the renderer has the copy, then held and no second copy
        Rig r;
        r.toCopy();
        for (int i = 0; i < 5; i++) CHECK(r.step() == NPSTILL_LIVE);
        r.in.done++;
        for (int i = 0; i < 500; i++) CHECK(r.step() == NPSTILL_STILL);
    }
    {   // a changed stamp shows on the same frame, and a new copy comes after it settles
        Rig r;
        r.toHeld();
        r.in.stamp = 8;
        CHECK(r.step() == NPSTILL_LIVE);
        r.toCopy();
    }
    {   // leaving and coming back
        Rig r;
        r.toHeld();
        r.in.active = 0;
        CHECK(r.step() == NPSTILL_LIVE);
        r.in.active = 1;
        CHECK(r.step() == NPSTILL_LIVE);
        r.toCopy();
    }
    {   // a new picture size: one copy, the old picture goes on, and at most one copy per kNpStillResizeFrames
        Rig r;
        r.toHeld();
        for (int i = 0; i < 100; i++) r.step();
        r.in.scale = 3000;
        CHECK(r.step() == NPSTILL_CAPTURE);
        CHECK(r.step() == NPSTILL_STILL);
        r.in.scale = 4096;
        for (int i = 0; i < kNpStillResizeFrames - 2; i++) CHECK(r.step() == NPSTILL_STILL);
        CHECK(r.step() == NPSTILL_CAPTURE);
        CHECK(r.step() == NPSTILL_STILL);
    }
    {   // the size changed while the copy was on its way
        Rig r;
        r.toCopy();
        r.in.scale = 3000;
        r.in.done++;
        CHECK(r.step() == NPSTILL_CAPTURE);
        CHECK(r.step() == NPSTILL_LIVE);
        r.in.done++;
        CHECK(r.step() == NPSTILL_STILL);
    }
    {   // the renderer could not make the copy: never held, tries again later, gives up after three
        Rig r;
        int copies = 0, held = 0;
        for (int i = 0; i < 10 * kNpStillRetryFrames; i++)
        {
            const int a = r.step();
            if (a == NPSTILL_CAPTURE) { copies++; }
            else if (a == NPSTILL_STILL) { held++; }
            else if (r.s.phase == NPSTILL_WAIT) { r.in.failed++; }
        }
        CHECK(copies == kNpStillMaxFails);
        CHECK(held == 0);
    }
    {   // no word at all (a discarded frame): the same
        Rig r;
        r.toCopy();
        for (int i = 0; i < kNpStillAckFrames; i++) CHECK(r.step() == NPSTILL_LIVE);
        CHECK(r.step() == NPSTILL_LIVE);
        CHECK(r.s.fails == 1);
        CHECK(r.s.phase == NPSTILL_WATCH);
    }
    {   // a held texture lost: live on that frame, and a fresh copy after the wait
        Rig r;
        r.toHeld();
        r.in.failed++;
        CHECK(r.step() == NPSTILL_LIVE);
        for (int i = 0; i < kNpStillRetryFrames - 1; i++) CHECK(r.step() == NPSTILL_LIVE);
        CHECK(r.step() == NPSTILL_CAPTURE);
    }
    {   // no room: no copy
        Rig r;
        r.in.room = 0;
        for (int i = 0; i < 100; i++) CHECK(r.step() == NPSTILL_LIVE);
        r.in.room = 1;
        CHECK(r.step() == NPSTILL_CAPTURE);
    }
    {   // MSC_STILL_REFRESH: every refresh-th held frame is a copy again, the others stand on it; refresh 0 or 1 never copies again
        for (int refresh = 0; refresh <= 4; ++refresh)
        {
            Rig r;
            r.toCopy();
            r.in.done++;
            int act = r.s.phase == NPSTILL_WAIT ? r.s.phase : 0;
            (void)act;
            CHECK(r.step(3, refresh) == NPSTILL_STILL);   // the frame that hears of the first copy
            int copies = 0, stills = 0;
            for (int i = 0; i < 120; i++)
            {
                const int a = r.step(3, refresh);
                copies += a == NPSTILL_CAPTURE;
                stills += a == NPSTILL_STILL;
                CHECK(a != NPSTILL_LIVE);
            }
            if (refresh <= 1) CHECK(copies == 0);
            else CHECK(copies == 120 / refresh && stills == 120 - 120 / refresh);
        }
        Rig r;
        r.toHeld();
        for (int i = 0; i < 5; i++) r.step(3, 2);
        r.in.stamp = 9;                                   // a change is live on that frame whatever the refresh says
        CHECK(r.step(3, 2) == NPSTILL_LIVE);
        Rig f;
        f.toHeld();
        f.step(3, 2);
        f.in.failed++;                                     // a refresh copy that fails ends the hold
        CHECK(f.step(3, 2) == NPSTILL_LIVE);
        CHECK(f.s.fails == 1);
    }
    {   // MSC_STILL_TOL: noise is not a move, a quarter pixel and more is
        float v[16], p[16], v2[16], p2[16];
        for (int i = 0; i < 16; i++) { v[i] = v2[i] = 0.3f * i; p[i] = p2[i] = 1.0f + i; }
        CHECK(!NpStillCameraMoved(v, v2, p, p2));
        v2[12] += 1e-5f; v2[0] += 1e-6f; v2[5] -= 1e-5f;
        CHECK(!NpStillCameraMoved(v, v2, p, p2));
        v2[12] = v[12] + 2e-3f;
        CHECK(NpStillCameraMoved(v, v2, p, p2));
        v2[12] = v[12]; v2[6] += 5e-4f;
        CHECK(NpStillCameraMoved(v, v2, p, p2));
        v2[6] = v[6]; p2[5] += 2e-4f;
        CHECK(NpStillCameraMoved(v, v2, p, p2));
        p2[5] = p[5]; v2[3] = 0.0f / 0.0f;
        CHECK(NpStillCameraMoved(v, v2, p, p2));
    }
    CHECK(NpStillParse("1") == 1);
    CHECK(NpStillParse("0") == 0);
    CHECK(NpStillParse("") == 0);
    CHECK(NpStillParse(0) == 0);
    CHECK(NpStillHash(1, "a", 1) != NpStillHash(1, "b", 1));
    if (gFailures == 0) std::printf("stillback: all passed\n");
    return gFailures != 0;
}
