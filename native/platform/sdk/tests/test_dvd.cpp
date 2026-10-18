// Unit tests of the DVD layer: a scanned table over a temporary tree, then the real disc dump
// (sys/fst.bin) when NP_TEST_DISC (default /home/claude/msc/game) exists.
#include <revolution/dvd.h>
#include <revolution/os.h>

#include <dirent.h>
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

static void write_file(const char* path, size_t n, unsigned seed) {
    FILE* f = fopen(path, "wb");
    for (size_t i = 0; i < n; ++i) fputc((int)((i * 7 + seed) & 0xFF), f);
    fclose(f);
}

static volatile int cb_hits = 0;
static volatile s32 cb_result = 0;
static void on_read(s32 result, DVDFileInfo* info) { (void)info; cb_result = result; cb_hits++; }

static void wait_idle(DVDFileInfo* fi) {
    int spins = 0;
    while (DVDGetCommandBlockStatus(&fi->block) != DVD_STATE_IDLE) {
        OSYieldThread();
        if (++spins > 5000000) { printf("timeout\n"); g_fail++; return; }
    }
}

static void test_scanned() {
    char root[] = "/tmp/np_dvd_test_XXXXXX";
    CHECK(mkdtemp(root) != 0);
    char p[512];
    snprintf(p, sizeof p, "%s/files", root); mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/files/Art", root); mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/files/Art/Nis", root); mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/files/readme.txt", root); write_file(p, 100, 1);
    snprintf(p, sizeof p, "%s/files/Art/A.bin", root); write_file(p, 4096, 2);
    snprintf(p, sizeof p, "%s/files/Art/Nis/b.nis", root); write_file(p, 70000, 3);

    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    cfg.disc_dir = root;
    NpSdk_Init(&cfg);
    DVDInit();

    s32 e1 = DVDConvertPathToEntrynum("readme.txt");
    s32 e2 = DVDConvertPathToEntrynum("/art/a.BIN");   // case-insensitive, absolute
    s32 e3 = DVDConvertPathToEntrynum("Art/Nis/b.nis");
    s32 e4 = DVDConvertPathToEntrynum("nothing.here");
    printf("  entries %d %d %d %d\n", e1, e2, e3, e4);
    CHECK(e1 > 0 && e2 > 0 && e3 > 0 && e4 == -1);

    DVDFileInfo fi;
    CHECK(DVDFastOpen(e3, &fi));
    CHECK(fi.size == 70000);
    static unsigned char buf[70016] __attribute__((aligned(32)));
    memset(buf, 0xEE, sizeof buf);
    CHECK(DVDReadAsyncPrio(&fi, buf, 32768, 0, on_read, 2));
    CHECK(DVDGetCommandBlockStatus(&fi.block) != DVD_STATE_IDLE || cb_hits == 1);
    wait_idle(&fi);
    CHECK(cb_hits == 1 && cb_result == 32768);
    CHECK(buf[0] == 3 && buf[1] == 10 && buf[32767] == (unsigned char)((32767 * 7 + 3) & 0xFF));
    CHECK(buf[32768] == 0xEE);
    // a read at an offset, rounded up past the end of the file (the game reads in 32 byte units)
    CHECK(DVDReadAsyncPrio(&fi, buf, 70016 - 69984, 69984, 0, 2));
    wait_idle(&fi);
    CHECK(buf[0] == (unsigned char)((69984 * 7 + 3) & 0xFF));
    CHECK(buf[69999 - 69984] == (unsigned char)((69999 * 7 + 3) & 0xFF));
    CHECK(buf[70000 - 69984] == 0);   // zero filled beyond the end
    // synchronous read
    CHECK(DVDFastOpen(e2, &fi));
    CHECK(DVDReadPrio(&fi, buf, 4096, 0, 2) == 4096);
    CHECK(buf[100] == (unsigned char)((100 * 7 + 2) & 0xFF));
    DVDClose(&fi);
    // two queued reads: priority 0 is served before priority 3
    DVDFileInfo a, b;
    DVDFastOpen(e3, &a);
    DVDFastOpen(e2, &b);
    static unsigned char buf2[4096] __attribute__((aligned(32)));
    DVDReadAsyncPrio(&a, buf, 65536, 0, 0, 3);
    DVDReadAsyncPrio(&b, buf2, 4096, 0, 0, 0);
    wait_idle(&a);
    wait_idle(&b);
    CHECK(DVDGetDriveStatus() == DVD_STATE_IDLE);
    const DVDDiskID* id = DVDGetCurrentDiskID();
    CHECK(memcmp(id->game, "R4QE", 4) == 0);
}

int main() {
    printf("scanned table\n");
    test_scanned();
    const char* disc = getenv("NP_TEST_DISC") ? getenv("NP_TEST_DISC") : "/home/claude/msc/game";
    char p[512];
    snprintf(p, sizeof p, "%s/sys/fst.bin", disc);
    if (access(p, R_OK) == 0) {
        printf("real disc (%s): see test_dvd_disc\n", disc);
    }
    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("test_dvd: ok\n");
    return 0;
}
