// np_found_games.h: what the LAN lobby does with a "found game" beacon, as one pure decision (host-testable, no game types).
//
// The original rule: if the list is full (10), drop every beacon, even one from a game already on the list; if a game of the same name is on the list,
// refresh only its timer. Two faults in a crowded or changing LAN: (1) with 10 entries nobody's timer is refreshed, so all 10 expire together 3.5 s later and
// the list flickers empty; (2) a host that comes back with the same name from another address or port (a Wi-Fi rejoin) keeps its OLD address on the list for as
// long as it keeps beaconing, and a join goes nowhere. Here: a known name is always refreshed, and its address, port and type follow the newest beacon; a new
// name on a full list replaces the stalest entry only when that one has already missed about half its life (it is likely gone), else it is ignored.
#pragma once
#include <string.h>

namespace NpFound {

enum Action { kIgnore = 0, kRefresh = 1, kAppend = 2, kReplace = 3 };

struct Verdict {
    Action action;
    int index;        // kRefresh / kReplace: the entry; kAppend: the first free slot
};

// names: `count` host names (entries) each `nameLen` bytes apart; since: seconds since each was last seen.
inline Verdict Decide(const char* names, int nameStride, const float* since, int sinceStride, int count, int cap, const char* newName, float expireSeconds) {
    for (int i = 0; i < count; ++i)
        if (strncmp(names + i * nameStride, newName, 11) == 0) return Verdict{kRefresh, i};
    if (count < cap) return Verdict{kAppend, count};
    int stalest = -1;
    float worst = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float s = *reinterpret_cast<const float*>(reinterpret_cast<const char*>(since) + i * sinceStride);
        if (stalest < 0 || s > worst) { stalest = i; worst = s; }
    }
    if (stalest >= 0 && worst >= expireSeconds * 0.5f) return Verdict{kReplace, stalest};
    return Verdict{kIgnore, -1};
}

}  // namespace NpFound
