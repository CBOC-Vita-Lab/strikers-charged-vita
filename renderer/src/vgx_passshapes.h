// MSC_CUTS_SHAPES_INTRO / MSC_CUTS_SHAPES_LOAD (held25): the logic of the pass-shape list, header-only so a host test runs the shipped code.
//
// held25 read the console logs of 25 Nov (Vice 6121-6123, The Dump 6033/6034). What MSC_CUTS_SHAPES (w9 cuts, held24 FIX-14) did there, and why the stadium fly-in still paid for its targets:
//  1. It recorded only pass objects made AFTER kick-off. The intro cut-scene (stadium fly-in, walk-out, entrance: 340-370 pictures before kick-off) makes its own: 751x1072 (the crowd atlas, with
//     depth), 560x530, 146x146, 73x73 ... Those were never in the list, so the second session (6034) read "2 new, 2 in all": 582x293 and 751x1072, both from the first goal.
//  2. It recorded each SIZE once. The atlas pass is drawn in many scenes a picture (a mask rebuild is a scene) and an object takes 8 scenes a picture (g_fboMaxUses): the intro makes 3 objects of
//     751x1072 (f750 one, f805 two more: [hitch] "+751x1072 +751x1072", pool f4). One pre-made object left two to be made in the first visible pictures.
//  3. It started only when the match was already running (g_cutsInMatch is set by the first match picture = the first picture of the cut-scene), one object per two pictures: in 6034 one object
//     (582x293) was made in that picture and the first picture itself made the other three (be 431 ms, "target", rt3/111 ms, other 319 ms).
//     The stadium's load runs 225-240 pictures (7-8 s) behind the loading screen before that.
// So: (INTRO) shapes are recorded from the match's start on, one line per object made (not per size), a depth that is added later is a 'U' line, and a shape's count is read back (at most
// kMaxPer); (LOAD) the pre-make may run in the last stretch of the stadium's load. All of it only decides WHEN a pass object is made and which: nothing is drawn, no simulation state.
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

#ifndef VGX_PASSSHAPES_BREAK
#define VGX_PASSSHAPES_BREAK 0   // host test mutants (renderer/tests/pass_shapes.cpp): 1 counts are not summed (one per size), 2 no cap, 3 'U' lines add instead of moving, 4 the load window ignores the
                                 // present count, 5 the load window stays open in a match, 6 memory check ignores the floor, 7 memory check ignores the cap, 8 missing() never asks for more than one,
                                 // 9 a pre-made object is recorded as new knowledge
#endif

namespace vgx {
namespace passshapes {

constexpr int kMaxPer = 4;        // objects of one size and depth the list asks for (4 x 8 scenes a picture)
constexpr int kMaxShapes = 24;    // distinct sizes

struct Shape {
    int w = 0, h = 0;
    bool depth = false;
    int n = 1;                    // objects of this size to have made
};

inline bool SizeOk(int w, int h) { return w >= 16 && h >= 16 && w <= 2048 && h <= 2048; }

// One line of the card's own file or of the package's seed. "P w h d" = an object was made (d: with depth); "U w h" = an object of w x h that was made without depth got one.
// counted false (MSC_CUTS_SHAPES_INTRO=0, the w10 reader): a size and depth is listed once, a 'U' line is ignored.
struct List {
    std::vector<Shape> v;
    bool counted = false;
    Shape* Find(int w, int h, bool d) {
        for (Shape& s : v) if (s.w == w && s.h == h && s.depth == d) return &s;
        return nullptr;
    }
    // returns true when the line changed the list
    bool AddLine(const char* line) {
        int w = 0, h = 0, d = 0;
        char c = 0;
        if (std::sscanf(line, "%c %d %d %d", &c, &w, &h, &d) == 4 && c == 'P' && SizeOk(w, h)) {
            Shape* s = Find(w, h, d != 0);
            if (!s) {
                if (counted && static_cast<int>(v.size()) >= kMaxShapes) return false;
                v.push_back(Shape{w, h, d != 0, 1});
                return true;
            }
            if (!counted) return false;
#if VGX_PASSSHAPES_BREAK == 1
            return false;
#else
            if (s->n >= kMaxPer) {
#if VGX_PASSSHAPES_BREAK == 2
                ++s->n;
                return true;
#endif
                return false;
            }
            ++s->n;
            return true;
#endif
        }
        if (counted && std::sscanf(line, "%c %d %d", &c, &w, &h) == 3 && c == 'U' && SizeOk(w, h)) {
            int zi = -1, si = -1;
            for (size_t i = 0; i < v.size(); ++i)
                if (v[i].w == w && v[i].h == h) { if (v[i].depth) si = static_cast<int>(i); else zi = static_cast<int>(i); }
#if VGX_PASSSHAPES_BREAK == 3
            zi = -1;
#endif
            if (zi >= 0 && v[static_cast<size_t>(zi)].n > 0) {   // (the object that was listed without depth is the one that got it)
                if (si >= 0) { if (v[static_cast<size_t>(si)].n < kMaxPer) ++v[static_cast<size_t>(si)].n; }
                else v.push_back(Shape{w, h, true, 1});
                if (--v[static_cast<size_t>(zi)].n == 0) v.erase(v.begin() + zi);
                return true;
            }
            if (si >= 0) { if (v[static_cast<size_t>(si)].n >= kMaxPer) return false; ++v[static_cast<size_t>(si)].n; return true; }
            if (static_cast<int>(v.size()) >= kMaxShapes) return false;
            v.push_back(Shape{w, h, true, 1});
            return true;
        }
        return false;
    }
    int TotalObjects() const { int t = 0; for (const Shape& s : v) t += s.n; return t; }
};

// Objects still to make of a shape: n wanted, `have` in the pool already (a pool object of that size, with depth when the shape has one).
inline int Missing(int n, int have) {
    const int m = n - have;
    if (m <= 0) return 0;
#if VGX_PASSSHAPES_BREAK == 8
    return 1;
#endif
    return m;
}

// KiB of one object: colour, and depth when it has one (RGBA8 and 32-bit depth).
inline long ObjectKib(int w, int h, bool depth) { return static_cast<long>(w) * h * 4 * (depth ? 2 : 1) / 1024; }

// May one more object of kib be made now? usedKib: what the pre-make has made in this match, capKib: MSC_CUTS_SHAPES_KIB, freeKib: free video memory (-1 = not known), floorKib: what must remain.
inline bool MemoryOk(long usedKib, long kib, long capKib, long freeKib, long floorKib) {
#if VGX_PASSSHAPES_BREAK != 7
    if (usedKib + kib > capKib) return false;
#endif
#if VGX_PASSSHAPES_BREAK == 6
    return true;
#endif
    return freeKib < 0 || freeKib >= floorKib + kib;
}

// The pre-make window outside a running match: the stadium's load (the stadium signal came, `sinceSignal` presents ago), not live play, no match running yet. minSince keeps it off the early part
// of the load (the textures are read first: pretouch of 484-513 textures, 7-10 s).
inline bool LoadWindow(bool loadOn, bool loadOpen, bool inMatch, bool play, uint32_t sinceSignal, uint32_t minSince) {
    if (!loadOn || !loadOpen || play) return false;
#if VGX_PASSSHAPES_BREAK == 5
    (void)inMatch;
#else
    if (inMatch) return false;
#endif
#if VGX_PASSSHAPES_BREAK == 4
    (void)sinceSignal; (void)minSince;
    return true;
#else
    return sinceSignal >= minSince;
#endif
}

// Is a pass object made by the pre-make new knowledge for the list? (no: it was asked for by the list)
inline bool RecordCreation(bool inPreMake, bool introOn, bool inMatch, bool kicked) {
#if VGX_PASSSHAPES_BREAK == 9
    inPreMake = false;
#endif
    if (inPreMake) return false;
    return introOn ? inMatch : kicked;
}

} // namespace passshapes
} // namespace vgx
