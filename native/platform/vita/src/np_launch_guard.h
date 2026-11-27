// np_launch_guard.h (stab13, MSC_LAUNCH_GUARD; the Galaxy port's CORE3 plan 6a and VitaMem's launch marker). Pure decisions, no console calls: the host test is
// tests/test_launch_guard.cpp, the card glue is in vita_plan.cpp.
//
// The core plan guard (np-plan.txt) learns of a start that did not get through its first 300 frames. A freeze later than that leaves no dump and no trace: the process
// cannot switch anything off while it is stuck. This guard has its own one-number file, ux0:data/mscharged/np-launch.txt:
//   at start         the number n (0 when there is no file or it is unreadable) is read and n + 1 is written: "this launch is running"
//   clean exit       (the game's quit, the settings screen's exit) or `kStableMinutes` of stable play (presented frames + time): 0 is written
//   a launch that finds n = 1 or 2 (one or two launches in a row that never cleared it: a freeze, a crash, a power cut, the system's kill) starts with the optional
//   thread / core / pacing switches below OFF; n = 3 or more starts as usual (a player who always quits with the PS button inside the first minutes must not be kept in
//   the safe mode for ever; the next clear sets 0). Nothing but the file np-launch.txt is written. Saves, settings and np-player.txt are never touched, a switch the
//   owner set himself (env.txt, np-player.txt) is never changed, and a launch with no unclean history changes nothing but that one file.
// Off: MSC_LAUNCH_GUARD=0, the console test kit (MSC_BOOT_MATCH or MSC_INPUT_SCRIPT set: the kit kills its runs), lab builds, a card error on the file.
#pragma once

#include <cstddef>

namespace NpLaunchGuard {

constexpr int kSafeMax = 2;            // consecutive unclean launches that are answered with the safe start
constexpr int kCountCap = 99;
constexpr unsigned kStableMinutes = 10; // MSC_LAUNCH_GUARD_MIN overrides (1..120)
constexpr unsigned long long kStableFrames = 300;

enum class State { kOff, kNormal, kSafe, kGaveUp };

// The text of the file: leading decimal digits, 0 for anything else.
inline int ParseCount(const char* text, size_t size) {
    int v = 0;
    size_t i = 0;
    while (i < size && (text[i] == ' ' || text[i] == '\t')) ++i;
    for (; i < size && text[i] >= '0' && text[i] <= '9'; ++i) {
        v = v * 10 + (text[i] - '0');
        if (v > kCountCap) return kCountCap;
    }
    return v;
}
inline State Decide(int previous) {
    if (previous <= 0) return State::kNormal;
    return previous <= kSafeMax ? State::kSafe : State::kGaveUp;
}
inline int NextCount(int previous) { return previous >= kCountCap ? kCountCap : previous + 1; }
inline bool Stable(unsigned long long frames, unsigned long long elapsedMs, unsigned minutes) {
    return frames >= kStableFrames && elapsedMs >= static_cast<unsigned long long>(minutes) * 60000ull;
}
inline unsigned MinutesFromText(const char* text) {
    if (!text || !*text) return kStableMinutes;
    unsigned v = 0;
    for (const char* p = text; *p >= '0' && *p <= '9'; ++p) v = v * 10 + static_cast<unsigned>(*p - '0');
    return v < 1 ? kStableMinutes : v > 120 ? 120 : v;
}

// The optional switches that a safe start turns off: lanes on other cores, helper threads and pacing. Each is exact on and off (their own proofs: det trace equal, per-picture
// hashes equal), so the match is the same; only speed is lost. The plan's thread plan has its own guard (np-plan.txt); MSC_FS_PIPE (the overlap of the frame send) is the one
// part of it that is also a switch of its own.
struct Risky { const char* name; const char* off; };
constexpr Risky kRisky[] = {
    {"MSC_VGX_FEPAR", "0"}, {"MSC_POSE_JOBS", "0"}, {"MSC_PHYS_POSE", "0"}, {"MSC_SNAP_PAR", "0"}, {"MSC_CROWD_POS_PAR", "0"}, {"MSC_BLEND_ASYNC", "0"},
    {"MSC_SIM26_BONES", "0"}, {"MSC_SIM26_ISLANDS", "0"}, {"MSC_SIM26_POSEORDER", "0"}, {"MSC_VITA_CORE0_ALONE", "0"}, {"MSC_PACE_VBLANK", "0"}, {"MSC_FS_PIPE", "0"},
    {"MSC_FPS_PROBE_ASYNC", "0"}, {"MSC_LANE_CORE3", "0"},
};
constexpr int kRiskyCount = static_cast<int>(sizeof(kRisky) / sizeof(kRisky[0]));

// Env: bool IsPlanDefault(const char* name) (the plan gave the value itself, the owner did not), void Set(const char* name, const char* value).
// A switch the plan did not set is the owner's (or absent): left alone. Returns how many were turned off.
template <class Env>
inline int ApplySafe(State state, Env& env) {
    if (state != State::kSafe) return 0;
    int n = 0;
    for (const Risky& r : kRisky) {
        if (!env.IsPlanDefault(r.name)) continue;
        env.Set(r.name, r.off);
        ++n;
    }
    return n;
}

} // namespace NpLaunchGuard
