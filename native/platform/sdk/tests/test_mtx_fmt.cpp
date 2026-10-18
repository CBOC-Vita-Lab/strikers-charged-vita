// Unit tests of the math routines (MTX/VEC/QUAT) and of the big-endian containers ARC and TPL.
#include <revolution/arc.h>
#include <revolution/mtx.h>
#include <revolution/os.h>
#include <revolution/tpl.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "np/np_sdk.h"

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static bool near(float a, float b, float eps = 1e-5f) { return fabsf(a - b) <= eps * (1.0f + fabsf(b)); }

static void test_mtx() {
    Mtx a, b, c, inv, id;
    PSMTXRotRad(a, 'y', 0.7f);
    PSMTXTrans(b, 1.0f, -2.0f, 3.0f);
    PSMTXConcat(a, b, c);   // rotate after translate
    Vec p = {1.0f, 2.0f, 3.0f}, q;
    PSMTXMultVec(c, &p, &q);
    // by hand: t = p + (1,-2,3) = (2,0,6); rotate about y by 0.7
    float x = 2.0f, z = 6.0f;
    CHECK(near(q.x, cosf(0.7f) * x + sinf(0.7f) * z));
    CHECK(near(q.y, 0.0f));
    CHECK(near(q.z, -sinf(0.7f) * x + cosf(0.7f) * z));
    CHECK(PSMTXInverse(c, inv) == 1);
    PSMTXConcat(inv, c, id);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) CHECK(near(id[i][j], i == j ? 1.0f : 0.0f, 1e-4f));
    // aliasing
    Mtx d;
    PSMTXCopy(a, d);
    PSMTXConcat(d, b, d);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) CHECK(near(d[i][j], c[i][j]));
    // normal matrix of a pure scale is 1/scale
    Mtx s, n;
    PSMTXScale(s, 2.0f, 4.0f, 8.0f);
    CHECK(PSMTXInvXpose(s, n) == 1);
    CHECK(near(n[0][0], 0.5f) && near(n[1][1], 0.25f) && near(n[2][2], 0.125f) && n[0][3] == 0.0f);
    Mtx z0;
    memset(z0, 0, sizeof(z0));
    CHECK(PSMTXInvXpose(z0, n) == 0);
    // a singular matrix (W5 LOOK2): the result is still 0, and the output is written (the cofactor matrix times 2^25), not left as it was
    {
        memset(n, 0xAB, sizeof(n));
        CHECK(PSMTXInvXpose(z0, n) == 0);
        bool zeros = true;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j) zeros = zeros && n[i][j] == 0.0f;
        CHECK(zeros);
        Mtx rk2 = {{1, 2, 3, 0}, {4, 5, 6, 0}, {7, 8, 9, 0}}, c;
        memset(c, 0xAB, sizeof(c));
        CHECK(PSMTXInvXpose(rk2, c) == 0);
        const float k = 33554432.0f;   // 2^25; the cofactors of this matrix are (-3, 6, -3), (6, -12, 6), (-3, 6, -3)
        CHECK(c[0][0] == -3.0f * k && c[0][1] == 6.0f * k && c[0][2] == -3.0f * k && c[0][3] == 0.0f);
        CHECK(c[1][0] == 6.0f * k && c[1][1] == -12.0f * k && c[1][2] == 6.0f * k && c[1][3] == 0.0f);
        CHECK(c[2][0] == -3.0f * k && c[2][1] == 6.0f * k && c[2][2] == -3.0f * k && c[2][3] == 0.0f);
    }

    Mtx44 o, pr, i44, t44;
    C_MTXOrtho(o, 1.0f, -1.0f, -2.0f, 2.0f, 0.0f, 10.0f);
    CHECK(near(o[0][0], 0.5f) && near(o[1][1], 1.0f) && near(o[2][2], -0.1f) && o[3][3] == 1.0f);
    C_MTXPerspective(pr, 90.0f, 1.5f, 1.0f, 100.0f);
    CHECK(near(pr[1][1], 1.0f, 1e-4f) && near(pr[0][0], 1.0f / 1.5f, 1e-4f) && pr[3][2] == -1.0f);
    CHECK(C_MTX44Inverse(pr, i44) == 1);
    Mtx44 prod;
    PSMTX44Concat(i44, pr, prod);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) CHECK(near(prod[i][j], i == j ? 1.0f : 0.0f, 2e-3f));
    PSMTX44Transpose(pr, t44);
    CHECK(t44[2][3] == pr[3][2] && t44[3][2] == pr[2][3]);

    Quaternion qa = {0, 0, 0, 1}, qb = {0, sinf(0.5f), 0, cosf(0.5f)}, qr;
    C_QUATSlerp(&qa, &qb, &qr, 0.5f);
    CHECK(near(qr.y, sinf(0.25f), 1e-4f) && near(qr.w, cosf(0.25f), 1e-4f));
    CHECK(near(PSQUATDotProduct(&qa, &qb), cosf(0.5f)));
    Mtx qm;
    PSMTXQuat(qm, &qb);
    Mtx ry;
    PSMTXRotRad(ry, 'y', 1.0f);   // quaternion half angle 0.5
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) CHECK(near(qm[i][j], ry[i][j], 1e-4f));
    Quaternion back;
    C_QUATMtx(&back, ry);
    CHECK(near(back.y, qb.y, 1e-4f) && near(back.w, qb.w, 1e-4f));
}

// ---- ARC: a U8 archive as the disc has it (big-endian words) ---------------------------------------
static void be32(unsigned char* p, unsigned v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void be16(unsigned char* p, unsigned v) { p[0] = v >> 8; p[1] = v; }

static void test_arc() {
    // root dir, "a" dir containing "x.bin", and "y.bin" at the top
    static unsigned char buf[512] __attribute__((aligned(32)));
    memset(buf, 0, sizeof(buf));
    be32(buf + 0, 0x55AA382D);
    be32(buf + 4, 0x20);       // fst
    be32(buf + 8, 4 * 12 + 16);
    be32(buf + 12, 0x80);      // file data
    unsigned char* fst = buf + 0x20;
    // names: "\0a\0x.bin\0y.bin\0" at fst + 48
    const char names[] = "\0a\0x.bin\0y.bin";
    memcpy(fst + 48, names, sizeof(names));
    be32(fst + 0, 0x01000000); be32(fst + 4, 0); be32(fst + 8, 4);            // 0: root, entries 0..3
    be32(fst + 12, 0x01000001); be32(fst + 16, 0); be32(fst + 20, 3);         // 1: dir "a" (name offset 1), next = 3
    be32(fst + 24, 0x00000003); be32(fst + 28, 0x80); be32(fst + 32, 5);      // 2: "x.bin" at 0x80, 5 bytes
    be32(fst + 36, 0x00000009); be32(fst + 40, 0x90); be32(fst + 44, 7);      // 3: "y.bin" at 0x90, 7 bytes
    memcpy(buf + 0x80, "hello", 5);
    memcpy(buf + 0x90, "goodbye", 7);

    ARCHandle h;
    CHECK(ARCInitHandle(buf, &h));
    CHECK(h.entryNum == 4);
    ARCFileInfo f;
    CHECK(ARCOpen(&h, "a/x.bin", &f));
    CHECK(ARCGetLength(&f) == 5);
    CHECK(memcmp(ARCGetStartAddrInMem(&f), "hello", 5) == 0);
    CHECK(ARCClose(&f));
    CHECK(ARCOpen(&h, "/Y.BIN", &f) && ARCGetLength(&f) == 7);
    CHECK(!ARCOpen(&h, "nope", &f));
    ARCHandle h2;   // initialising a second time (already converted)
    CHECK(ARCInitHandle(buf, &h2) && h2.entryNum == 4);
    ARCDir dir;
    ARCDirEntry ent;
    CHECK(ARCOpenDir(&h, "/", &dir));
    int n = 0;
    while (ARCReadDir(&dir, &ent)) ++n;
    CHECK(n == 2);   // "a" and "y.bin"
}

// ---- TPL ------------------------------------------------------------------------------------------
static void test_tpl() {
    static unsigned char buf[256] __attribute__((aligned(32)));
    memset(buf, 0, sizeof(buf));
    be32(buf + 0, 2142000);
    be32(buf + 4, 1);
    be32(buf + 8, 0x0C);           // descriptor array
    be32(buf + 0x0C, 0x14);        // texture header
    be32(buf + 0x10, 0x40);        // CLUT header
    unsigned char* th = buf + 0x14;
    be16(th + 0, 64);              // height
    be16(th + 2, 128);             // width
    be32(th + 4, 14);              // format
    be32(th + 8, 0x80);            // data
    be32(th + 12, 1);
    be32(th + 16, 1);
    be32(th + 20, 1);
    be32(th + 24, 1);
    be32(th + 28, 0x3f800000);     // LOD bias 1.0
    unsigned char* ch = buf + 0x40;
    be16(ch + 0, 16);
    be32(ch + 4, 2);
    be32(ch + 8, 0xC0);
    TPLPalette* pal = (TPLPalette*)buf;
    TPLBind(pal);
    TPLDescriptor* d = TPLGet(pal, 0);
    CHECK(d->textureHeader->height == 64 && d->textureHeader->width == 128);
    CHECK(d->textureHeader->format == 14);
    CHECK((unsigned char*)d->textureHeader->data == buf + 0x80);
    CHECK(d->textureHeader->LODBias == 1.0f);
    CHECK(d->CLUTHeader->numEntries == 16 && (unsigned char*)d->CLUTHeader->data == buf + 0xC0);
}

int main() {
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    OSInit();
    printf("mtx\n"); test_mtx();
    printf("arc\n"); test_arc();
    printf("tpl\n"); test_tpl();
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_mtx_fmt: ok\n");
    return 0;
}
