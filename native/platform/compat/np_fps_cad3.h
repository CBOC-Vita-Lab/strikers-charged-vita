// np_fps_cad3.h: MSC_HELD_CADENCE=auto (wave 11, cadence16). A steady 3-refresh cadence (20 pictures a second, every picture on screen for exactly 50 ms) for a HELD scene
// (cut-scene, goal celebration, replay) whose pictures cannot be made in 33 ms. Pure: no clock, no I/O, no game types, so np_fps.cpp and the host test (tools/fps/cad3_unit.cpp)
// run the same code. C++11.
//
// The problem (console logs r11c, r12, r13, held scenes): a picture that misses its 33 ms slot is shown for 3 or more refreshes, the next one that fits for 2 again, so the scene shows an
// uneven 3/4/2/3 mix ("held 0/0/2/27/39 vblanks", judder 0.5-2.3 refreshes a picture). A steady 20 reads better than that mix; it is a deliberate drop to 20, hence OFF by default and
// the owner's call.
//
// The rule.
//   - Only a held scene is judged (the caller passes enabled = false in live play, in menus and loads, in LAN, online or a recorded game, in a Mega Strike, when the game locks the swap itself, and when
//     the late wait that makes the cadence is not on). Leaving the held scene puts 2 back at once.
//   - In: of the last `inWindow` (20) judged pictures of a visit more than a third (`inMisses`, 7) were longer than `missMs` (36 ms: a picture held for 3 refreshes or more). The first
//     `skip` pictures of a visit carry its first-sight work and are not judged; a picture over `stallMs` is a load, not a pace, and is not judged either.
//   - Out: `outFrames` (40) pictures in a row whose work (workMs, the slowest stage, not the 50 ms they are shown for) stayed under `outMs` (28 ms: the 33 ms slot with margin).
//   - One entry and at most one exit per visit (maxChanges = 2 counts both), so a scene never flips back and forth; after the exit the visit stays at 2.
//   - Carry (`carry`, off in the build): a visit of a class that ended in 3 would start the next visit of that class in 3 at its cut; the exit rule still applies. Off because the logs show
//     cut-scenes of one class that fit 33 ms (83-98 % of pictures) right after ones that do not, and a scene that fits with little margin never earns the exit (work under 28 ms). So the
//     decision is taken inside the visit, after the skipped pictures and the 20 judged (about 24 pictures: 0.8 to 1.6 s of a shot), not at the cut.
// Presentation only: the period is read by the late wait and the gate of the game thread; nothing the simulation or the network reads.
#ifndef NP_FPS_CAD3_H
#define NP_FPS_CAD3_H

// NP_CAD3_BREAK=1..6 (host proof mutants, never set in a build): 1 the way in at half instead of a third, 2 one fitting picture brings 2 back, 3 a scene may enter again after its exit,
// 4 the enabled flag is ignored, 5 the first pictures of a visit are judged, 6 every visit starts in 3 (carry not cleared). tools/fps/cad3_unit.cpp must fail for each.
#ifndef NP_CAD3_BREAK
#define NP_CAD3_BREAK 0
#endif

namespace npfps {

struct Cad3Config {
    int inWindow = 20;        // pictures judged for the way in
    int inMisses = 7;         // misses in the window that mean "more than a third" (misses * 3 > window)
    double missMs = 36.0;     // a picture longer than this missed its 33.4 ms slot
    int outFrames = 40;       // pictures in a row under outMs that bring 2 back
    double outMs = 28.0;
    int skip = 4;             // first pictures of a visit that are not judged
    double stallMs = 134.0;   // a picture this long is a load or a stall: not judged
    int maxChanges = 2;       // changes per visit (one entry, one exit)
    bool carry = false;       // true: a class that ended in 3 starts its next visit in 3 at its cut. OFF: the logs show scenes of one class that fit 33 ms right after ones that do not (cut-scenes: 17-98 % fit)
};

struct Cad3In {
    bool enabled;       // a held scene that may be paced at 3 (single player, auto policy 30, late wait on)
    unsigned visit;     // identifies the visit: changes at every cut (task state and cut counter folded by the caller)
    int cls;            // 0 replay, 1 cut-scene (the carry slot)
    double pictureMs;   // the length of the picture just finished (frame to frame): the measure before the cadence is on
    double workMs;      // the work of the picture just finished (slowest stage, waits left out): the measure while it is on
};

struct Cad3Out {
    int period;         // refreshes a picture is shown for: 2 or 3
    int event;          // 0 none, 1 entered 3 (misses), 2 left 3 (fits), 3 started in 3 (carry), 4 left held scene (back to 2)
    int missCount;      // misses in the window at the event (event 1)
};

class Cad3 {
public:
    explicit Cad3(const Cad3Config& c = Cad3Config()) : c_(c) { Reset(); }
    void Reset() {
        period_ = 2; haveVisit_ = false; visit_ = 0; cls_ = 0; n_ = 0; ring_ = 0; ringN_ = 0; misses_ = 0; fit_ = 0; changes_ = 0; entered_ = exited_ = false;
        carry_[0] = carry_[1] = false;
        for (int i = 0; i < 64; ++i) ringBits_[i] = 0;
    }
    int Period() const { return period_; }
    unsigned Entries() const { return entries_; }
    unsigned Exits() const { return exits_; }
    unsigned Carried() const { return carried_; }
    unsigned Visits() const { return visits_; }

    Cad3Out Frame(const Cad3In& in) {
        Cad3Out o; o.period = period_; o.event = 0; o.missCount = 0;
        if (NP_CAD3_BREAK != 4 && !in.enabled) {
            if (haveVisit_) { EndVisit(); o.event = period_ == 3 ? 4 : 0; }
            haveVisit_ = false;
            period_ = 2;
            o.period = 2;
            return o;
        }
        if (!haveVisit_ || in.visit != visit_) {
            if (haveVisit_) EndVisit();
            haveVisit_ = true; visit_ = in.visit; cls_ = in.cls ? 1 : 0; ++visits_;
            n_ = 0; ringN_ = 0; ring_ = 0; misses_ = 0; fit_ = 0; changes_ = 0; entered_ = exited_ = false;
            for (int i = 0; i < 64; ++i) ringBits_[i] = 0;
            period_ = 2;
            if (c_.carry && (NP_CAD3_BREAK == 6 || carry_[cls_]) && c_.maxChanges >= 2) { period_ = 3; entered_ = true; changes_ = 1; ++carried_; o.event = 3; }
            o.period = period_;
        }
        ++n_;
        if (NP_CAD3_BREAK != 5 && n_ <= c_.skip) return o;
        if (period_ == 2) {
            if (in.pictureMs > c_.stallMs) return o;   // a load, not a pace
            const int w = c_.inWindow < 64 ? c_.inWindow : 64;
            const int miss = in.pictureMs > c_.missMs ? 1 : 0;
            if (ringN_ == w) misses_ -= ringBits_[ring_];
            else ++ringN_;
            ringBits_[ring_] = miss; misses_ += miss;
            ring_ = (ring_ + 1) % w;
            if (ringN_ == w && (NP_CAD3_BREAK == 3 || (!entered_ && !exited_)) && changes_ < c_.maxChanges + (NP_CAD3_BREAK == 3 ? 4 : 0) && misses_ >= (NP_CAD3_BREAK == 1 ? 1 : c_.inMisses) && misses_ * (NP_CAD3_BREAK == 1 ? 2 : 3) > w) {
                period_ = 3; entered_ = true; ++changes_; ++entries_; fit_ = 0; o.event = 1; o.missCount = misses_;
            }
        } else {
            if (in.workMs > c_.stallMs) return o;
            fit_ = in.workMs < c_.outMs ? fit_ + 1 : 0;
            if (fit_ >= (NP_CAD3_BREAK == 2 ? 1 : c_.outFrames) && !exited_ && changes_ < c_.maxChanges) {
                period_ = 2; exited_ = true; ++changes_; ++exits_; o.event = 2;
            }
        }
        o.period = period_;
        return o;
    }

private:
    void EndVisit() { carry_[cls_] = period_ == 3; }
    Cad3Config c_;
    int period_;
    bool haveVisit_;
    unsigned visit_;
    int cls_, n_, ring_, ringN_, misses_, fit_, changes_;
    bool entered_, exited_;
    bool carry_[2];
    unsigned char ringBits_[64];
    unsigned entries_ = 0, exits_ = 0, carried_ = 0, visits_ = 0;
};

} // namespace npfps

#endif
