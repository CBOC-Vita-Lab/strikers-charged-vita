// MSC_ODE_BPFULL: NpBpNextFull (NEON when built for ARM, the C loop with -DNP_BP_NO_NEON) against the reference test NpBpRejectFull on random keys, bodies and category bits.
// Every call: the survivors it returns, one after the other from any start, must be exactly the j at which the reference does not reject (np_broadphase.h).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ode/np_broadphase.h"

static uint32_t rng = 1;
static uint32_t Rng() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(int argc, char** argv)
{
    const int rounds = argc > 1 ? atoi(argv[1]) : 20000;
    long checked = 0, survivors = 0, bad = 0, runSkips = 0;
    for (int r = 0; r < rounds; r++)
    {
        const int n = 1 + (int)(Rng() % 40);
        const int cap = n + NP_BP_PAD;
        int* a = (int*)calloc(9 * cap, sizeof(int));
        int *lox = a, *hix = a + cap, *loy = a + 2 * cap, *hiy = a + 3 * cap, *loz = a + 4 * cap, *hiz = a + 5 * cap;
        unsigned *cat = (unsigned*)(a + 6 * cap), *col = (unsigned*)(a + 7 * cap), *body = (unsigned*)(a + 8 * cap);
        const int range = (r & 1) ? 6 : 1000;
        static const unsigned bits[] = { 0xFFFFFFFFu, 1, 2, 4, 0x40, 0xFF, 0x1F05F, 0, 0x20, 0xAF };
        for (int i = 0; i < n; i++)
        {
            int l0 = (int)(Rng() % range) - range / 2, l1 = (int)(Rng() % range) - range / 2, l2 = (int)(Rng() % range) - range / 2;
            lox[i] = l0; hix[i] = l0 + (int)(Rng() % (range / 2 + 1));
            loy[i] = l1; hiy[i] = l1 + (int)(Rng() % (range / 2 + 1));
            loz[i] = l2; hiz[i] = l2 + (int)(Rng() % (range / 2 + 1));
            if ((Rng() & 15) == 0) { lox[i] = -0x7F800000; hix[i] = 0x7F800000; }
            cat[i] = bits[Rng() % 10];
            col[i] = bits[Rng() % 10];
            body[i] = (Rng() & 3) == 0 ? 0 : 0x1000 + 16 * (Rng() % 5);
        }
        NpBpPad(lox, hix, loy, hiy, n);
        NpBpExtra x = { loz, hiz, cat, col, body };
        NpBpPadExtra(x, n);
        for (int i = 0; i < n; i++)
        {
            int from = i + 1 + (int)(Rng() % 3 == 0 ? Rng() % 4 : 0);
            if (from > n) from = n;
            int j = NpBpNextFull(lox, hix, loy, hiy, x, i, from, n);
            for (int k = from; k < n; k++)
            {
                const int rej = NpBpRejectFull(lox, hix, loy, hiy, x, i, k);
                checked++;
                if (!rej) survivors++;
                if (rej == (k == j) || (!rej && k < j) || (rej && k == j)) { /* a survivor must be returned as soon as it comes */ }
                if (!rej)
                {
                    if (j != k) { bad++; printf("round %d i %d: expected %d got %d\n", r, i, k, j); }
                    j = NpBpNextFull(lox, hix, loy, hiy, x, i, k + 1, n);
                }
            }
            if (j != n) { bad++; printf("round %d i %d: expected end got %d\n", r, i, j); }
        }
        // MSC_ODE_BPFULL=2: the same data with the bodies in runs of consecutive geoms (a character's bones), NpBpNextSeg against the reference
        {
            for (int i = 0; i < n;)
            {
                const int len = 1 + (int)(Rng() % 12);
                const unsigned id = (Rng() % 5 == 0) ? 0 : 0x2000 + 16 * (Rng() % 7);
                for (int k = 0; k < len && i < n; k++, i++)
                {
                    body[i] = id;
                    if (id && (Rng() & 1))   // members of a body overlap each other and keep like bits, as bones do
                    {
                        cat[i] = bits[1 + Rng() % 3];
                        col[i] = bits[1 + Rng() % 3];
                    }
                }
            }
            NpBpPad(lox, hix, loy, hiy, n);
            NpBpPadExtra(x, n);
            const int m = n + 1;
            int* si = (int*)calloc(8 * m, sizeof(int));
            unsigned* su = (unsigned*)calloc(2 * m, sizeof(unsigned));
            unsigned char* sr = (unsigned char*)calloc(m, 1);
            NpBpSegs S;
            S.end = si; S.segOf = si + m; S.ulox = si + 2 * m; S.uhix = si + 3 * m; S.uloy = si + 4 * m; S.uhiy = si + 5 * m; S.uloz = si + 6 * m; S.uhiz = si + 7 * m;
            S.ucat = su; S.ucol = su + m; S.run = sr; S.skipped = 0;
            NpBpBuildSegs(S, lox, hix, loy, hiy, x, n, argc > 2 ? atoi(argv[2]) : 0);
            for (int i = 0; i < n; i++)
            {
                int from = i + 1 + (int)(Rng() % 3 == 0 ? Rng() % 4 : 0);
                if (from > n) from = n;
                int j = NpBpNextSeg(lox, hix, loy, hiy, x, S, i, from, n);
                for (int k = from; k < n; k++)
                {
                    const int rej = NpBpRejectFull(lox, hix, loy, hiy, x, i, k);
                    checked++;
                    if (!rej)
                    {
                        survivors++;
                        if (j != k) { bad++; printf("runs round %d i %d: expected %d got %d\n", r, i, k, j); }
                        j = NpBpNextSeg(lox, hix, loy, hiy, x, S, i, k + 1, n);
                    }
                }
                if (j != n) { bad++; printf("runs round %d i %d: expected end got %d\n", r, i, j); }
            }
            runSkips += (long)S.skipped;
            free(si); free(su); free(sr);
        }
        free(a);
    }
    printf("ode bpfull: compared %ld pairs, %ld survivors, wrong %ld (run skips %ld geoms)\n", checked, survivors, bad, runSkips);
    return bad != 0;
}
