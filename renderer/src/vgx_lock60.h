// MSC_LOCK60: the picture-size governor of the LOCKED 60 mode (header-only so a host test and the trace model run the shipped code).
//
// The ladder of vgx_backend_gl.cpp (DynamicResolution) was built for a game that starts every scene at 30 and may be held there; it discovers its picture size by missing
// frames, it moves one step per 20 presents, a step may make a render target, and its prediction treats three quarters of a frame's GPU time as following the pixels. This is the
// policy for a game that is to hold 60: the same rungs, decided differently.
//   - Scene presets (PresetFor): a scene class starts at the rung the budget table says fits (docs/TECHNICAL.md "LOCKED 60"), not at the game's own size.
//   - Only rungs that are READY are used (a kept render target of their size, made ahead outside play): a step never allocates in play. A rung that is not ready is skipped; none
//     ready below the current one means "no room", which the frame-rate controller reads as the cue for its relief (np_fps_ctl.h Config::Lock60).
//   - Down at once: a frame over the budget (lateAt) or two of the last three over 90 % asks for a step; the size of the step is what THIS frame's time says it needs to fit in
//     82 %, at most maxRungs. The cost model says only `pixShare` of a frame follows the pixels (a draw-bound game: 0.30 by the budget table; the old ladder's 0.75 predicted
//     savings that never came and made the next step a second try).
//   - Up only on proof: proofFrames measured frames in a row, each projected at the next rung under upFit of the budget (a tenth of them may be over, none over upHard), after a gap
//     from the last change, while the effects governor holds nothing back. A step up is on probation for probeFrames frames: one frame over probeAt of the budget takes it back at once
//     and blocks the next try for 2 s, doubled for each failed try (at most 60 s); 30 s of calm forgive one doubling. So a rung that does not hold is tried once in a while, and
//     never twice in a row, and a load that sits between two rungs does not flap.
// Render only: the size of the picture reaches nothing the simulation or the network reads.
#pragma once

#include <algorithm>
#include <cmath>

namespace vgx {
namespace lock60 {

enum SceneKind { kMenu = 0, kPlay = 1, kReplay = 2, kCut = 3, kMega = 4, kHeavy = 5, kSceneKinds = 6 };

// Per side, of the game's own size (the ladder's own scale values); the budget table's choices. `start`: where the scene begins; `floor`: the deepest rung the governor may use.
struct Preset { float start, floor; };
inline Preset PresetFor(int kind) {
    switch (kind) {
        case kPlay:   return {0.93f, 0.70f};   // open play: 600x416 holds 16.7 ms at the table's middle figures; 448x320 is the floor
        case kHeavy:  return {0.78f, 0.62f};   // Pipeline Central (the heavy profile): mist and multi-texture floors
        case kReplay: return {0.78f, 0.62f};   // twice the draws of open play: CPU-bound first; the picture can only give what the pixels give
        case kCut:    return {0.86f, 0.70f};   // goal celebrations and cut-scenes
        case kMega:   return {0.78f, 0.56f};   // the Mega Strike: the effects pass is fixed cost, the thinning (MSC_FX_GOV_MEGA) comes first
        default:      return {0.78f, 0.62f};   // menus: 17 passes
    }
}

// MSC_PICTURE_FLOORS, Smooth 60: when 60 does not hold at the floor the frame-rate controller holds a steady 30, and the picture goes to the FULL size (the table's top rung, edge smoothing as the
// mode has it), not smaller: after `enter` measured pictures in a held 30 the governor's range is that one rung; it leaves it after `leave` pictures back at 60. One entry, one move; a
// 30 that comes and goes within a few pictures (the controller's own anti-flap holds it for seconds) moves nothing.
struct Fallback30 {
    int enter = 90, leave = 240;
    int held = 0, calm = 0, entries = 0, exits = 0;
    bool full = false;
    bool Frame(bool held30) {
        if (held30) {
            calm = 0;
            if (!full && ++held >= enter) { full = true; ++entries; }
        } else {
            held = 0;
            if (full && ++calm >= leave) { full = false; calm = 0; ++exits; }
        }
        return full;
    }
};

// MSC_PICTURE_FLOORS: the presets with the Smooth 60 floor (vgx_floors.h ScaleFloor: the per-side scale of the game's own size that holds the floor's pixels, 1.13 of 640x448 for 800x456) as the lowest
// scale either number may have: no scene starts or ends below it (the old presets went down to 0.56, 448x320 at 0.70).
inline Preset PresetForFloor(int kind, float minScale) {
    Preset p = PresetFor(kind);
    p.start = std::max(p.start, minScale);
    p.floor = std::max(p.floor, minScale);
    return p;
}

// The scene kind from what the renderer knows: the scene class the game reports (0 other, 1 play, 2 replay, 3 cut-scene), a Mega Strike scene, the heavy stadium profile.
inline int KindOf(int sceneClass, bool mega, bool heavy) {
    if (mega) return kMega;
    if (sceneClass == 1) return heavy ? kHeavy : kPlay;
    if (sceneClass == 2) return kReplay;
    if (sceneClass == 3) return kCut;
    return kMenu;
}

// nxr3a MSC_SCENE_GPU_RESET: true when `kind` is live play (play or the heavy stadium's play) and the scene before it was another kind (or none): the GPU figures of that other scene are stale.
inline bool PlayBeginsAfterOther(int prev, int kind) { return (kind == kPlay || kind == kHeavy) && prev != kPlay && prev != kHeavy; }

// The level table is the renderer's (kDynBase); it is given here as the per-side scale of each level. Level for a scale: the first level at or below it, from `from` on.
inline int LevelFor(const float* scale, int count, int from, float want) {
    for (int l = from; l < count; ++l)
        if (scale[l] <= want + 0.001f) return l;
    return count - 1;
}

struct Gov {
    // configuration (MSC_LOCK60_*; defaults from the budget table)
    float downAt = 0.90f;      // two of the last three frames over this share of the budget ask for a step
    float lateAt = 1.00f;      // one frame over this asks for a step at once
    float fit = 0.82f;         // a step down aims at this share of the budget (by the model, on the frame's own time)
    float upFit = 0.80f;       // a step up needs every proof frame's projected time under this share (a tenth of them may be over; none over upHard)
    float upHard = 0.90f;
    float probeAt = 0.95f;     // a frame over this share within the probation of an up step takes it back
    int proofFrames = 150;     // measured frames of proof for an up step: 2.5 s at 60
    int probeFrames = 90;
    int gap = 20;              // measured frames between two size changes (20 at 60: a third of a second)
    int lateGap = 4;           // ... after a change, for a step down taken on one late frame (the frames queued at the old size are measured too)
    int maxRungs = 3;
    int upExtra = 0;           // flip25 (MSC_LOCK60_UPSLOW, default 0 = off): frames of extra proof for an up step per step down not yet forgiven (every size change costs a 75-85 ms picture on the console: a rung that is stepped up to and down from again within seconds is a visible flap, jobs 6122: up, down 39 s later, up 3.9 s later, down 3.4 s later)
    int blockBase = 120;       // frames: the block on trying again after a failed up step: 2 s, doubled for each failure, at most blockMax (60 s)
    int blockMax = 3600;
    float pixShare = 0.30f;    // the share of a frame's GPU time that follows the pixels
    // nxr3a (R3-4 / R3-6), set per scene kind by the renderer, all off in a plain Gov:
    //   steady: no step down on ONE late frame and no take-back on ONE frame over probeAt (the menus and the loading screens, whose single late frames are texture-upload bursts: 6122 f=504-506,
    //           GPU 18.9 ms of 16.7 with a mean of 6.7 made a 61 ms size change in a menu); a sustained overload (two of three frames over downAt and the mean over it) still steps down
    //   frozen: no size decision at all (MSC_LOCK60_MENU_FREEZE)
    //   noUp:   no step up (a LAN / online session: a failed try is two size changes of 60-85 ms, and the other console waits for both; MSC_LOCK60_NET_HOLD); steps down stay
    bool steady = false, frozen = false, noUp = false;
    // state
    int level = 0, best = 0, floor = 0;
    float ema = -1.0f;
    int since = 1000;          // measured frames since the last change
    int hot = 0;               // leaky count of frames over downAt (0..3)
    int win = 0, bad = 0;      // the proof window
    int probation = 0;         // frames left on probation after an up step
    int fails = 0;             // failed up steps in a row (the doubling)
    int blocked = 0;           // frames left in which no up step is tried
    int calm = 0;              // frames since the last down step (30 s forgive one failure)
    int recentDowns = 0;       // steps down not yet forgiven (one is forgiven per 30 s without a step down): each beyond the first lengthens the next proof by its own length (periodic bursts settle low)
    int decay = 0;             // frames since the last step down or forgiven one
    int downs = 0, ups = 0, takebacks = 0;
    unsigned spikesHeld = 0;   // late frames (or probation take-backs) that `steady` did not act on
    bool no_room = false;      // the last decision found no ready rung below
    void Start(int lvl, int bestLevel, int floorLevel) {
        level = lvl; best = bestLevel; floor = floorLevel;
        ema = -1.0f; since = 1000; hot = 0; win = bad = 0; probation = 0; blocked = 0; calm = 0; recentDowns = 0; decay = 0;
        no_room = false;
    }
    // The model's relative cost of a rung: a fixed part and a part that follows the pixels. pix(l): the pixels of a rung relative to the game's own size (with the pass scale and
    // the samples folded in by the caller).
    template <class Pix>
    float Cost(int l, Pix pix) const { return (1.0f - pixShare) + pixShare * pix(l); }

    // One measured frame. ms: its GPU time; budget: the frame budget; pix(l): relative pixel cost of a rung; ready(l): the rung may be entered without making anything;
    // holdUp: something that must finish first (the effects governor has a step left); blockedNow: no decision at all (a forced level, a scene change in progress).
    // Returns the level to go to, or -1 to stay. The caller applies it and calls Changed().
    template <class Pix, class Ready>
    int Frame(float ms, float budget, Pix pix, Ready ready, bool holdUp, bool blockedNow) {
        if (blockedNow) return -1;
        ++since;
        ++calm;
        if (probation > 0) --probation;
        if (blocked > 0) --blocked;
        ema = ema < 0 ? ms : 0.75f * ema + 0.25f * ms;
        hot = ms > downAt * budget ? std::min(hot + 1, 3) : std::max(hot - 1, 0);
        if (frozen) { no_room = false; return -1; }
        const bool late = ms > lateAt * budget;
        const float cur = Cost(level, pix);
        no_room = true;
        for (int l = level + 1; l <= floor; ++l)
            if (ready(l)) { no_room = false; break; }
        // down
        const bool askLateRaw = late && since >= lateGap;
        const bool askLate = askLateRaw && !steady;
        if (askLateRaw && steady && !(hot >= 2 && ema > downAt * budget && since >= gap)) ++spikesHeld;
        const bool askMean = hot >= 2 && ema > downAt * budget && since >= gap;
        if ((askLate || askMean) && !holdUp) {
            const float own = askLate ? std::max(ms, ema) : ema;
            int l = level, k = 0;
            while (l < floor && k < maxRungs) {
                int n = l + 1;
                while (n <= floor && !ready(n)) ++n;
                if (n > floor) break;
                l = n;
                ++k;
                if (own * Cost(l, pix) / cur <= fit * budget) break;
            }
            if (l != level) {
                if (probation > 0) Failed();   // the step up did not hold: take it back, wait before the next try
                recentDowns = std::min(recentDowns + 1, 8);
                decay = 0;
                calm = 0;
                return l;
            }
            return -1;   // nothing ready below: the frame-rate controller's relief is the next answer
        }
        // a frame over probeAt inside the probation of an up step: back at once (late frames come in clusters)
        if (probation > 0 && ms > probeAt * budget && since >= lateGap && !steady) {
            int l = level + 1;
            while (l <= floor && !ready(l)) ++l;
            if (l <= floor) {
                Failed();
                recentDowns = std::min(recentDowns + 1, 8);
                decay = 0;
                calm = 0;
                return l;
            }
        }
        // forgive a failure after 30 s of calm
        if (recentDowns > 0 && ++decay >= 1800) { --recentDowns; decay = 0; }
        if (fails > 0 && calm >= 1800) { --fails; calm = 900; }
        // up, on proof
        if (level > best && since >= gap && blocked == 0 && !holdUp && !late && !noUp) {
            const int nxt = level - 1;
            if (!ready(nxt)) { win = bad = 0; return -1; }
            const float proj = ms * Cost(nxt, pix) / cur;
            if (proj > upHard * budget) {
                win = bad = 0;
            } else {
                ++win;
                if (proj > upFit * budget) ++bad;
                if (bad * 10 > win) win = bad = 0;
                else if (win >= proofFrames * (1 + std::min(std::max(recentDowns - 1, 0), 3)) + upExtra * std::min(recentDowns, 4)) {
                    win = bad = 0;
                    return nxt;
                }
            }
        } else if (since < gap || late || holdUp) {
            win = bad = 0;
        }
        return -1;
    }
    // The caller took the step to `to`.
    void Changed(int to, bool wasUp) {
        if (wasUp) { ++ups; probation = probeFrames; } else ++downs;
        level = to;
        since = 0;
        hot = 0;
        win = bad = 0;
        ema = -1.0f;   // the old size's mean says nothing about the new one
    }

private:
    void Failed() {
        ++takebacks;
        if (fails < 6) ++fails;
        blocked = std::min(blockBase << std::min(fails - 1, 5), blockMax);
        probation = 0;
    }
};

// The level a scene class starts at, clamped into the ready rungs between `best` and `floor`; -1 when no rung is ready.
template <class Ready>
inline int StartLevel(int want, int best, int floor, Ready ready) {
    const int l = std::clamp(want, best, floor);
    for (int k = l; k <= floor; ++k)
        if (ready(k)) return k;
    for (int k = l - 1; k >= best; --k)
        if (ready(k)) return k;
    return -1;
}

// The rungs to make ahead (outside play): every rung from the class preset's start down to the deepest floor any scene uses, then rungs above the start while the memory allows.
// kib(l): the video memory a rung's target takes. Returns the number of levels written to `out`, in the order to make them (the rungs a scene needs first).
template <class Kib>
inline int TourOrder(int start, int bestLevel, int floorLevel, long budgetKib, Kib kib, int* out, int cap) {
    int n = 0;
    long used = 0;
    auto add = [&](int l) {
        if (n >= cap) return;
        for (int i = 0; i < n; ++i) if (out[i] == l) return;
        const long k = kib(l);
        if (used + k > budgetKib) return;
        used += k;
        out[n++] = l;
    };
    for (int l = start; l <= floorLevel; ++l) add(l);
    for (int l = start - 1; l >= bestLevel; --l) add(l);
    return n;
}

// The steps after the picture is at its floor and the frame is still over (the order the owner asked for: effects first, then the rungs, then these): 1 the crowd's impostors refreshed
// half as often, 2 and 3 the players' shadow pictures redrawn one and two frames less often (the shadow's place still follows the player every frame). One step per `stepEvery`
// frames of overload (30 measured frames over 92 % of the budget at the floor), one back after `calmFrames` (300) under 70 % or with a rung to spare, never two moves within `hold` frames.
struct Relief {
    int step = 0, maxStep = 3;
    int hot = 0, calm = 0, wait = 0;
    int stepEvery = 30, calmFrames = 300, hold = 120;
    // nxr4a MSC_L60_RELIEF_BAND (bandAt, a share of the budget; 0 = the rule as before): at the floor rung (noRoom) a step came back only under 0.70 of the budget, but one only went up over 0.92, so a
    // step earned in a walk-out or a Mega Strike (30 ms against the 60 budget) stayed for the whole match in a stadium that sits at 0.72-0.77 of the budget in play (Vice, 6121:1780 "relief 3, GPU mean
    // 12.1 ms"; 6122.log:1689 shows the same stadium at relief 0 on the same rung at 11.0 ms). With a band a step also comes back under bandAt (0.80) of the budget, never into a step it would have to go up
    // again: a step that went up again within reclimbWithin frames of a band return blocks the band for blockFrames, doubled for each such return (at most 8 times the first). Render only.
    float bandAt = 0.0f;
    int reclimbWithin = 1800, blockFrames = 3600, blockMaxMul = 8;
    int t_ = 0, lastBandAt_ = -1000000000, blockUntil_ = 0, blockLen_ = 0;
    int bandReturns = 0, bandReclimbs = 0;
    int calmLeak = 2;   // (with the band on: a frame between the band and the climb line takes this many frames off the calm count instead of wiping it, NEON round 5)
    // ema: the smoothed GPU time; noRoom: no ready rung below the current one; holdUp: the effects governor still has a step. Returns the new step, or -1 for none.
    int Frame(float ema, float budget, bool noRoom, bool holdUp) {
        ++t_;
        if (wait > 0) --wait;
        if (noRoom && !holdUp && ema > 0.92f * budget) {
            calm = 0;
            if (++hot >= stepEvery && step < maxStep && wait == 0) {
                hot = 0; wait = hold;
                if (t_ - lastBandAt_ <= reclimbWithin) {   // (a return by the band came back up: the band was wrong for this scene)
                    blockLen_ = blockLen_ ? std::min(blockLen_ * 2, blockFrames * blockMaxMul) : blockFrames;
                    blockUntil_ = t_ + blockLen_;
                    ++bandReclimbs;
                    lastBandAt_ = -1000000000;
                }
                return ++step;
            }
        } else {
            hot = 0;
            const bool lowCalm = ema < 0.70f * budget;
            const bool bandCalm = bandAt > 0.0f && t_ >= blockUntil_ && ema < bandAt * budget;
            if (!noRoom || lowCalm || bandCalm) {
                if (++calm >= calmFrames && step > 0 && wait == 0) {
                    calm = 0; wait = hold;
                    if (noRoom && !lowCalm) { lastBandAt_ = t_; ++bandReturns; }
                    return --step;
                }
            } else if (bandAt > 0.0f && calmLeak > 0 && calm > 0) {
                calm = calm > calmLeak ? calm - calmLeak : 0;
            } else {
                calm = 0;
            }
        }
        return -1;
    }
};

} // namespace lock60
} // namespace vgx
