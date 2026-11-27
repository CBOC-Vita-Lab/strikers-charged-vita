// np_fps_cadence.h: MSC_CADENCE (wave 7). A steady picture rate per scene class, decided at the cuts. Pure: no clock, no I/O, no game types, so the game's np_fps.cpp and
// the unit test (tools/fps/cadence_unit.cpp) run the same code. C++11.
//
// The problem it answers. A goal celebration, a goal replay and a cut-scene do not fit 16.7 ms on the Vita at any picture size (budget table: about 22-26 ms of GPU, front-end
// 6-10 M instructions a picture; estimates, nothing ran on a console). AUTO's controller (np_fps_ctl.h) meets such a scene as follows: the policy of the scene before stays, a 60
// that is in force misses for a few frames and is taken away in the middle of the shot (an uneven 40-55 for a moment), and a 30 is tried for 60 again after 2 s of evidence, again in the
// middle of the shot. After the scene the controller's cool-down keeps the open play that follows at 30 for 6 s or more (Hold30 and a failed try both arm it). An uneven rate feels worse
// than a steady one; open play must never be given 30 by such a decision.
//
// What it does instead.
//   - Scene classes from the task state: open play, replay, cut-scene (the goal celebration and the other NIS), menu (front-end menus, stadium viewer, pause menu: MSC_CADENCE_MENUS, ClassOfTaskState),
//     other (loading, boot). Only a replay, a cut-scene or a menu can be held; open play and loading never are.
//   - A held scene is shown at a locked 30: AUTO's own policy 30 (every picture for exactly two refreshes, the display gate and swap mode 4), the 50 Hz simulation blended to the
//     picture's time as before. A scene that is not held is left to the controller. The verdict is taken at the cut (the first frame of the scene) and never changes inside the scene.
//   - The verdict of a class (a replay; the goal celebration; any other cut-scene) is learned from measured frames, with long hysteresis: it starts held (the budget's answer), a visit
//     whose frames (95 %) all fit 60 at 85 % of a refresh counts as a fit visit, `visitsToFree` fit visits in a row release it (2), and a released class whose visit then made the
//     controller drop to 30 is held again and needs twice as many fit visits next time (at most `visitsMax`: 8). A released class is put to 60 at its cut (not 2 s into the shot).
//   - At the cut back to open play the policy that was in force before the scene is put back at once (Auto::Resume, Between::Resume): the controller's cool-down is not allowed to hold
//     the match at 30 after a replay. Open play is never held by this class.
//   - A goal in play arms a deferral: until the cut (at most `deferMax` seconds) the controller makes no down decision, so the burst of the goal's effects cannot take the rate away in
//     the shot that precedes the cut. (The frames keep their own length; only the policy waits.)
#ifndef NP_FPS_CADENCE_H
#define NP_FPS_CADENCE_H

namespace npfps {

enum CadenceClass { kCadOther = 0, kCadOpen = 1, kCadReplay = 2, kCadCut = 3, kCadMenu = 4 };

// The class of a task state (eGameTaskState, Game/Task/GameTaskState.h). `menus` is MSC_CADENCE_MENUS: the front-end menus (team, captain, stadium select, results), the stadium viewer and the
// pause menu are a class of their own that is held like a replay; with it off they are "other", left to the controller as before. Never held: open play (the player steers a character or the
// Mega Save hand), loading and the boot states (the controller's own load pace), anything unknown.
enum { kTsPaused = 1, kTsGameplay = 2, kTsFrontEnd = 4, kTsReplay = 8, kTsNis = 16, kTsDebugReplay = 131072, kTsStadiumViewer = 16777216, kTsHomeMenu = 33554432,
       kTsLoading = 65536 | 524288 | 1048576 | 2097152 | 4194304 | 8388608 };
inline int ClassOfTaskState(unsigned ts, bool menus) {
    const unsigned masked = ts & ~(unsigned)(kTsPaused | kTsHomeMenu);
    if (ts & (kTsReplay | kTsDebugReplay)) return kCadReplay;
    if (ts & kTsNis) return kCadCut;
    if (menus && (ts & kTsLoading) == 0) {
        if (masked == (unsigned)kTsGameplay) return (ts & kTsPaused) ? kCadMenu : kCadOpen;   // the pause menu over a match is a menu; the match itself is open play
        if (masked & (kTsFrontEnd | kTsStadiumViewer)) return kCadMenu;
    }
    return masked == (unsigned)kTsGameplay ? kCadOpen : kCadOther;
}

struct CadenceConfig {
    double fitMargin;    // a frame fits when its predicted 60 Hz cost is under this share of a refresh
    double fitShare;     // share of a visit's judged frames that must fit for the visit to be a fit visit
    double minVisitSec;  // shortest visit that is judged
    int minFrames;       // fewest judged frames in a visit that is judged
    int skipFrames;      // frames at the start of a visit that are not judged (the first pictures of a scene carry its first-sight work)
    int visitsToFree;    // consecutive fit visits that release a held class
    int visitsMax;       // the most that a failure can ask for
    double stallRefresh; // a frame this many refreshes long (a load) is not judged
    double deferMax;     // seconds a goal's deferral may last
    CadenceConfig()
        : fitMargin(0.85), fitShare(0.95), minVisitSec(1.0), minFrames(40), skipFrames(12), visitsToFree(2), visitsMax(8), stallRefresh(8.0), deferMax(4.0) {}
};

struct CadenceIn {
    double now;          // seconds on any monotonic clock
    int cls;             // CadenceClass of the task state
    unsigned key;        // sub-class of a cut-scene / replay (the caller may leave 0: a goal in play makes the next cut-scene the goal celebration)
    unsigned scene;      // the controller's scene identity (a change is a cut, as the controller sees it)
    bool valid;          // costMs is a measurement
    double costMs;       // the frame's predicted 60 Hz cost (busiest core or the GPU at its smallest picture)
    double refreshMs;
    double intervalMs;   // the previous frame's length
    bool enabled;        // false: the verdict is "not held", nothing else (a LAN session, MSC_CADENCE=0)
    int policy;          // the controller's policy for the frame now (1 = 30, 2 = 60): what the scene is in
    bool paced;          // MSC_FPS_BETWEEN: AUTO's 30 is running free
};

struct CadenceOut {
    bool hold30;         // the scene is held at 30 (call Auto::Hold30)
    bool cut;            // this frame is the first of a new scene
    bool resume;         // this frame is the first of open play after a held scene: put resumePolicy / resumePaced back
    int resumePolicy;
    bool resumePaced;
    bool go60;           // this frame is the first of a released replay / cut-scene: policy 60 from the cut
    bool deferDown;      // a goal is waiting for its cut: the controller makes no down decision
    CadenceOut() : hold30(false), cut(false), resume(false), resumePolicy(0), resumePaced(false), go60(false), deferDown(false) {}
};

class Cadence {
public:
    explicit Cadence(const CadenceConfig& c = CadenceConfig()) : cfg_(c) { Reset(); }

    void Reset() {
        for (int i = 0; i < kKeys; ++i) { held_[i] = true; fit_[i] = 0; need_[i] = cfg_.visitsToFree; }
        cls_ = -1; key_ = 0; scene_ = 0xFFFFFFFFu; visitKey_ = 0;
        visitHold_ = false; visitFrames_ = 0; visitFit_ = 0; skipped_ = 0;
        visitStart_ = lastNow_ = 0;
        visitFreeDropped_ = false;
        sawHeavy_ = false; lastOpenPolicy_ = 0; lastOpenPaced_ = false; haveOpen_ = false;
        pending_ = false; pendingAt_ = 0; goalKey_ = false; changes_ = 0;
        lastHold_ = false;
    }

    // A goal in play (Presentation::OnGoalScored). The next cut-scene is the goal celebration.
    void NoteGoal(double now) { pending_ = true; pendingAt_ = now; goalKey_ = true; }

    CadenceOut Frame(const CadenceIn& in) {
        CadenceOut o;
        const bool cutNow = in.cls != cls_ || in.key != key_ || in.scene != scene_;
        if (!in.enabled) {
            if (cutNow) { cls_ = in.cls; key_ = in.key; scene_ = in.scene; visitHold_ = false; visitKey_ = in.key; }
            lastHold_ = false;
            return o;
        }
        if (cutNow) {
            Finish();
            const bool heavy = in.cls == kCadReplay || in.cls == kCadCut || in.cls == kCadMenu;
            unsigned key = in.key;
            if (in.cls == kCadCut && goalKey_) key = 1;
            visitHold_ = heavy ? held_[Slot(in.cls, key)] : false;
            if (heavy) {
                sawHeavy_ = true;
                if (!visitHold_) o.go60 = true;
            }
            if (in.cls == kCadOpen) {
                if (sawHeavy_ && haveOpen_) { o.resume = true; o.resumePolicy = lastOpenPolicy_; o.resumePaced = lastOpenPaced_; }
                sawHeavy_ = false;
                goalKey_ = false;
                pending_ = false;
            }
            cls_ = in.cls; key_ = in.key; scene_ = in.scene;
            visitKey_ = key;
            visitFrames_ = 0; visitFit_ = 0; skipped_ = 0; visitStart_ = in.now;
            visitFreeDropped_ = false;
            o.cut = true;
            if (visitHold_ != lastHold_) { ++changes_; lastHold_ = visitHold_; }
        }
        if (in.cls == kCadOpen) {
            lastOpenPolicy_ = in.policy;
            lastOpenPaced_ = in.paced;
            haveOpen_ = true;
        }
        o.hold30 = visitHold_;
        // a goal waits for its cut
        if (pending_ && in.cls == kCadOpen) {
            if (in.now - pendingAt_ < cfg_.deferMax) o.deferDown = true;
            else pending_ = false;
        }
        // judge the frame
        if (in.cls == kCadReplay || in.cls == kCadCut || in.cls == kCadMenu) {
            if (skipped_ < cfg_.skipFrames) {
                ++skipped_;
            } else {
                if (in.valid && in.refreshMs > 0 && !(in.intervalMs > cfg_.stallRefresh * in.refreshMs)) {
                    ++visitFrames_;
                    if (in.costMs <= cfg_.fitMargin * in.refreshMs) ++visitFit_;
                }
                // a released class whose frames the controller took down to 30 anyway
                if (!visitHold_ && in.policy == 1) visitFreeDropped_ = true;
            }
        }
        lastNow_ = in.now;
        return o;
    }

    // Verdict changes made so far (a verdict changes at cuts only, and rarely: the test counts them).
    int Changes() const { return changes_; }
    bool Held(int cls, unsigned key) const { return held_[Slot(cls, key)]; }
    int Fit(int cls, unsigned key) const { return fit_[Slot(cls, key)]; }
    int Need(int cls, unsigned key) const { return need_[Slot(cls, key)]; }
    bool Pending() const { return pending_; }

private:
    static const int kKeys = 5;
    static int Slot(int cls, unsigned key) {
        if (cls == kCadReplay) return 3;
        if (cls == kCadMenu) return 4;
        if (cls == kCadCut) return key == 1 ? 1 : 2;
        return 0;
    }
    // The visit that ends now (its class, key and frames are the old ones).
    void Finish() {
        if (cls_ != kCadReplay && cls_ != kCadCut && cls_ != kCadMenu) return;
        const int k = Slot(cls_, visitKey_);
        const bool judged = visitFrames_ >= cfg_.minFrames && lastNow_ - visitStart_ >= cfg_.minVisitSec;
        if (visitHold_) {
            if (!judged) return;   // a short visit says nothing
            const bool fit = (double)visitFit_ >= cfg_.fitShare * (double)visitFrames_;
            if (fit) {
                if (++fit_[k] >= need_[k]) { held_[k] = false; fit_[k] = 0; }
            } else {
                fit_[k] = 0;
            }
        } else if (visitFreeDropped_) {
            // released, and the controller took it down inside the shot: held again, and a longer proof next time
            held_[k] = true;
            fit_[k] = 0;
            need_[k] = need_[k] * 2 > cfg_.visitsMax ? cfg_.visitsMax : need_[k] * 2;
        }
    }

    CadenceConfig cfg_;
    bool held_[kKeys];
    int fit_[kKeys], need_[kKeys];
    int cls_;
    unsigned key_, scene_, visitKey_;
    bool visitHold_;
    int visitFrames_, visitFit_, skipped_;
    double visitStart_, lastNow_;
    bool visitFreeDropped_;
    bool sawHeavy_, haveOpen_;
    int lastOpenPolicy_;
    bool lastOpenPaced_;
    bool pending_, goalKey_;
    double pendingAt_;
    int changes_;
    bool lastHold_;
};

} // namespace npfps

#endif // NP_FPS_CADENCE_H
