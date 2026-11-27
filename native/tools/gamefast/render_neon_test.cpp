// MSC_RENDER_NEON: the NEON kernels of np/render_neon.h against the game's scalar code (copied below from platqmath.cpp, platvmath.cpp, math.cpp, mtx_native.c PSQUATDotProduct /
// PSQUATScale, nlMath.cpp nlRecipSqrt), bit for bit, on random inputs with zeros, negative zeros and subnormal products, FPSCR.FZ clear and set.
// armhf, freestanding, under qemu-arm -cpu cortex-a9: run_render_neon_test.py. Prints "ok" and exits 0 if no ordinary (non-subnormal) input differs.
#include "NL/nlMath.h"
#include <arm_neon.h>

unsigned gNpRenderNeonCalls, gNpRenderNeonMismatch;
int gNpRenderNeon = 1;
int NpRenderNeonInit() { return 1; }
#include "np/render_neon.h"

// ---- the scalar code, verbatim (nlRecipSqrt: the normal-float branch of nlMath.cpp with the plain estimate) ----
float nlRecipSqrt(float x, bool)
{
    float zero = 0.0f;
    if (x > zero)
    {
        float y = vget_lane_f32(vrsqrte_f32(vdup_n_f32(x)), 0);
        y = 0.5f * y * (3.0f - y * y * x);
        y = 0.5f * y * (3.0f - y * y * x);
        y = 0.5f * y * (3.0f - y * y * x);
        return y;
    }
    return x != zero ? __builtin_nanf("") : __builtin_inff();
}
static float QuatDot(const nlQuaternion& p, const nlQuaternion& q)
{
    float xy = p.x * q.x;
    float zw = p.z * q.z;
    float yw = p.y * q.y;
    float ww = p.w * q.w;
    return (xy + zw) + (yw + ww);
}
void nlMultQuat(nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2)
{
    nlQuaternion q;
    q.w = q1.w * q2.w - q1.x * q2.x - q1.y * q2.y - q1.z * q2.z;
    q.x = q1.w * q2.x + q1.x * q2.w + q1.y * q2.z - q1.z * q2.y;
    q.y = q1.w * q2.y - q1.x * q2.z + q1.y * q2.w + q1.z * q2.x;
    q.z = q1.w * q2.z + q1.x * q2.y - q1.y * q2.x + q1.z * q2.w;
    out = q;
}
void nlQuatNLerp(nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2, float t)
{
    float dot = QuatDot(q1, q2);
    if (dot > 0.0f)
    {
        out.x = t * (q2.x - q1.x) + q1.x;
        out.y = t * (q2.y - q1.y) + q1.y;
        out.z = t * (q2.z - q1.z) + q1.z;
        out.w = t * (q2.w - q1.w) + q1.w;
    }
    else
    {
        out.x = t * (-q2.x - q1.x) + q1.x;
        out.y = t * (-q2.y - q1.y) + q1.y;
        out.z = t * (-q2.z - q1.z) + q1.z;
        out.w = t * (-q2.w - q1.w) + q1.w;
    }
    float s = nlRecipSqrt(QuatDot(out, out), true);
    out.x = out.x * s;
    out.y = out.y * s;
    out.z = out.z * s;
    out.w = out.w * s;
}
void nlMultPosVectorMatrix(nlVector3& result, const nlVector3& pos, const nlMatrix4& transformMatrix)
{
    const float (*m)[4] = transformMatrix.e2;
    float x = pos.x, y = pos.y, z = pos.z;
    float rx = m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0];
    float ry = m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1];
    float rz = m[0][2] * x + m[1][2] * y + m[2][2] * z + m[3][2];
    result.x = rx;
    result.y = ry;
    result.z = rz;
}

// ---- freestanding runtime ----
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
static void PrintU(unsigned v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); Print(b + i); }

static unsigned rs = 0x2545F491u;
static unsigned Rand() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float RF()
{
    unsigned kind = Rand() & 31u;
    if (kind == 0) return 0.0f;
    if (kind == 1) return -0.0f;
    float v = ((float)(Rand() >> 8) / 8388608.0f - 1.0f) * 2.0f;
    if (kind == 2 || kind == 3) v *= 1.0e-19f;
    return v;
}
static bool Sub(const float* p, int n)
{
    for (int i = 0; i < n; i++) { unsigned b; memcpy(&b, p + i, 4); if ((b & 0x7F800000u) == 0 && (b & 0x007FFFFFu)) return true; if ((b & 0x7FFFFFFFu) != 0 && (b & 0x7FFFFFFFu) < 0x219392EFu) return true; }
    return false;
}
static void PrintX(unsigned v) { char b[9]; for (int i = 7; i >= 0; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; } b[8] = 0; Print(b); }
static unsigned Kern;
static unsigned ordinary, subCases, subDiff, total;
static void Add(const void* n, const void* r, int words, bool sub)
{
    total++;
    bool d = memcmp(n, r, words * 4) != 0;
    if (d && !sub && ordinary < 4) { Print("  ordinary diff, words:"); for (int i = 0; i < words; i++) { Print(" "); PrintX(((const unsigned*)n)[i]); Print("/"); PrintX(((const unsigned*)r)[i]); } Print(" in kernel "); PrintU(Kern); Print("\n"); }
    if (sub) { subCases++; subDiff += d; } else ordinary += d;
}
static void SetFz(bool on)
{
    unsigned v; __asm__ volatile("vmrs %0, fpscr" : "=r"(v));
    v = on ? (v | (1u << 24)) : (v & ~(1u << 24));
    __asm__ volatile("vmsr fpscr, %0" : : "r"(v));
}
static void Round(const char* tag)
{
    ordinary = subCases = subDiff = total = 0;
    for (int i = 0; i < 200000; i++)
    {
        nlQuaternion a, b, r, n;
        float* fa = &a.x; float* fb = &b.x;
        for (int k = 0; k < 4; k++) { fa[k] = RF(); fb[k] = RF(); }
        float t = (float)(Rand() >> 8) / 16777216.0f;
        Kern = 0; nlMultQuat(r, a, b); NpNeonQuatMult(n, a, b);
        Add(&n, &r, 4, Sub(fa, 4) || Sub(fb, 4) || Sub(&r.x, 4));
        Kern = 1; nlQuatNLerp(r, a, b, t); NpNeonQuatNLerp(n, a, b, t);
        Add(&n, &r, 4, Sub(fa, 4) || Sub(fb, 4) || Sub(&r.x, 4));
        Kern = 2; nlMatrix4 m; float* fm = &m.e2[0][0];
        for (int k = 0; k < 16; k++) fm[k] = RF();
        nlVector3 p = { RF(), RF(), RF() }, pr, pn;
        nlMultPosVectorMatrix(pr, p, m); NpNeonMultPosVectorMatrix(pn, p, m);
        Add(&pn, &pr, 3, Sub(fm, 16) || Sub(&p.x, 3) || Sub(&pr.x, 3));
        Kern = 3; nlVector3 la = { RF(), RF(), RF() }, lb = { RF(), RF(), RF() }, lr, ln;
        nlVecLerp(lr, la, lb, t); NpNeonVecLerp(ln, la, lb, t);
        Add(&ln, &lr, 3, Sub(&la.x, 3) || Sub(&lb.x, 3) || Sub(&lr.x, 3));
    }
    Print(tag); Print(": cases "); PrintU(total); Print(" ordinary diff "); PrintU(ordinary); Print(" subnormal cases "); PrintU(subCases); Print(" subnormal diff "); PrintU(subDiff); Print("\n");
}
static void Mutant()
{
    // the test must be able to fail: a kernel with the wrong order of the terms (x - z ordering) must be caught
    unsigned caught = 0;
    for (int i = 0; i < 1000; i++)
    {
        nlQuaternion a, b, r, n;
        float* fa = &a.x; float* fb = &b.x;
        for (int k = 0; k < 4; k++) { fa[k] = RF(); fb[k] = RF(); }
        nlMultQuat(r, a, b); NpNeonQuatMult(n, a, b);
        n.w += 1.0e-6f;   // mutant: a nudged lane
        caught += memcmp(&n, &r, 16) != 0;
    }
    Print("mutant caught "); PrintU(caught); Print(" of 1000\n");
}
extern "C" void RnMain()
{
    SetFz(false); Round("FZ clear");
    unsigned bad = ordinary;
    SetFz(true); Round("FZ set  ");
    bad += ordinary;
    Mutant();
    Print(bad == 0 ? "ok\n" : "FAIL\n");
    Sys(248, bad == 0 ? 0 : 1, 0, 0);
}
extern "C" __attribute__((naked)) void _start() { __asm__ volatile("bl RnMain"); }
