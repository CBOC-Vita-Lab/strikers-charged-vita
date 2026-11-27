// nxfly25 (25 Nov 2026): MSC_VGX_ATLAS_APART, the two decisions of AtlasPasses (vgx_draw.cpp) as a host-tested header.
// 1. ApartTarget: a pass that draws into the joint atlas texture and samples a picture of another size that lives in the joint texture gets a texture of its own for its picture (a small
//    page that is not the joint one) instead of being refused (a refused figure pass draws in the main target and is copied out: a scene break).
// 2. MaySample: the sort of consecutive atlas passes into page order may keep a group that samples the page another group draws into when one of the two draws into an `apart` page
//    and the sort keeps the writer before the reader (and it was before it in the order the game gave).
#pragma once
#include <cstddef>
#ifndef VGX_ATLASAPART_BREAK
#define VGX_ATLASAPART_BREAK 0   // host test mutants (renderer/tests/atlas_apart.cpp): 1 same-size pictures count as a conflict that apart solves (they are not: a 128 pass reading a 128 cell), 2 no cap, 3 the writer-first test is dropped, 4 the original order is not checked, 5 the apart condition is dropped (every dependency allowed), 6 a picture that is apart already is made apart again (no memory)
#endif
namespace vgx {
namespace atlasapart {
struct Conflict {
    bool direct, joinOn, jointSize, apartOn;   // the pass: a direct pass of a joint size, the switches
    bool alreadyApart;                          // the pass's picture has its own page already
    size_t apartCount, apartMax;                // pictures kept apart so far, the cap
    bool sampledIsAtlasPicture, sampledApart;   // a draw of the pass samples a picture of the atlas; its cell is in an apart page
    bool sampledSameSize, sampledInPassTexture; // its cell has the pass's own cell size; its texture is the one the pass's size lives in (the joint texture)
};
inline bool ApartTarget(const Conflict& c) {
    if (!(c.direct && c.joinOn && c.jointSize && c.apartOn)) return false;
#if VGX_ATLASAPART_BREAK != 6
    if (c.alreadyApart) return true;
#endif
#if VGX_ATLASAPART_BREAK != 2
    if (c.apartCount >= c.apartMax) return false;
#endif
    if (!c.sampledIsAtlasPicture || c.sampledApart) return false;
#if VGX_ATLASAPART_BREAK != 1
    if (c.sampledSameSize) return false;
#endif
    return c.sampledInPassTexture;
}
// reader g samples the page group h draws into; hApart / gApart: that group draws into an apart page; hFirst: h was before g in the order given; keyLess: the sort puts h before g
inline bool MaySample(bool hApart, bool gApart, bool hFirst, bool keyLess) {
#if VGX_ATLASAPART_BREAK == 5
    return hFirst && keyLess;
#else
    if (!(hApart || gApart)) return false;
#if VGX_ATLASAPART_BREAK == 3
    return hFirst;
#elif VGX_ATLASAPART_BREAK == 4
    return keyLess;
#else
    return hFirst && keyLess;
#endif
#endif
}
}  // namespace atlasapart
}  // namespace vgx
