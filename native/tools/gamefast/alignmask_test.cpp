// MSC_ALLOC_MASK: the mask form MemoryAllocator::AllocateFromStart uses against nlAlignUp (value % alignment), for every power-of-two alignment 1..2^31
// with values at every residue near each multiple, near 0 and near 2^32 (wrap-around), and 20 million random values; and the guard (0 and non powers of two take the divide).
#include <stdio.h>
#include <stdlib.h>
#include <initializer_list>
typedef unsigned int u32;
static inline u32 AlignUp(u32 value, u32 alignment)
{
    u32 remainder = value % alignment;
    return value + (remainder != 0) * (alignment - remainder);
}
static inline u32 AlignMask(u32 v, u32 alignment) { const u32 m = alignment - 1; return (v + m) & ~m; }
static inline bool UseMask(u32 alignment) { return alignment != 0 && (alignment & (alignment - 1)) == 0; }
int main()
{
    unsigned long long n = 0, bad = 0;
    for (int b = 0; b < 32; ++b)
    {
        u32 a = 1u << b;
        if (!UseMask(a)) { printf("guard wrong for %u\n", a); return 1; }
        for (u32 k = 0; k < 3000; ++k)
            for (int side = 0; side < 2; ++side)
            {
                u32 base = side ? 0u - k * a : k * a;   // multiples of a from the bottom and the top of the range (the top wraps)
                for (int d = -70; d <= 70; ++d)
                {
                    u32 v = base + (u32)d;
                    ++n; bad += AlignUp(v, a) != AlignMask(v, a);
                }
            }
        for (u32 v = 0xFFFFFFFFu - 4096; v != 0 ; ++v) { ++n; bad += AlignUp(v, a) != AlignMask(v, a); }
        ++n; bad += AlignUp(0, a) != AlignMask(0, a);
    }
    srand(12345);
    for (int i = 0; i < 20000000; ++i)
    {
        u32 v = ((u32)rand() << 16) ^ (u32)rand() ^ ((u32)rand() << 31);
        u32 a = 1u << (rand() & 31);
        ++n; bad += AlignUp(v, a) != AlignMask(v, a);
    }
    for (u32 a : { 0u, 3u, 5u, 6u, 12u, 24u, 0x80000001u, 0xFFFFFFFFu, 0xC0000000u }) if (UseMask(a)) { printf("guard accepts %u\n", a); return 1; }
    printf("alignmask: %llu values, %llu differences\n", n, bad);
    return bad != 0;
}
