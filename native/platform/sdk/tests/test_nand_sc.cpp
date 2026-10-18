// Unit tests of NAND (the game's save sequence, sync and async) and SC.
#include <revolution/nand.h>
#include <revolution/os.h>
#include <revolution/sc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static volatile s32 cb_result = 12345;
static volatile int cb_done = 0;
static void on_done(s32 r, NANDCommandBlock*) { cb_result = r; cb_done = 1; }
static void wait_cb() {
    int spins = 0;
    while (!cb_done) { OSYieldThread(); if (++spins > 3000000) { printf("timeout\n"); g_fail++; return; } }
    cb_done = 0;
}

int main() {
    char dir[] = "/tmp/np_nand_test_XXXXXX";
    CHECK(mkdtemp(dir) != 0);
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    cfg.data_dir = dir;
    cfg.language = 3;
    cfg.widescreen = 0;
    NpSdk_Init(&cfg);

    CHECK(SCGetLanguage() == SC_LANG_FR);
    CHECK(SCGetAspectRatio() == SC_ASPECT_STD);
    CHECK(SCCheckStatus() == SC_STATUS_OK);
    CHECK((SCGetSimpleAddressID() >> 24) == 49);
    u8 v = 99;
    CHECK(SCFindU8Item(&v, SC_ITEM_IPL_LNG) && v == 3);

    CHECK(NANDInit() == NAND_RESULT_OK);
    char home[64], cur[64];
    CHECK(NANDGetHomeDir(home) == NAND_RESULT_OK);
    printf("  home %s\n", home);
    CHECK(strcmp(home, "/title/00010000/52345145/data") == 0);
    CHECK(NANDGetCurrentDir(cur) == NAND_RESULT_OK && strcmp(cur, "/") == 0);
    CHECK(NANDChangeDir(home) == NAND_RESULT_OK);
    CHECK(NANDGetCurrentDir(cur) == NAND_RESULT_OK && strcmp(cur, home) == 0);

    u32 answer = 77;
    CHECK(NANDCheck(8, 2, &answer) == NAND_RESULT_OK && answer == 0);

    CHECK(NANDCreate("save.bin", NAND_PERM_RWALL, 0) == NAND_RESULT_OK);
    CHECK(NANDCreate("save.bin", NAND_PERM_RWALL, 0) == NAND_RESULT_EXISTS);
    NANDFileInfo fi;
    CHECK(NANDOpen("save.bin", &fi, NAND_ACCESS_RW) == NAND_RESULT_OK);
    char data[4096];
    for (int i = 0; i < 4096; ++i) data[i] = (char)(i * 3);
    CHECK(NANDWrite(&fi, data, 4096) == 4096);
    u32 len = 0;
    CHECK(NANDGetLength(&fi, &len) == NAND_RESULT_OK && len == 4096);
    CHECK(NANDSeek(&fi, 0, NAND_SEEK_BEG) == 0);
    char back[4096];
    memset(back, 0, sizeof back);
    CHECK(NANDRead(&fi, back, 4096) == 4096);
    CHECK(memcmp(data, back, 4096) == 0);
    CHECK(NANDClose(&fi) == NAND_RESULT_OK);

    // the game's asynchronous sequence: open, get length, read, close
    NANDCommandBlock cb;
    memset(&cb, 0, sizeof cb);
    CHECK(NANDOpenAsync("save.bin", &fi, NAND_ACCESS_READ, on_done, &cb) == NAND_RESULT_OK);
    wait_cb();
    CHECK(cb_result == NAND_RESULT_OK);
    len = 0;
    CHECK(NANDGetLengthAsync(&fi, &len, on_done, &cb) == NAND_RESULT_OK);
    wait_cb();
    CHECK(cb_result == NAND_RESULT_OK && len == 4096);
    memset(back, 0, sizeof back);
    CHECK(NANDReadAsync(&fi, back, 4096, on_done, &cb) == NAND_RESULT_OK);
    wait_cb();
    CHECK(cb_result == 4096 && memcmp(data, back, 4096) == 0);
    CHECK(NANDCloseAsync(&fi, on_done, &cb) == NAND_RESULT_OK);
    wait_cb();

    CHECK(NANDOpen("missing.bin", &fi, NAND_ACCESS_READ) == NAND_RESULT_NOEXISTS);
    CHECK(NANDCreateDir("sub", NAND_PERM_RWALL, 0) == NAND_RESULT_OK);
    CHECK(NANDChangeDir("sub") == NAND_RESULT_OK);
    CHECK(NANDGetCurrentDir(cur) == NAND_RESULT_OK && strcmp(cur, "/title/00010000/52345145/data/sub") == 0);
    CHECK(NANDChangeDir("..") == NAND_RESULT_OK);
    CHECK(NANDDelete("sub") == NAND_RESULT_OK);
    CHECK(NANDDelete("save.bin") == NAND_RESULT_OK);
    CHECK(NANDDelete("save.bin") == NAND_RESULT_NOEXISTS);
    CHECK(NANDChangeDir("/tmp") == NAND_RESULT_OK);

    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("test_nand_sc: ok\n");
    return 0;
}
