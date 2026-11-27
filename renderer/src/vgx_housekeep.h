// 1.3 housekeeping rules (wave 8, lessons from the Galaxy console runs). Header-only and pure, so a host test (tests/housekeep.cpp) runs the shipped decisions.
//
// SweepGap (MSC_VGX_HOUSEKEEP_BACKOFF): a periodic sweep that found nothing to let go waits twice as long before the next, up to a ceiling; one that found something goes back to its
// base period. Galaxy's program-map trim walked ~970 programs every picture and deleted nothing (1.5 ms a picture, a console estimate). Timing only: an entry becomes due by age alone,
// so what changes is the picture at which it is freed, never what is drawn.
//
// ThrashGuard (MSC_POOL_THRASH_GUARD): TrimPool deletes a pass object (or kept target) of a size after 60 presents when the free video memory reads as tight, and the hold ladder / a
// Mega Strike makes it again. In Vita3K the free figure reads 0 and that loop ran in the menus (wave 8 integrate notes); on a console any real shortage does the same. A size that is
// made again within kWindow presents of being trimmed is pinned: the age trim leaves it alone (a failed allocation still frees everything idle, PoolEmergencyTrim), so a
// trim-and-remake happens at most once for a size.
#pragma once
#include <cstdint>

namespace vgx::housekeep {

// The next gap (in pictures or frames) between two sweeps. base: the sweep's normal period; cur: the gap in force; nothingDone: the sweep just run let nothing go.
inline uint64_t SweepGap(uint64_t base, uint64_t cur, bool nothingDone, uint64_t ceiling) {
#if defined(VGX_HK_BREAK) && VGX_HK_BREAK == 1
    (void)cur; (void)ceiling;
    return nothingDone ? base * 2 : base;   // deliberate break: the gap never grows past two periods
#elif defined(VGX_HK_BREAK) && VGX_HK_BREAK == 2
    (void)nothingDone; (void)ceiling;
    return cur < base ? base : cur * 2;     // deliberate break: the gap never comes back after something was freed
#else
    if (!nothingDone) return base;
    const uint64_t g = (cur < base ? base : cur) * 2;
    return g > ceiling ? (ceiling < base ? base : ceiling) : g;
#endif
}

constexpr uint32_t kWindow = 3600;   // presents (a minute at 60) in which a remake of a trimmed size counts as a loop
constexpr int kSizes = 16;

class ThrashGuard {
public:
    struct Entry { int w = 0, h = 0; uint32_t trimmedAt = 0; bool pinned = false; bool used = false; };
    void Clear() { for (Entry& e : e_) e = Entry{}; remakes_ = 0; }
    // an object of this size was let go at present `now`
    void Trimmed(int w, int h, uint32_t now) {
        Entry* e = Find(w, h);
        if (!e) e = Free();
        e->used = true; e->w = w; e->h = h; e->trimmedAt = now;
    }
    // an object of this size was made at present `now`; true when it was a remake of a size trimmed within kWindow presents (the size is now pinned)
    bool Made(int w, int h, uint32_t now) {
        Entry* e = Find(w, h);
        if (!e || !e->trimmedAt) return false;
#if defined(VGX_HK_BREAK) && VGX_HK_BREAK == 3
        (void)now;
        const bool loop = true;   // deliberate break: any remake, however late, pins
#else
        const bool loop = now - e->trimmedAt <= kWindow;
#endif
        e->trimmedAt = 0;
        if (!loop) return false;
        if (!e->pinned) { e->pinned = true; ++remakes_; return true; }
        return false;
    }
    bool Pinned(int w, int h) const {
#if defined(VGX_HK_BREAK) && VGX_HK_BREAK == 4
        (void)w; (void)h;
        return false;   // deliberate break: nothing is ever pinned
#else
        for (const Entry& e : e_) if (e.used && e.pinned && e.w == w && e.h == h) return true;
        return false;
#endif
    }
    uint32_t Remakes() const { return remakes_; }
private:
    Entry* Find(int w, int h) { for (Entry& e : e_) if (e.used && e.w == w && e.h == h) return &e; return nullptr; }
    Entry* Free() {
        Entry* best = &e_[0];
        for (Entry& e : e_) {
            if (!e.used) return &e;
            if (!e.pinned && (best->pinned || e.trimmedAt < best->trimmedAt)) best = &e;
        }
        return best;
    }
    Entry e_[kSizes];
    uint32_t remakes_ = 0;
};

} // namespace vgx::housekeep
