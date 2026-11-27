// np_gx_seq16_fill against the writes one by one of np_gx_pipe.h. Run native and, for the NEON path, under qemu-arm (run_gl_render_fast_test.sh).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "np_gx_seqfill.h"

static void Scalar(unsigned char* p, unsigned count, unsigned comps) {
    for (unsigned i = 0; i < count; ++i)
        for (unsigned k = 0; k < comps; ++k) {
            const unsigned v = static_cast<unsigned short>(i);
            p[0] = static_cast<unsigned char>(v >> 8);
            p[1] = static_cast<unsigned char>(v);
            p += 2;
        }
}
int main() {
    static const unsigned kCounts[] = {0, 1, 2, 7, 8, 9, 15, 16, 17, 255, 256, 257, 1000, 4096, 65535};
    unsigned long checks = 0;
    for (unsigned comps = 1; comps <= 6; ++comps)
        for (unsigned count : kCounts)
            for (unsigned misalign = 0; misalign < 4; ++misalign) {
                const size_t bytes = size_t(count) * comps * 2;
                std::vector<unsigned char> a(bytes + 32, 0xA5), b(bytes + 32, 0xA5);
                np_gx_seq16_fill(a.data() + 8 + misalign, count, comps);
                Scalar(b.data() + 8 + misalign, count, comps);
                if (a != b) { std::printf("FAIL comps %u count %u misalign %u\n", comps, count, misalign); return 1; }
                ++checks;
            }
    std::printf("gx_seqfill ok (%lu checks, neon %d)\n", checks,
#if defined(__ARM_NEON)
                1
#else
                0
#endif
    );
}
