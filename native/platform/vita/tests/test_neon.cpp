// Tests and instruction counts of the NEON kernels (neon/np_neon.h) against their scalar twins.
//   armhf (qemu, the real kernels):  make -C platform/vita TARGET=armhf neon
//   x86-64 (the twins stand in for the kernels: checks the harness of this test only)
// Argument: "count" runs only the counting loops between qemu marks (for tools/qemu-plugin).
#include "np_neon.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
static void Mark(long n) { syscall(0x7f00, n); }
#else
static void Mark(long) {}
#endif

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static uint32_t g_rng = 20261009;
static float Rand(float lo, float hi) { g_rng = g_rng * 1664525u + 1013904223u; return lo + (hi - lo) * ((g_rng >> 8) / 16777216.0f); }
// relative difference with an absolute floor: fused multiply-add and a different summation order change the last bits
static bool Near(const float* a, const float* b, int n, float tol = 2e-5f) {
    for (int i = 0; i < n; ++i) {
        const float d = std::fabs(a[i] - b[i]), s = std::fabs(a[i]) + std::fabs(b[i]) + 1.0f;
        if (!(d <= tol * s)) { std::printf("  mismatch at %d: %g vs %g\n", i, a[i], b[i]); return false; }
    }
    return true;
}

int main(int argc, char** argv) {
    std::printf("NEON in this build: %d\n", NP_HAVE_NEON);
    const bool countOnly = argc > 1 && !std::strcmp(argv[1], "count");
    if (!countOnly) {
        for (int it = 0; it < 2000; ++it) {
            alignas(16) float a[4][4], b[4][4], r1[4][4], r2[4][4];
            for (int i = 0; i < 16; ++i) { (&a[0][0])[i] = Rand(-3, 3); (&b[0][0])[i] = Rand(-3, 3); }
            ref_mtx44_concat(a, b, r1); np_mtx44_concat(a, b, r2);
            CHECK(Near(&r1[0][0], &r2[0][0], 16));
            // 3x4 with the implied last row; aliasing out == a
            float a3[3][4], b3[3][4], c1[3][4], c2[3][4];
            std::memcpy(a3, a, sizeof(a3)); std::memcpy(b3, b, sizeof(b3));
            ref_mtx34_concat(a3, b3, c1); std::memcpy(c2, a3, sizeof(c2)); np_mtx34_concat(c2, b3, c2);
            CHECK(Near(&c1[0][0], &c2[0][0], 12));
            float p[3] = {Rand(-9, 9), Rand(-9, 9), Rand(-9, 9)}, o1[3], o2[3];
            ref_pos_mtx44(o1, p, a); np_pos_mtx44(o2, p, a);
            CHECK(Near(o1, o2, 3));
            float q1[4] = {Rand(-1, 1), Rand(-1, 1), Rand(-1, 1), Rand(-1, 1)}, q2[4] = {Rand(-1, 1), Rand(-1, 1), Rand(-1, 1), Rand(-1, 1)}, m1[4], m2[4];
            ref_quat_mul(m1, q1, q2); np_quat_mul(m2, q1, q2);
            CHECK(Near(m1, m2, 4));
            const float t = Rand(0, 1);
            ref_quat_nlerp(m1, q1, q2, t); np_quat_nlerp(m2, q1, q2, t);
            CHECK(Near(m1, m2, 4, 1e-5f));
            float len = m2[0] * m2[0] + m2[1] * m2[1] + m2[2] * m2[2] + m2[3] * m2[3];
            CHECK(std::fabs(len - 1.0f) < 1e-5f);
        }
        // array transform
        {
            alignas(16) float m[4][4];
            for (int i = 0; i < 16; ++i) (&m[0][0])[i] = Rand(-2, 2);
            std::vector<float> in(3 * 257), o1(3 * 257), o2(3 * 257);
            for (auto& x : in) x = Rand(-5, 5);
            for (int i = 0; i < 257; ++i) ref_pos_mtx44(&o1[3 * i], &in[3 * i], m);
            np_pos_mtx44_array(o2.data(), in.data(), 257, m);
            CHECK(Near(o1.data(), o2.data(), 3 * 257));
        }
        // skinning loop: random vertices, a bone matrix, repeated indices (a vertex with several weights), with and without morph
        {
            const uint32_t V = 700, W = 1500;
            std::vector<float> in(3 * V), morph(3 * V), o1(3 * V, 0.25f), o2(3 * V, 0.25f);
            for (auto& x : in) x = Rand(-3, 3);
            for (auto& x : morph) x = Rand(-0.1f, 0.1f);
            std::vector<NpSkinWeight> w(W);
            for (auto& e : w) { g_rng = g_rng * 1664525u + 1013904223u; e.vertexIndex = (g_rng >> 8) % V; e.vertexWeight = Rand(0, 1); }
            alignas(16) float M[4][4];
            for (int i = 0; i < 16; ++i) (&M[0][0])[i] = Rand(-2, 2);
            for (int withMorph = 0; withMorph < 2; ++withMorph) {
                std::fill(o1.begin(), o1.end(), 0.25f); std::fill(o2.begin(), o2.end(), 0.25f);
                ref_skin_accumulate(o1.data(), in.data(), withMorph ? morph.data() : nullptr, M, w.data(), W);
                np_skin_accumulate(o2.data(), in.data(), withMorph ? morph.data() : nullptr, M, w.data(), W);
                CHECK(Near(o1.data(), o2.data(), 3 * V, 5e-5f));
            }
        }
        // frustum: random planes (unit normals), random spheres: exact agreement except at the boundary, so count disagreements
        {
            int disagree = 0, total = 0;
            for (int round = 0; round < 200; ++round) {
                float planes[6][4];
                for (int p = 0; p < 6; ++p) {
                    float n[3] = {Rand(-1, 1), Rand(-1, 1), Rand(-1, 1)};
                    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-3f;
                    planes[p][0] = n[0] / l; planes[p][1] = n[1] / l; planes[p][2] = n[2] / l; planes[p][3] = Rand(2, 12);
                }
                const int N = 259;
                std::vector<float> sp(4 * N);
                for (int i = 0; i < N; ++i) { sp[4 * i] = Rand(-12, 12); sp[4 * i + 1] = Rand(-12, 12); sp[4 * i + 2] = Rand(-12, 12); sp[4 * i + 3] = Rand(0.2f, 3.5f); }
                std::vector<uint8_t> out(N);
                np_sphere_frustum_batch(planes, sp.data(), N, out.data());
                for (int i = 0; i < N; ++i, ++total) {
                    const int ref = ref_sphere_frustum(planes, &sp[4 * i], sp[4 * i + 3]);
                    const int single = np_sphere_frustum(planes, &sp[4 * i], sp[4 * i + 3]);
                    if (ref != out[i] || ref != single) ++disagree;
                }
            }
            std::printf("frustum: %d disagreements in %d spheres (rounding at the plane boundary only)\n", disagree, total);
            CHECK(disagree * 1000 < total);
        }
    }
    // ---- instruction counting loops (qemu marks: the plugin prints the cumulative counts at each mark) ----
    {
        alignas(16) float a[4][4], b[4][4], r[4][4];
        for (int i = 0; i < 16; ++i) { (&a[0][0])[i] = Rand(-1, 1); (&b[0][0])[i] = Rand(-1, 1); }
        float a3[3][4], b3[3][4]; std::memcpy(a3, a, sizeof(a3)); std::memcpy(b3, b, sizeof(b3));
        float q1[4] = {0.1f, 0.2f, 0.3f, 0.9f}, q2[4] = {0.4f, 0.1f, -0.2f, 0.8f}, qo[4], p[3] = {1, 2, 3}, po[3];
        std::vector<float> in(3 * 1024, 0.5f), out(3 * 1024);
        float planes[6][4] = {{1, 0, 0, 5}, {-1, 0, 0, 5}, {0, 1, 0, 5}, {0, -1, 0, 5}, {0, 0, 1, 5}, {0, 0, -1, 5}};
        std::vector<float> sp(4 * 1024);
        for (int i = 0; i < 1024; ++i) { sp[4 * i] = Rand(-8, 8); sp[4 * i + 1] = Rand(-8, 8); sp[4 * i + 2] = Rand(-8, 8); sp[4 * i + 3] = 1; }
        std::vector<uint8_t> res(1024);
        const int N = 20000;
        long mark = 100;
        #define LOOP(name, call) do { Mark(mark++); for (int i = 0; i < N; ++i) { call; asm volatile("" :: "r"(r), "r"(qo), "r"(po) : "memory"); } Mark(mark++); std::printf("counted %s\n", name); } while (0)
        LOOP("ref mtx44 concat", ref_mtx44_concat(a, b, r));
        LOOP("neon mtx44 concat", np_mtx44_concat(a, b, r));
        LOOP("ref mtx34 concat", ref_mtx34_concat(a3, b3, a3));
        LOOP("neon mtx34 concat", np_mtx34_concat(a3, b3, a3));
        LOOP("ref pos*mtx44", ref_pos_mtx44(po, p, a));
        LOOP("neon pos*mtx44", np_pos_mtx44(po, p, a));
        LOOP("ref quat mul", ref_quat_mul(qo, q1, q2));
        LOOP("ref quat nlerp", ref_quat_nlerp(qo, q1, q2, 0.3f));
        LOOP("neon quat nlerp", np_quat_nlerp(qo, q1, q2, 0.3f));
        // per element figures: 1024 positions / 1024 spheres per call, N/1000 calls
        Mark(mark++); for (int i = 0; i < N / 100; ++i) { for (int k = 0; k < 1024; ++k) ref_pos_mtx44(&out[3 * k], &in[3 * k], a); asm volatile("" ::: "memory"); } Mark(mark++);
        Mark(mark++); for (int i = 0; i < N / 100; ++i) { np_pos_mtx44_array(out.data(), in.data(), 1024, a); asm volatile("" ::: "memory"); } Mark(mark++);
        Mark(mark++); for (int i = 0; i < N / 100; ++i) { for (int k = 0; k < 1024; ++k) res[k] = (uint8_t)ref_sphere_frustum(planes, &sp[4 * k], sp[4 * k + 3]); asm volatile("" ::: "memory"); } Mark(mark++);
        Mark(mark++); for (int i = 0; i < N / 100; ++i) { np_sphere_frustum_batch(planes, sp.data(), 1024, res.data()); asm volatile("" ::: "memory"); } Mark(mark++);
        {
            // skinning: 1500 weights x (N/100) calls
            std::vector<float> in(3 * 700, 0.5f), o(3 * 700, 0.f), mo(3 * 700, 0.01f);
            std::vector<NpSkinWeight> w(1500);
            for (int i = 0; i < 1500; ++i) { w[i].vertexIndex = (uint32_t)(i * 7919) % 700; w[i].vertexWeight = 0.5f; }
            Mark(mark++); for (int i = 0; i < N / 100; ++i) { ref_skin_accumulate(o.data(), in.data(), nullptr, a, w.data(), 1500); asm volatile("" ::: "memory"); } Mark(mark++);
            Mark(mark++); for (int i = 0; i < N / 100; ++i) { np_skin_accumulate(o.data(), in.data(), nullptr, a, w.data(), 1500); asm volatile("" ::: "memory"); } Mark(mark++);
            Mark(mark++); for (int i = 0; i < N / 100; ++i) { ref_skin_accumulate(o.data(), in.data(), mo.data(), a, w.data(), 1500); asm volatile("" ::: "memory"); } Mark(mark++);
            Mark(mark++); for (int i = 0; i < N / 100; ++i) { np_skin_accumulate(o.data(), in.data(), mo.data(), a, w.data(), 1500); asm volatile("" ::: "memory"); } Mark(mark++);
            std::printf("counted skinning (1500 weights x %d calls: ref, neon, ref+morph, neon+morph)\n", N / 100);
        }
        std::printf("counted arrays (1024 elements x %d calls: ref pos, neon pos, ref sphere, neon sphere batch); results %g %d\n", N / 100, out[5], res[7]);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
