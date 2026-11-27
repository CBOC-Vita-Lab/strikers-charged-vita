// MSC_POSE_FAST: the real SAnimDecode.cpp, math.cpp and nlMath.cpp, plain code against the shortcut, bit for bit. armhf under qemu-arm (run_pose_fast_test.py).
//   decoders: every 16-bit value in every lane of Rot16; every (b0, b1, b2) and every (b3, b4, b5) of Rot12; a key ending a page with the next page unmapped.
//   nlRecipSqrt: a stride through all 2^32 floats and all floats near the edges of its integer test, FPSCR.FZ clear and set.
//   nlQuatRenormalize: against nlQuatNLerp(q, q, z, +-0) on unit quaternions, on the values of decoded keys, on awkward bit patterns, with FPSCR.FZ clear and set.
#include "Game/SAnimDecode.h"
#include "NL/nlMath.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

extern "C" const unsigned int __float_huge[1] = { 0x7F800000 };
extern "C" const unsigned int __float_nan[1] = { 0x7FC00000 };

static uint32_t rngState = 0x9E3779B9u;
static uint32_t Rng()
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

static int fails;
static void Fail(const char* what, const void* a, const void* b)
{
    if (fails++ < 5)
    {
        const uint32_t* x = (const uint32_t*)a;
        const uint32_t* y = (const uint32_t*)b;
        printf("FAIL %s: %08x %08x %08x %08x / %08x %08x %08x %08x\n", what, x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3]);
    }
}

static void Check16(const void* key)
{
    nlQuaternion a, b;
    gNpPoseFast = 0;
    SAnimDecodeRot16(&a, key);
    gNpPoseFast = 1;
    SAnimDecodeRot16(&b, key);
    if (memcmp(&a, &b, sizeof(a)) != 0)
        Fail("rot16", &a, &b);
}

static void Check12(const void* key)
{
    nlQuaternion a, b;
    gNpPoseFast = 0;
    SAnimDecodeRot12(&a, key);
    gNpPoseFast = 1;
    SAnimDecodeRot12(&b, key);
    if (memcmp(&a, &b, sizeof(a)) != 0)
        Fail("rot12", &a, &b);
}

static uint32_t SetFz(int on)
{
#ifdef __arm__
    uint32_t v;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(v));
    v = on ? (v | (1u << 24)) : (v & ~(1u << 24));
    __asm__ volatile("vmsr fpscr, %0" : : "r"(v));
    return v;
#else
    return 0;
#endif
}

static long renormDone, renormDeclined;

static void CheckRenorm(const nlQuaternion& q, unsigned short angle, float t)
{
    nlQuaternion fast = q;
    if (!nlQuatRenormalize(fast))
    {
        renormDeclined++;
        return;
    }
    renormDone++;
    nlQuaternion plain = q, z;
    fn_802B549C(z, angle);
    nlQuatNLerp(plain, plain, z, t);
    if (memcmp(&plain, &fast, sizeof(plain)) != 0)
        Fail("renormalize", &plain, &fast);
}

static void CheckRecip(uint32_t bits)
{
    float x, a, b;
    memcpy(&x, &bits, 4);
    gNpPoseFast = 0;
    a = nlRecipSqrt(x, true);
    gNpPoseFast = 1;
    b = nlRecipSqrt(x, true);
    if (memcmp(&a, &b, 4) != 0 && !(a != a && b != b))   // NaN payloads may differ only between NaNs, and the code that makes them is the same
        Fail("nlRecipSqrt", &a, &b);
}

static float Awkward()
{
    static const uint32_t v[] = { 0, 0x80000000, 1, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00800000, 0x80800000, 0x3F800000, 0xBF800000, 0x7F000000, 0x7EFFFFFF, 0xFEFFFFFF,
        0x7F7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00001, 0x7F800001 };
    uint32_t u = v[Rng() % (sizeof(v) / sizeof(v[0]))];
    float f;
    memcpy(&f, &u, 4);
    return f;
}

int main()
{
    setvbuf(stdout, 0, _IONBF, 0);
    // Rot16: each lane, every value, the others random
    for (int lane = 0; lane < 4; lane++)
    {
        for (int v = 0; v < 65536; v++)
        {
            int16_t key[4] = { (int16_t)Rng(), (int16_t)Rng(), (int16_t)Rng(), (int16_t)Rng() };
            key[lane] = (int16_t)v;
            Check16(key);
        }
    }
    // Rot12: two exhaustive halves
    for (uint32_t v = 0; v < (1u << 24); v++)
    {
        unsigned char key[6] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)Rng(), (unsigned char)Rng(), (unsigned char)Rng() };
        Check12(key);
        unsigned char key2[6] = { key[3], key[4], key[5], key[0], key[1], key[2] };
        Check12(key2);
    }
    printf("decoders compared\n");
    // a key at the very end of a mapping, at every alignment the data can have
    unsigned char* page = (unsigned char*)mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == (unsigned char*)MAP_FAILED || mprotect(page + 4096, 4096, PROT_NONE) != 0)
    {
        printf("FAIL no guard page\n");
        return 1;
    }
    for (int i = 0; i < 4096; i++)
        page[i] = (unsigned char)Rng();
    Check12(page + 4096 - 6);
    Check12(page + 4096 - 7);
    Check16(page + 4096 - 8);
    Check16(page + 4096 - 9);

    for (int fz = 0; fz < 2; fz++)
    {
        SetFz(fz);
        // nlRecipSqrt: a stride through all floats, and every float around each edge of the integer test
        long compared = 0;
        for (int part = 0; part < 7; part++)
        {
            static const uint32_t from[7] = { 0, 0x00700000, 0x7F700000, 0x7F7FFFF0, 0x80000000, 0xFF7FFFF0, 0x3F000000 };
            static const uint32_t count[7] = { 0x20000, 0x200000, 0x100000, 0x20, 0x20000, 0x20, 0x1000000 };
            for (uint32_t k = 0; k < count[part]; k++)
            {
                CheckRecip(from[part] + k);
                compared++;
            }
        }
        for (uint64_t u = 0; u < 0x100000000ull; u += 509)
        {
            CheckRecip((uint32_t)u);
            compared++;
        }
        printf("FZ=%d: nlRecipSqrt compared %ld\n", fz, compared);
        long start = renormDone;
        for (int i = 0; i < 3000000; i++)
        {
            nlQuaternion q;
            float* c = (float*)&q;
            switch (i % 4)
            {
            case 0:   // a unit quaternion, roughly
                for (int k = 0; k < 4; k++)
                    c[k] = (float)(int)(Rng() & 0xFFFF) / 32768.0f - 1.0f;
                nlQuatNormalize(q, q);
                break;
            case 1:   // decoded keys, zeros included
                for (int k = 0; k < 4; k++)
                    c[k] = (Rng() & 3) ? (int16_t)(Rng() & 0xFFF0) * (1.0f / 32768.0f) : 0.0f;
                break;
            case 2:   // anything
                for (int k = 0; k < 4; k++)
                {
                    uint32_t u = Rng();
                    memcpy(&c[k], &u, 4);
                }
                break;
            default:
                for (int k = 0; k < 4; k++)
                    c[k] = (Rng() & 1) ? Awkward() : (float)(int)(Rng() & 0xFF) / 128.0f - 1.0f;
                break;
            }
            CheckRenorm(q, (unsigned short)Rng(), (Rng() & 1) ? 0.0f : -0.0f);
        }
        printf("FZ=%d: renormalize compared %ld\n", fz, renormDone - start);
    }
    SetFz(0);
    printf("renormalize declined %ld\n", renormDeclined);
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails != 0;
}
