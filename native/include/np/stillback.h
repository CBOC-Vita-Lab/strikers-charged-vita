/* np/stillback.h: the 3D picture behind a menu drawn from one copy of itself (1.3), default OFF, render-only.
 *
 *   MSC_MENU_STILL=0|1   0 (default). 1: in the front end, once the camera and the screen's world have stood still, the world is copied once and the copy stands in for it.
 *   MSC_PAUSE_STILL=0|1  0 (default). 1: the same behind the pause menu of a local match (Galaxy's PETARI_PAUSE_FREEZE). Never in a game of more than one machine.
 *   MSC_STILL_LOG=1      one line per change, and every 300 frames the frames of each kind.
 *
 * Nothing the game does is skipped. The game still updates, poses and attaches everything; only the send of the world's views is left out, as the game's own layer toggles
 * (eCLV_*Enabled) do. So no call that draws a random number, FEImpostorCharacter::Render among them, runs more or less often, and the simulation cannot see the difference.
 *
 * The policy is here, without game headers, so that a host test can compile it (native/tools/stillback). Game/Render/StillBackdrop.cpp feeds it and acts on its answer.
 *   LIVE     the frame as the game drew it.
 *   CAPTURE  the frame as the game drew it, and the copy is made. The picture stays live until the renderer has said that the copy exists (done), so a copy that could not be
 *            made (failed: no memory for it, or nothing came back) never shows.
 *   STILL    the world's views are not sent, the copy is drawn in their place.
 * A held copy goes as soon as the stamp changes (that same frame is LIVE, so a changed camera never shows a stale picture) or the renderer says it is gone (failed).
 * A new picture size is one CAPTURE frame while the old copy goes on standing in (it is sampled by normalised coordinates): no run of live frames that could make the dynamic
 * resolution step back and start it again.
 */
#ifndef NP_STILLBACK_H
#define NP_STILLBACK_H

enum NpStillAct
{
    NPSTILL_LIVE = 0,
    NPSTILL_CAPTURE = 1,
    NPSTILL_STILL = 2,
};

enum NpStillPhase
{
    NPSTILL_WATCH = 0,   // waiting for a steady stamp
    NPSTILL_WAIT = 1,    // the copy is on its way to the renderer
    NPSTILL_HELD = 2,    // the copy stands in
};

static const int kNpStillAckFrames = 30;    // no word from the renderer in this many frames: as a failure (a discarded frame, say)
static const int kNpStillRetryFrames = 300; // after a failure, before the next attempt
static const int kNpStillResizeFrames = 60; // at least this far apart: copies for a new picture size
static const int kNpStillMaxFails = 3;      // then no more attempts until active goes off

struct NpStillIn
{
    int active;         // this is a screen that may be held (0: everything starts again)
    unsigned stamp;     // what the picture depends on: camera, world, the set of characters
    int scale;          // the picture size, any number that changes with it
    unsigned done;      // the renderer's counter of finished copies
    unsigned failed;    // the renderer's counter of copies not made and of held copies lost
    int room;           // the renderer has the memory for a copy
};

struct NpStillState
{
    int phase;
    int seen;
    unsigned stamp;
    int calm;           // frames with the same stamp
    int scale;
    unsigned doneBase, failBase;
    int wait;
    int cool;
    int fails;
    int held;           // frames held since the last copy (MSC_STILL_REFRESH)
};

static inline int NpStillFail(NpStillState& s, unsigned failedNow)
{
    s.failBase = failedNow;
    s.fails++;
    s.cool = kNpStillRetryFrames;
    s.phase = NPSTILL_WATCH;
    s.calm = 0;
    return NPSTILL_LIVE;
}

// settle: frames of an unchanged stamp before the first copy
// refresh (MSC_STILL_REFRESH, 0 = never): while held, every refresh-th frame is a CAPTURE again: the world is drawn live that frame and copied over the held picture, so what moves in the world
// (machinery, flags) goes on at 1/refresh of the picture rate instead of stopping. The copy is made in the order of the frame, so the next held frame draws the new one; a copy that
// fails ends the hold as any other.
static inline int NpStillStep(NpStillState& s, const NpStillIn& in, int settle, int refresh = 0)
{
    if (!in.active)
    {
        const NpStillState clean = NpStillState();
        s = clean;
        return NPSTILL_LIVE;
    }
    if (s.cool > 0)
    {
        s.cool--;
    }
    const bool same = s.seen && in.stamp == s.stamp;
    s.seen = 1;
    s.stamp = in.stamp;
    s.calm = same ? s.calm + 1 : 0;

    switch (s.phase)
    {
    case NPSTILL_HELD:
        if (in.failed != s.failBase)
        {
            return NpStillFail(s, in.failed);
        }
        if (!same)
        {
            s.phase = NPSTILL_WATCH;
            return NPSTILL_LIVE;
        }
        if (in.scale != s.scale && s.cool == 0)
        {
            s.scale = in.scale;
            s.cool = kNpStillResizeFrames;
            s.held = 0;
            return NPSTILL_CAPTURE;
        }
        if (refresh > 1 && ++s.held >= refresh)
        {
            s.held = 0;
            return NPSTILL_CAPTURE;
        }
        return NPSTILL_STILL;
    case NPSTILL_WAIT:
        if (!same)
        {
            s.phase = NPSTILL_WATCH;
            return NPSTILL_LIVE;
        }
        if (in.failed != s.failBase)
        {
            return NpStillFail(s, in.failed);
        }
        if (in.done != s.doneBase)
        {
            if (in.scale != s.scale)   // the size changed under the copy
            {
                s.scale = in.scale;
                s.doneBase = in.done;
                s.wait = 0;
                return NPSTILL_CAPTURE;
            }
            s.phase = NPSTILL_HELD;
            s.held = 0;
            return NPSTILL_STILL;
        }
        if (++s.wait > kNpStillAckFrames)
        {
            return NpStillFail(s, in.failed);
        }
        return NPSTILL_LIVE;
    default:
        if (s.calm >= settle && s.cool == 0 && s.fails < kNpStillMaxFails && in.room)
        {
            s.phase = NPSTILL_WAIT;
            s.scale = in.scale;
            s.doneBase = in.done;
            s.failBase = in.failed;
            s.wait = 0;
            return NPSTILL_CAPTURE;
        }
        return NPSTILL_LIVE;
    }
}

// MSC_STILL_TOL (wave 7): the camera's matrices are recomputed every frame and differ in the last bits of a float (1e-7 on the rotation, up to 1e-5 on a translation of 100 world units; measured on
// a settled menu, MSC_STILL_DIAG), so an exact comparison never settled and the held copy never engaged. The camera has moved when a view or projection element is further from the
// reference than a quarter of a pixel of 960 at a focal length of about 1000 pixels: rotation and the matrix's other elements 2.5e-4, translation 1.2e-3 (a surface 5 units away or more),
// projection 1e-4. The reference is the matrix at the last move, so a slow real drift still ends the hold (a quarter pixel from where it began), it just is not the noise.
static const float kNpStillRot = 2.5e-4f, kNpStillTrans = 1.2e-3f, kNpStillProj = 1e-4f;
static inline int NpStillCameraMoved(const float* refView, const float* view, const float* refProj, const float* proj)
{
    for (int i = 0; i < 16; i++)
    {
        float d = view[i] - refView[i];
        d = d < 0 ? -d : d;
        const float lim = (i >= 12 && i <= 14) ? kNpStillTrans : kNpStillRot;
        if (!(d <= lim)) return 1;   // (a NaN is a move)
        float e = proj[i] - refProj[i];
        e = e < 0 ? -e : e;
        if (!(e <= kNpStillProj)) return 1;
    }
    return 0;
}

// FNV-1a over the bytes of something the picture depends on
static inline unsigned NpStillHash(unsigned h, const void* data, unsigned bytes)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (unsigned i = 0; i < bytes; i++)
    {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static inline int NpStillParse(const char* e)
{
    return e != 0 && e[0] == '1' && e[1] == 0;
}

#endif // NP_STILLBACK_H
