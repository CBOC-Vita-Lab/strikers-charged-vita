// The hooks towards the renderer glue (platform/gx): defined here as the glue would define them, the SDK layer must
// call them at the right moments: arenas registered at start-up, cache operations and disc reads reported, the EFB
// size passed on from VIConfigure.
#include <revolution/dvd.h>
#include <revolution/os.h>
#include <revolution/vi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "np/np_sdk.h"

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static struct { const void* p; unsigned n; } g_ram[4];
static int g_nram = 0;
static const void* g_last_w = 0;
static unsigned g_last_n = 0, g_nwrites = 0;
static int g_efb_w = 0, g_efb_h = 0;

extern "C" void np_gx_register_ram(const void* base, unsigned int size) {
    if (g_nram < 4) { g_ram[g_nram].p = base; g_ram[g_nram].n = size; ++g_nram; }
}
extern "C" void np_gx_notify_write(const void* p, unsigned int n) { g_last_w = p; g_last_n = n; ++g_nwrites; }
extern "C" void np_gx_set_efb_size(int w, int h) { g_efb_w = w; g_efb_h = h; }

int main() {
    char root[] = "/tmp/np_gxh_XXXXXX";
    CHECK(mkdtemp(root) != 0);
    char p[512];
    snprintf(p, sizeof p, "%s/files", root); mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/files/a.bin", root);
    FILE* f = fopen(p, "wb");
    for (int i = 0; i < 4096; ++i) fputc(i & 0xFF, f);
    fclose(f);

    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    cfg.disc_dir = root;
    NpSdk_Init(&cfg);
    OSInit();
    printf("  registered %d range(s)\n", g_nram);
    CHECK(g_nram == 2);
    CHECK(g_ram[0].n == 24u * 1024 * 1024 && g_ram[1].n == 64u * 1024 * 1024);
    CHECK(g_ram[0].p == OSGetMEM1ArenaLo() && g_ram[1].p == OSGetMEM2ArenaLo());

    static char buf[256] __attribute__((aligned(32)));
    unsigned before = g_nwrites;
    DCFlushRange(buf, 128);
    CHECK(g_nwrites == before + 1 && g_last_w == buf && g_last_n == 128);
    DCStoreRange(buf + 32, 64);
    CHECK(g_last_w == buf + 32 && g_last_n == 64);
    DCInvalidateRange(buf, 32);
    CHECK(g_last_n == 32);

    DVDInit();
    DVDFileInfo fi;
    CHECK(DVDOpen("a.bin", &fi));
    static unsigned char data[4096] __attribute__((aligned(32)));
    before = g_nwrites;
    CHECK(DVDReadPrio(&fi, data, 4096, 0, 2) == 4096);
    CHECK(g_nwrites > before && g_last_w == data && g_last_n == 4096);
    CHECK(data[5] == 5);
    DVDClose(&fi);

    GXRenderModeObj rm;
    memset(&rm, 0, sizeof rm);
    rm.fbWidth = 640;
    rm.efbHeight = 448;
    rm.xfbHeight = 448;
    VIInit();
    VIConfigure(&rm);
    printf("  efb size %dx%d\n", g_efb_w, g_efb_h);
    CHECK(g_efb_w == 640 && g_efb_h == 448);

    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_gx_hooks: ok\n");
    return 0;
}
