// vgx_cadence_full.h: MSC_CADENCE_FULL (wave 7). The picture size of a scene that the cadence policy (np_fps_cadence.h) holds at a locked 30. Pure: no clock, no I/O, no GL, C++11.
//
// At 30 the budget is 33 ms and a replay at the game's own size (960x544 with the shape rules as they are) is estimated at about 26 ms, so a held scene is drawn at the game's own size
// instead of the smaller picture the 60 ladder had reached. The size changes at the cut that holds the scene and at the cut that ends the hold (to the ladder's size of before), and at
// no other time: inside the scene the ladder's own rules apply as they do for any scene held at 30 (a step down needs frames over the 30 budget, with the ladder's hysteresis; nothing
// goes above the game's own size). A render target is never made at a cut: the size is taken only when its target is already kept (or is the picture in use); otherwise the picture
// stays as it is and the miss is counted.
#ifndef VGX_CADENCE_FULL_H
#define VGX_CADENCE_FULL_H

namespace vgx {
namespace cadfull {

struct In {
    bool on;            // the switch and the preconditions (the ladder is on, not fixed or forced, not LOCKED 60, no Mega Strike)
    bool held;          // the frame-rate policy holds this scene at 30 (np_fps_cadence.h Cadence hold30)
    int level;          // the level in force (a larger index is a smaller picture)
    int native;         // the level of the game's own size
    bool nativeReady;   // a render target of the game's own size is kept, or is the picture in use
    bool backReady;     // likewise for the level remembered at the cut in (read by the caller for Out.from of the last call: see Frame)
};

struct Out {
    int setLevel;       // -1: nothing; else the level to take now (a cut)
    bool cut;           // this frame is a cut of the hold (in or out)
    Out() : setLevel(-1), cut(false) {}
};

class Gov {
public:
    Gov() : lastHeld_(false), active_(false), from_(0), skippedIn_(0), skippedOut_(0), cutsIn_(0), cutsOut_(0) {}
    // The level to go back to at the cut out of the hold, for the caller to look up (is its target kept?): the level at the cut in.
    int From() const { return from_; }
    bool Active() const { return active_; }
    int SkippedIn() const { return skippedIn_; }
    int SkippedOut() const { return skippedOut_; }
    int CutsIn() const { return cutsIn_; }
    int CutsOut() const { return cutsOut_; }

    Out Frame(const In& in) {
        Out o;
        const bool held = in.on && in.held;
        if (held && !lastHeld_) {
            o.cut = true;
            ++cutsIn_;
            from_ = in.level;
            if (in.level > in.native) {
                if (in.nativeReady) { o.setLevel = in.native; active_ = true; }
                else { ++skippedIn_; active_ = false; }
            } else {
                active_ = false;   // (already at the game's own size or larger: nothing to give and nothing to give back)
            }
        } else if (!held && lastHeld_) {
            o.cut = true;
            ++cutsOut_;
            if (active_) {
                if (from_ != in.level) {
                    if (in.backReady) o.setLevel = from_;
                    else ++skippedOut_;
                }
                active_ = false;
            }
        }
        lastHeld_ = held;
        return o;
    }

private:
    bool lastHeld_, active_;
    int from_;
    int skippedIn_, skippedOut_, cutsIn_, cutsOut_;
};

} // namespace cadfull
} // namespace vgx

#endif // VGX_CADENCE_FULL_H
