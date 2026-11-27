// MSC_VGX_LADDER_ALIAS: the shared depth block table of vitaGL 0044 (depth_share_core.h, the code the console runs) and the host model's window arithmetic (vgx_aliasdepth.h).
// Run by run_depthshare_test.sh; the mutant run (the A2 clears removed from a copy of the header) must FAIL.
#include <cstdio>
#include <cstdlib>
#ifdef DEPTH_SHARE_CORE
#include DEPTH_SHARE_CORE
#else
#include "../../dependencies/vitaGL/source/utils/depth_share_core.h"
#endif
#include "../src/vgx_aliasdepth.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// what vgl_depth_share_get does around the core: find or make the block, then join
static int Get(vgl_dshare_tbl& t, int cw, int ch, int mode, void* mem) {
    int i = vgl_ds_find(&t, mode, cw, ch);
    if (i < 0) {
        i = vgl_ds_free_slot(&t);
        if (i < 0) { ++t.failed; return -1; }
        vgl_ds_add(&t, i, mem, mode, cw, ch);
    }
    vgl_ds_join(&t, i);
    return i;
}

int main() {
    char blk[4];   // addresses only
    // 1. a block per (sample mode, capacity): sample counts are respected
    {
        vgl_dshare_tbl t = {};
        const int a = Get(t, 960, 544, 0, &blk[0]);   // one sample
        const int b = Get(t, 960, 544, 1, &blk[1]);   // two samples
        const int c = Get(t, 960, 544, 0, &blk[2]);   // another one-sample user: the same block
        const int d = Get(t, 912, 544, 0, &blk[3]);   // another capacity: its own block
        CHECK(a >= 0 && b >= 0 && a != b);
        CHECK(c == a);
        CHECK(d >= 0 && d != a && d != b);
        CHECK(t.made == 3 && t.joined == 4);
        CHECK(t.rec[a].refs == 2 && t.rec[b].refs == 1);
        CHECK(t.rec[a].mem == &blk[0]);   // the second one-sample user did not make a block of its own
    }
    // 2. refcount: a block is freed with its last user, never before; a surface that is not shared is the caller's
    {
        vgl_dshare_tbl t = {};
        const int a = Get(t, 960, 544, 0, &blk[0]);
        Get(t, 960, 544, 0, &blk[1]);
        Get(t, 960, 544, 0, &blk[2]);
        CHECK(t.rec[a].refs == 3);
        CHECK(vgl_ds_release(&t, &blk[0]) == 0);
        CHECK(vgl_ds_release(&t, &blk[0]) == 0);
        CHECK(t.rec[a].mem == &blk[0]);
        CHECK(vgl_ds_release(&t, &blk[0]) == 1);          // the last user: freed
        CHECK(t.rec[a].mem == 0 && vgl_ds_free_slot(&t) == 0);
        CHECK(vgl_ds_release(&t, &blk[0]) == -1);         // gone: not a shared block any more
        CHECK(vgl_ds_release(&t, &blk[3]) == -1);         // a block of its own
        // and a new block can be made in the freed slot
        CHECK(Get(t, 960, 544, 0, &blk[1]) == 0 && t.made == 2);
    }
    // 3. the table is bounded: no slot -> the join is refused (the object keeps its own surface)
    {
        vgl_dshare_tbl t = {};
        static char many[VGL_DSHARE_MAX + 2];
        for (int i = 0; i < VGL_DSHARE_MAX; ++i) CHECK(Get(t, 100 + 32 * i, 100, 0, &many[i]) == i);
        CHECK(Get(t, 5000, 100, 0, &many[VGL_DSHARE_MAX]) == -1 && t.failed == 1);
        CHECK(Get(t, 100, 100, 0, &many[VGL_DSHARE_MAX + 1]) == 0);   // an existing capacity still joins
    }
    // 4. A2: "the user whose scene began last" is not an address that outlives its object. A framebuffer object's slot is reused after a delete (a new object of another size at the same
    // address) and an object that is resized gives up its depth and joins again: the new context must be seen as a change of user, so the mask-keep record is forgotten.
    {
        vgl_dshare_tbl t = {};
        char x, y;                                   // object addresses
        const int a = Get(t, 960, 544, 0, &blk[0]);
        Get(t, 960, 544, 0, &blk[0]);
        CHECK(vgl_ds_scene(&t, &blk[0], &x) == 1);   // first scene: no context known
        CHECK(vgl_ds_scene(&t, &blk[0], &x) == 0);   // the same context again: nothing to forget
        CHECK(vgl_ds_scene(&t, &blk[0], &y) == 1);   // another object: forget
        CHECK(vgl_ds_scene(&t, &blk[0], &y) == 0);
        // x is deleted, its address reused by a new object (another size) which joins and draws first thing: that is a new context although the address equals last time's
        CHECK(vgl_ds_scene(&t, &blk[0], &x) == 1);
        CHECK(vgl_ds_release(&t, &blk[0]) == 0);     // x leaves
        Get(t, 960, 544, 0, &blk[0]);                // the new object at x's address joins
        CHECK(vgl_ds_scene(&t, &blk[0], &x) == 1);   // A2: must be a change (the stale address compare said 0)
        // the same for an object that is resized: leave and join again, same address
        CHECK(vgl_ds_scene(&t, &blk[0], &x) == 0);
        CHECK(vgl_ds_release(&t, &blk[0]) == 0);
        Get(t, 960, 544, 0, &blk[0]);
        CHECK(vgl_ds_scene(&t, &blk[0], &x) == 1);
        CHECK(a == 0 && t.rec[a].refs == 2);
        CHECK(vgl_ds_scene(&t, &blk[3], &x) == 0);   // not a shared block
    }
    // 5. the window: a w x h target uses the first h rows of rows cw wide
    {
        using namespace vgx::aliasdepth;
        CHECK(Fits(960, 544, 960, 544) && Fits(704, 400, 960, 544) && !Fits(961, 544, 960, 544) && !Fits(960, 545, 960, 544) && !Fits(1, 1, 0, 0));
        CHECK(WinU(960, 960) == 1.0f && WinV(544, 544) == 1.0f);
        CHECK(WinU(480, 960) == 0.5f && WinV(272, 544) == 0.5f);
        CHECK(WinU(1200, 960) == 1.0f && WinV(0, 544) == 1.0f);   // outside the capacity: no window (the target has its own texture)
        // the console's row: VGL_ALIGN(capacity, 32) samples; a picture u of w / aligned(cw) is the effects pass's u (A1: not VGL_ALIGN(w, 32))
        const int cw = 960, w = 800;
        const int rowOwn = (w + 31) & ~31, rowShared = (cw + 31) & ~31;
        CHECK(rowOwn == 800 && rowShared == 960);
        CHECK(static_cast<float>(w) / rowShared < 1.0f && rowOwn != rowShared);   // reading a shared surface with its own width's row would shear it (A1)
    }
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("depthshare: all checks passed\n");
    return 0;
}
