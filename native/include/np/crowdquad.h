// MSC_CROWD_QUADFAST (np/overlap.h): the corner positions of a crowd sprite quad, ImpostorSprite::BuildQuad's arithmetic split in two so that what does not
// depend on the sprite is done once per Render call instead of once per sprite. The unit axes a0 = cs * right + sn * up and b0 = -sn * right + cs * up are the
// same expressions as before (sn, cs from nlSinCos at angle 0, a constant), and a = (0.5 * width) * a0 is the same product nlVec3Scale made, so every
// corner is the same float. Test: tools/gamefast/crowdquad_test.cpp (real nlMath.cpp, compared bit for bit with the plain code below).
#pragma once
#include "NL/nlMath.h"

struct NpQuadBasis
{
    nlVector3 a0, b0;
};

inline void NpQuadBasisInit(NpQuadBasis& basis, const nlVector3& right, const nlVector3& up, float sn, float cs)
{
    basis.a0.x = cs * right.x + sn * up.x;
    basis.a0.y = cs * right.y + sn * up.y;
    basis.a0.z = cs * right.z + sn * up.z;
    basis.b0.x = -sn * right.x + cs * up.x;
    basis.b0.y = -sn * right.y + cs * up.y;
    basis.b0.z = -sn * right.z + cs * up.z;
}

// The four corners (12 floats: x y z of corner 0..3) of a sprite at `pos` whose scaled width and height are `width` and `height`.
inline void NpQuadCorners(float* out, const nlVector3& pos, float width, float height, const NpQuadBasis& basis)
{
    const float sa = 0.5f * width, sb = 0.5f * height;
    nlVector3 a, b;
    a.x = sa * basis.a0.x; a.y = sa * basis.a0.y; a.z = sa * basis.a0.z;
    b.x = sb * basis.b0.x; b.y = sb * basis.b0.y; b.z = sb * basis.b0.z;
    nlVector3 position = pos;
    position.z += 0.5f * height;
    out[0] = position.x + a.x + b.x;  out[1] = position.y + a.y + b.y;  out[2] = position.z + a.z + b.z;
    out[3] = position.x - a.x + b.x;  out[4] = position.y - a.y + b.y;  out[5] = position.z - a.z + b.z;
    out[6] = position.x - a.x - b.x;  out[7] = position.y - a.y - b.y;  out[8] = position.z - a.z - b.z;
    out[9] = position.x + a.x - b.x;  out[10] = position.y + a.y - b.y;  out[11] = position.z + a.z - b.z;
}
