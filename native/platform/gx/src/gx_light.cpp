// GX glue, part 5: light object helpers the vgx API does not provide. The software parts of the SDK's GXLight.c
// (decompilation, matching): the object is the hardware's light memory layout, which vgx's GXInitLightColor /
// GXInitLightPos / GXInitLightDir / GXLoadLightObjImm already use: 3 unused words, colour, a[3] (angle attenuation),
// k[3] (distance attenuation), position[3], direction[3].
#include <revolution/gx.h>

namespace {
struct Light {
    unsigned int pad[3];
    unsigned int color;
    float aa, ab, ac;
    float ka, kb, kc;
    float posX, posY, posZ;
    float dirX, dirY, dirZ;
};
static_assert(sizeof(Light) == 64, "GXLightObj is 64 bytes");
static_assert(sizeof(GXLightObj) == sizeof(Light), "GXLightObj layout");
inline Light* L(GXLightObj* o) { return reinterpret_cast<Light*>(o); }
} // namespace

extern "C" {

void GXInitLightAttnA(GXLightObj* light, f32 a, f32 b, f32 c) {
    Light* l = L(light);
    l->aa = a;
    l->ab = b;
    l->ac = c;
}

void GXInitLightAttnK(GXLightObj* light, f32 a, f32 b, f32 c) {
    Light* l = L(light);
    l->ka = a;
    l->kb = b;
    l->kc = c;
}

void GXInitLightSpot(GXLightObj* light, f32 angle, GXSpotFn fn) {
    Light* l = L(light);
    f32 a, b, c, d;
    if (angle <= 0.0f || angle > 90.0f) fn = GX_SP_OFF;
    const f32 rad = __builtin_cosf(3.14159265358979323846f * angle / 180.0f);
    switch (fn) {
    case GX_SP_FLAT: a = -1000.0f * rad; b = 1000.0f; c = 0.0f; break;
    case GX_SP_COS: b = 1.0f / (1.0f - rad); a = -rad * b; c = 0.0f; break;
    case GX_SP_COS2: c = 1.0f / (1.0f - rad); a = 0.0f; b = -rad * c; break;
    case GX_SP_SHARP:
        d = 1.0f / ((1.0f - rad) * (1.0f - rad));
        a = (rad * (rad - 2.0f)) * d;
        b = 2.0f * d;
        c = -d;
        break;
    case GX_SP_RING1:
        d = 1.0f / ((1.0f - rad) * (1.0f - rad));
        c = -4.0f * d;
        a = c * rad;
        b = 4.0f * (1.0f + rad) * d;
        break;
    case GX_SP_RING2:
        d = 1.0f / ((1.0f - rad) * (1.0f - rad));
        a = 1.0f - 2.0f * rad * rad * d;
        b = 4.0f * rad * d;
        c = -2.0f * d;
        break;
    case GX_SP_OFF:
    default: a = 1.0f; b = 0.0f; c = 0.0f; break;
    }
    l->aa = a;
    l->ab = b;
    l->ac = c;
}

void GXInitLightDistAttn(GXLightObj* light, f32 distance, f32 brightness, GXDistAttnFn fn) {
    Light* l = L(light);
    f32 a, b, c;
    if (distance < 0.0f) fn = GX_DA_OFF;
    if (brightness <= 0.0f || brightness >= 1.0f) fn = GX_DA_OFF;
    switch (fn) {
    case GX_DA_GENTLE:
        a = 1.0f;
        b = (1.0f - brightness) / (brightness * distance);
        c = 0.0f;
        break;
    case GX_DA_MEDIUM:
        a = 1.0f;
        b = (0.5f * (1.0f - brightness)) / (brightness * distance);
        c = (0.5f * (1.0f - brightness)) / (distance * (brightness * distance));
        break;
    case GX_DA_STEEP:
        a = 1.0f;
        b = 0.0f;
        c = (1.0f - brightness) / (distance * (brightness * distance));
        break;
    case GX_DA_OFF:
    default: a = 1.0f; b = 0.0f; c = 0.0f; break;
    }
    l->ka = a;
    l->kb = b;
    l->kc = c;
}

// A specular light is a directional light whose "position" is far away along the half vector.
void GXInitSpecularDir(GXLightObj* light, f32 x, f32 y, f32 z) {
    Light* l = L(light);
    const f32 dirX = -x, dirY = -y, dirZ = 1.0f + -z;
    f32 mag = dirX * dirX + dirY * dirY + dirZ * dirZ;
    if (mag != 0.0f) mag = 1.0f / __builtin_sqrtf(mag);
    l->dirX = dirX * mag;
    l->dirY = dirY * mag;
    l->dirZ = dirZ * mag;
    l->posX = -999999999999999999.0f * x;
    l->posY = -999999999999999999.0f * y;
    l->posZ = -999999999999999999.0f * z;
}

} // extern "C"
