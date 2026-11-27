// MSC_CROWD_QUADFAST: np/crowdquad.h against ImpostorSprite::BuildQuad's own arithmetic (copied below, statement for statement), bit for bit,
// with the real nlMath.cpp's nlSinCos. armhf under qemu-arm (run_crowdquad_test.py). Positions of many random sprites and axes, then the shorts the writer stores.
#include "NL/nlMath.h"
#include "np/crowdquad.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t rngState = 0x1234567u;
static uint32_t Rng() { rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5; return rngState; }
static float Rf(float lo, float hi) { return lo + (hi - lo) * (float)(Rng() & 0xFFFFFF) / 16777216.0f; }

// ImpostorSprite::BuildQuad, positions only
static void Plain(float* out, const nlVector3& pos, float width, float height, const nlVector3& right, const nlVector3& up)
{
    float sn, cs;
    nlSinCos(&sn, &cs, DegreesToAngle(0.0f));
    nlVector3 a, b;
    a.x = cs * right.x + sn * up.x;
    a.y = cs * right.y + sn * up.y;
    a.z = cs * right.z + sn * up.z;
    b.x = -sn * right.x + cs * up.x;
    b.y = -sn * right.y + cs * up.y;
    b.z = -sn * right.z + cs * up.z;
    nlVec3Scale(a, 0.5f * width);
    nlVec3Scale(b, 0.5f * height);
    nlVector3 position = pos;
    position.z += 0.5f * height;
    out[0] = position.x + a.x + b.x;  out[1] = position.y + a.y + b.y;  out[2] = position.z + a.z + b.z;
    out[3] = position.x - a.x + b.x;  out[4] = position.y - a.y + b.y;  out[5] = position.z - a.z + b.z;
    out[6] = position.x - a.x - b.x;  out[7] = position.y - a.y - b.y;  out[8] = position.z - a.z - b.z;
    out[9] = position.x + a.x - b.x;  out[10] = position.y + a.y - b.y;  out[11] = position.z + a.z - b.z;
}

int main()
{
    float sn, cs;
    nlSinCos(&sn, &cs, DegreesToAngle(0.0f));
    printf("sin(0) %08x cos(0) %08x\n", *(uint32_t*)&sn, *(uint32_t*)&cs);
    long compared = 0, fails = 0, shortFails = 0;
    for (int axes = 0; axes < 4000; axes++)
    {
        nlVector3 right = { Rf(-3, 3), Rf(-3, 3), Rf(-3, 3) }, up = { Rf(-3, 3), Rf(-3, 3), Rf(-3, 3) };
        if (axes % 7 == 0) right.x = 0.0f;
        if (axes % 11 == 0) up.z = -0.0f;
        NpQuadBasis basis;
        NpQuadBasisInit(basis, right, up, sn, cs);
        for (int k = 0; k < 64; k++)
        {
            nlVector3 pos = { Rf(-120, 120), Rf(-120, 120), Rf(-5, 40) };
            const float w = Rf(0.01f, 9.0f) * Rf(0.5f, 2.0f), h = Rf(0.01f, 9.0f) * Rf(0.5f, 2.0f);
            float a[12], b[12];
            Plain(a, pos, w, h, right, up);
            NpQuadCorners(b, pos, w, h, basis);
            for (int j = 0; j < 12; j++)
            {
                compared++;
                if (memcmp(&a[j], &b[j], 4) != 0)
                {
                    // a zero of the other sign is the only difference allowed to exist; report it
                    if (fails++ < 5) printf("FAIL axes %d k %d j %d: %08x %08x\n", axes, k, j, *(uint32_t*)&a[j], *(uint32_t*)&b[j]);
                }
                if ((short)(a[j] * 64.0f) != (short)(b[j] * 64.0f)) shortFails++;
            }
        }
    }
    printf("compared %ld floats, %ld differ in bits, %ld differ as stored shorts\n", compared, fails, shortFails);
    printf(fails || shortFails ? "FAILED\n" : "ok\n");
    return fails || shortFails;
}
