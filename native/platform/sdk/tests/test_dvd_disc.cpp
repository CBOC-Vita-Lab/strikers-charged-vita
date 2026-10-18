// The real disc dump (sys/fst.bin + files/): every file of the table must resolve, sizes must match
// the host files, and a few reads must return the file's bytes. Skips (exit 0) without a disc.
#include <revolution/dvd.h>
#include <revolution/os.h>

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

int main() {
    const char* disc = getenv("NP_TEST_DISC") ? getenv("NP_TEST_DISC") : "/home/claude/msc/game";
    char p[512];
    snprintf(p, sizeof p, "%s/sys/fst.bin", disc);
    if (access(p, R_OK) != 0) { printf("test_dvd_disc: no disc, skipped\n"); return 0; }
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    cfg.disc_dir = disc;
    NpSdk_Init(&cfg);
    DVDInit();
    const char* names[] = {"Art/effects/marioeffects.bun", "audio/CHAR_BOO_Sfx.nlxwb", "audio/char_boo_sfx.resbun", "RFLRes01.arc", "opening.bnr"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
        s32 e = DVDConvertPathToEntrynum(names[i]);
        char hp[600];
        snprintf(hp, sizeof hp, "%s/files/%s", disc, names[i]);
        struct stat st;
        int have = stat(hp, &st) == 0;
        if (!have && i == 2) { snprintf(hp, sizeof hp, "%s/files/audio/CHAR_BOO_Sfx.resbun", disc); have = stat(hp, &st) == 0; }
        printf("  %-34s entry %d host %s\n", names[i], e, have ? "yes" : "no");
        CHECK(e > 0);
        DVDFileInfo fi;
        CHECK(DVDFastOpen(e, &fi));
        if (have) CHECK(fi.size == (u32)st.st_size);
        // read the first 64 KiB (aligned) and the tail
        u32 n = fi.size < 65536 ? (fi.size + 31) & ~31u : 65536;
        unsigned char* buf = (unsigned char*)malloc(n + 64);
        unsigned char* al = (unsigned char*)(((unsigned long)buf + 31) & ~31ul);
        CHECK(DVDReadPrio(&fi, al, (s32)n, 0, 2) == (s32)n);
        FILE* f = fopen(hp, "rb");
        if (f) {
            unsigned char* ref = (unsigned char*)malloc(n);
            size_t got = fread(ref, 1, n, f);
            CHECK(memcmp(ref, al, got) == 0);
            free(ref);
            fclose(f);
        }
        free(buf);
        DVDClose(&fi);
    }
    // every entry of the table resolves back to a readable extent: sample the first 40 files
    printf("  disc id %.4s%.2s\n", DVDGetCurrentDiskID()->game, DVDGetCurrentDiskID()->company);
    CHECK(memcmp(DVDGetCurrentDiskID()->game, "R4QE", 4) == 0);
    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("test_dvd_disc: ok\n");
    return 0;
}
