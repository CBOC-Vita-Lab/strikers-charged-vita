// held16 (wave 11): the pure decisions of MSC_FXAA_WARM=2 (the budgeted warm of the one-sample program variants and the one-sample targets a held scene is drawn into) and of
// MSC_PICTURE_HOLD_SCENEPOL's entry into a replay. Header-only so tests/heldaa.cpp runs the shipped rules. Render only.
#pragma once
#include <cstdint>

namespace vgxheldaa {

inline int WarmN(long v) { return v < 1 ? 1 : (v > 64 ? 64 : static_cast<int>(v)); }          // variants a present
inline double WarmMs(long v) { return v < 1 ? 1.0 : (v > 40 ? 40.0 : static_cast<double>(v)); }   // ms a present (a variant that starts under it may end over it)

// Does the one-sample target of a size have to be made ahead? Only for a stadium whose held scenes are one sample, outside live play and not a Mega Strike, when none is kept and
// the memory leaves its colour + depth plus the trim floor.
struct AheadIn { bool warm2, heldOneSample, play, mega, heavy, kept; long kib; int freeKib; long floorKib; };
enum AheadOut { kAheadNo = 0, kAheadMake = 1, kAheadNoRoom = 2 };
inline AheadOut Ahead(const AheadIn& a) {
    if (!a.warm2 || !a.heldOneSample || a.play || a.mega || a.heavy || a.kept) return kAheadNo;
    if (a.freeKib >= 0 && a.freeKib < a.floorKib + a.kib) return kAheadNoRoom;
    return kAheadMake;
}

// SCENEPOL entry: a replay or cut-scene takes the scene policy when it is in force, the held scene is not already begun for this kind, and the picture is not a Mega Strike.
// The replay class is entered on the replay's own scene class (2) or, if the game keeps reporting live play for the first pictures of a replay, on the replay flag (see HoldTick).
inline bool SceneWant(int kind, bool mega, int kReplay, int kCut) { return (kind == kReplay || kind == kCut) && !mega; }

} // namespace vgxheldaa
