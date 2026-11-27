// np_fps_tiermem.h (pace13, 22 Nov 2026; MSC_TIER_MEMORY=1, default OFF): Smooth 60 remembers the tier a stadium settled at, per picture mode, and the next visit starts there. Pure code
// (host test native/tools/fps/tiermem_unit.cpp). The idea is Galaxy's size step per stage kept on the card: The Dump always steps down, so starting a match at the 45 (or the 30) skips the
// kick-off dip (60, 12 repeated refreshes, then 45, maybe 30) and the flap risk of the first minutes. The way up is unchanged: the seeded start is a relief like any other (Auto::SeedRelief:
// the sticky hold and its 12 s proof at 75 %; Fallback45::SeedFallback: the hold and its 12 s of pictures that fit), so a stadium that now holds 60 gets it back on proof.
// File: <data dir>/np-tier-memory.txt (the directory that holds NAND/, never inside the saves). Text, one line per entry after the header:   mode stadium tier
//   mode 1 = Smooth 60 in tiers (the only picture mode with tiers today; the field keeps room for others), stadium = the game's stadium id, tier 45 or 30. 60 is not stored: it is the default.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NP_TMEM_BREAK
#define NP_TMEM_BREAK 0   // host test mutants (tools/fps/tiermem_unit.cpp, each must fail it): 1 the settled tier is not the one with the most time, 2 a 60 does not clear the entry, 3 the file is keyed without the mode
#endif

namespace npfps {

enum { kTierMemSmooth60 = 1 };

class TierMem {
public:
    static const int kMax = 64;
    static const int kMinPlaySecs = 60;   // a match must have had this much live single-player play to count
    int Get(int mode, int stadium) const {   // 0: nothing remembered
        for (int i = 0; i < n_; ++i) if (e_[i].stadium == stadium && (NP_TMEM_BREAK == 3 || e_[i].mode == mode)) return e_[i].tier;
        return 0;
    }
    // The tier of a finished match: the one with the most live play seconds (secs60 / secs45 / secs30), when there was enough play. Returns true when the table changed (to be written).
    bool Settled(int mode, int stadium, double secs60, double secs45, double secs30) {
        if (stadium < 0 || secs60 + secs45 + secs30 < kMinPlaySecs) return false;
        int t = 60;
#if NP_TMEM_BREAK == 1
        if (secs45 > 0.0 || secs30 > 0.0) t = secs30 > secs45 ? 30 : 45;
#else
        if (secs45 > secs60 && secs45 >= secs30) t = 45;
        else if (secs30 > secs60 && secs30 > secs45) t = 30;
#endif
        return Put(mode, stadium, t);
    }
    bool Put(int mode, int stadium, int tier) {
        for (int i = 0; i < n_; ++i) {
            if (e_[i].stadium != stadium || e_[i].mode != mode) continue;
            if (tier == 60) {
#if NP_TMEM_BREAK == 2
                return false;
#endif
                e_[i] = e_[--n_];
                return true;
            }
            if (e_[i].tier == tier) return false;
            e_[i].tier = tier;
            return true;
        }
        if (tier == 60) return false;
        if (n_ >= kMax) return false;
        e_[n_].mode = mode; e_[n_].stadium = stadium; e_[n_].tier = tier; ++n_;
        return true;
    }
    int Count() const { return n_; }
    // text <-> table. Parse ignores what it does not understand (a newer file, a hand edit): never fails the start.
    void Parse(const char* text) {
        n_ = 0;
        if (!text) return;
        const char* p = text;
        while (*p && n_ < kMax) {
            int mode = 0, stadium = 0, tier = 0, used = 0;
            if (sscanf(p, "%d %d %d%n", &mode, &stadium, &tier, &used) == 3 && mode > 0 && stadium >= 0 && stadium < 1000 && (tier == 45 || tier == 30)) {
                bool dup = false;
                for (int i = 0; i < n_; ++i) if (e_[i].mode == mode && e_[i].stadium == stadium) dup = true;
                if (!dup) { e_[n_].mode = mode; e_[n_].stadium = stadium; e_[n_].tier = tier; ++n_; }
            }
            while (*p && *p != '\n') ++p;
            if (*p) ++p;
        }
    }
    int Format(char* out, size_t size) const {
        int len = snprintf(out, size, "# Smooth 60 tier memory: mode stadium tier (45 or 30). Safe to delete.\n");
        for (int i = 0; i < n_ && len > 0 && (size_t)len < size; ++i) len += snprintf(out + len, size - (size_t)len, "%d %d %d\n", e_[i].mode, e_[i].stadium, e_[i].tier);
        return len;
    }
private:
    struct E { int mode, stadium, tier; } e_[kMax];
    int n_ = 0;
};

} // namespace npfps
