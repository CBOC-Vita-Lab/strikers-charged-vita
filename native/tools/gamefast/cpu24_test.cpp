// MSC_CPU24_PART: the game's original float code (copied from ParticleSystem.cpp EvaluateParticleColour) against the
// cpu24 fast forms, bit for bit, over random and awkward inputs (zeros, -0, negative and huge times, lifeSpan 0 / negative, NaN, inf, colour scales 1.0f and others).
// Two builds: x86 with libc (run_cpu24_test.py host) and freestanding armhf with the VitaSDK compiler under qemu-arm -cpu cortex-a9 (run_cpu24_test.py arm; the game's float flags).
// CPU24_BREAK=1 in the compile (-DCPU24_BREAK=1) is the proof mutant: the fast colour is nudged for 1 case in 64 and the test must fail.
#ifdef NP_CPU24_FREESTANDING
extern "C" void* memcpy(void* d, const void* s, unsigned n) { char* a = (char*)d; const char* b = (const char*)s; while (n--) *a++ = *b++; return d; }
extern "C" int memcmp(const void* a, const void* b, unsigned n) { const unsigned char* x = (const unsigned char*)a; const unsigned char* y = (const unsigned char*)b; for (; n; n--, x++, y++) if (*x != *y) return *x < *y ? -1 : 1; return 0; }
static long Sys(long n, long a, long b, long c)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r7 __asm__("r7") = n;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}
static void Print(const char* s) { int n = 0; while (s[n]) n++; Sys(4, 1, (long)s, n); }
static void PrintU(unsigned long long v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); Print(b + i); }
static void Exit(int c) { Sys(248, c, 0, 0); }
// floor of a float without libm: the mathematical floor (what floor(double) gives for a float argument)
static float RefFloor(float x)
{
    if (!(x == x)) return x;
    if (x >= 8388608.0f || x <= -8388608.0f) return x;
    int i = (int)x;
    float f = (float)i;
    return f > x ? f - 1.0f : f;
}
#define FLOORF RefFloor
#else
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void Print(const char* s) { fputs(s, stdout); }
static void PrintU(unsigned long long v) { printf("%llu", v); }
static void Exit(int c) { exit(c); }
#define FLOORF(x) ((float)floor(x))   // the original's call (floor of a double: a float converted to double)
#endif

struct Col { unsigned char c[4]; };

// ---- the original (ParticleSystem.cpp) ----
static Col OrigColour(float te, float ls, const Col* tab, float rs, float gs, float bs)
{
    float frame = 24.0f * (te / ls);
    int first = (int)FLOORF(frame);
    int second = first + 1;
    Col colour;
    if (first >= 24) colour = tab[25];
    else {
        const Col* fc = &tab[first];
        const Col* sc = &tab[second];
        unsigned int fraction = (unsigned int)(65536.0f * ((frame - (float)first) / (float)(second - first)));
        unsigned int inv = 65536 - fraction;
        for (int i = 0; i < 4; ++i) colour.c[i] = (unsigned char)((fraction * sc->c[i] + inv * fc->c[i]) >> 16);
    }
    Col result;
    float red = colour.c[0] * rs; red = red >= 0.0f ? red : 0.0f; red = red <= 255.0f ? red : 255.0f; result.c[0] = (unsigned char)red;
    float green = colour.c[1] * gs; green = green >= 0.0f ? green : 0.0f; green = green <= 255.0f ? green : 255.0f; result.c[1] = (unsigned char)green;
    float blue = colour.c[2] * bs; blue = blue >= 0.0f ? blue : 0.0f; blue = blue <= 255.0f ? blue : 255.0f; result.c[2] = (unsigned char)blue;
    result.c[3] = colour.c[3];
    return result;
}
// ---- the cpu24 form (NpEvaluateParticleColourFast + the scale test of EvaluateParticleColour) ----
static Col NewColour(float te, float tf, float ls, const Col* tab, float rs, float gs, float bs, unsigned n)
{
    if (!(rs == 1.0f && gs == 1.0f && bs == 1.0f)) return OrigColour(te, ls, tab, rs, gs, bs);
    const float frac = ls > 0.0f ? tf : te / ls;
    float frame = 24.0f * frac;
    int first = frame >= 0.0f ? (int)frame : (int)FLOORF(frame);
    Col colour;
    if (first >= 24) colour = tab[25];
    else {
        const Col* fc = &tab[first];
        const Col* sc = &tab[first + 1];
        unsigned int fraction = (unsigned int)(65536.0f * (frame - (float)first));
        unsigned int inv = 65536 - fraction;
        for (int i = 0; i < 4; ++i) colour.c[i] = (unsigned char)((fraction * sc->c[i] + inv * fc->c[i]) >> 16);
    }
#ifdef CPU24_BREAK
    if ((n & 63u) == 1u) colour.c[0] = (unsigned char)(colour.c[0] + 1);
#endif
    (void)n;
    return colour;
}

static unsigned rs_ = 0x2545F491u;
static unsigned Rand() { rs_ ^= rs_ << 13; rs_ ^= rs_ >> 17; rs_ ^= rs_ << 5; return rs_; }
static float RF(float lo, float hi) { return lo + (hi - lo) * ((float)(Rand() >> 8) / 16777216.0f); }
static float FromBits(unsigned b) { float f; memcpy(&f, &b, 4); return f; }

int main()
{
    static Col tabMem[1200 + 26];
    Col* const tab = tabMem + 1200;   // (a negative keyframe index reads below the table: the original does too; room for those the test makes)
    unsigned long long total = 0, diffs = 0, fast = 0;
    for (unsigned round = 0; round < 2000; ++round)
    {
        for (int i = -1200; i < 26; ++i) for (int k = 0; k < 4; ++k) tab[i].c[k] = (unsigned char)Rand();
        for (unsigned j = 0; j < 4000; ++j)
        {
            float ls, te;
            const unsigned kind = Rand() & 31u;
            ls = RF(0.05f, 6.0f);
            te = RF(0.0f, 1.2f) * ls;
            if (kind == 0) te = -RF(0.0f, 2.0f);           // negative times
            else if (kind == 1) te = 0.0f;                  // a spawn
            else if (kind == 2) te = -0.0f;
            else if (kind == 3) ls = 0.0f;                  // lifeSpan 0: the divide is kept
            else if (kind == 4) ls = -RF(0.0f, 2.0f);
            else if (kind == 5) te = FromBits(Rand());      // any bits: NaN, inf, huge, subnormal
            else if (kind == 6) ls = FromBits(Rand());
            else if (kind == 7) te = ls;                    // exactly the end
            else if (kind == 8) te = ls * (1.0f - 1.0f / 64.0f) ;
            else if (kind == 9) te = ls * (23.0f / 24.0f);  // keyframe edge
            else if (kind == 10) te = 1.0e30f;
            const float tf = te / ls;                       // what UpdateAllParticles stored
            { const float fr = 24.0f * (te / ls); if (!(fr == fr) || fr < -1100.0f) continue; }   // (NaN or far below the table: an out of range read in the original too)
            static const float scales[4] = {1.0f, 1.0f, 0.5f, 1.25f};
            const float s = scales[Rand() & 3u];
            const float rsc = s, gsc = (Rand() & 7u) ? s : 1.0f, bsc = (Rand() & 7u) ? s : 1.0f;
            const Col a = OrigColour(te, ls, tab, rsc, gsc, bsc);
            const Col b = NewColour(te, tf, ls, tab, rsc, gsc, bsc, (unsigned)total);
            ++total;
            if (rsc == 1.0f && gsc == 1.0f && bsc == 1.0f) ++fast;
            if (memcmp(a.c, b.c, 4) != 0) { if (diffs < 4) Print("  colour diff\n"); ++diffs; }
        }
    }
    Print("cpu24 test: colours "); PrintU(total); Print(" (scales all 1.0f: "); PrintU(fast); Print("), differences "); PrintU(diffs);
    Print("\n");
    const bool ok = diffs == 0;
    Print(ok ? "ok\n" : "FAIL\n");
    Exit(ok ? 0 : 1);
    return 0;
}
#ifdef NP_CPU24_FREESTANDING
extern "C" __attribute__((naked)) void _start() { __asm__ volatile("bl main"); }
#endif
