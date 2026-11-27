// The 1.3 loading switches of dvd_native.cpp (MSC_DVD_DIRECT, _CHUNK_KIB, _RETRY, _PREOPEN, _FAULT). The parent re-runs itself with different environments and
// every child must read the same bytes as the generator says; the counters show that the paths under test were taken:
//   stdio (the old path), direct with 4 KiB chunks, direct with a fault injected into every 3rd raw read (retry on), the same with the retry off (must fail),
//   direct with the pre-open helper.
#include <revolution/dvd.h>
#include <revolution/os.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "np/np_sdk.h"

extern "C" volatile uint32_t np_stat_dvd_retries, np_stat_dvd_preopen_hits, np_stat_dvd_raw_calls;

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static unsigned char gen(size_t i, unsigned seed) { return (unsigned char)((i * 31 + (i >> 8) * 7 + seed) & 0xFF); }

static void write_file(const char* path, size_t n, unsigned seed) {
    FILE* f = fopen(path, "wb");
    for (size_t i = 0; i < n; ++i) fputc(gen(i, seed), f);
    fclose(f);
}

static void wait_idle(DVDFileInfo* fi) {
    for (long spins = 0; DVDGetCommandBlockStatus(&fi->block) != DVD_STATE_IDLE; ++spins) {
        if (DVDGetCommandBlockStatus(&fi->block) == DVD_STATE_FATAL) return;
        OSYieldThread();
        if (spins > 50000000) { printf("timeout\n"); g_fail++; return; }
    }
}

static int child(int expectFail) {
    char root[] = "/tmp/np_dvd_direct_XXXXXX";
    if (!mkdtemp(root)) return 2;
    char p[512];
    snprintf(p, sizeof p, "%s/files", root); mkdir(p, 0777);
    struct { const char* name; size_t size; unsigned seed; } files[] = {
        {"a.bin", 100, 1}, {"b.bin", 70000, 2}, {"c.bin", 1500000, 3}, {"d.bin", 4096, 4}, {"e.bin", 33000, 5}, {"f.bin", 12288, 6},
    };
    struct { const char* name; size_t size; unsigned seed; } fresh[] = {   // read only by the burst below (not in the worker's handle cache yet)
        {"g0.bin", 5000, 11}, {"g1.bin", 9000, 12}, {"g2.bin", 3000, 13}, {"g3.bin", 7000, 14}, {"g4.bin", 20000, 15}, {"g5.bin", 6000, 16},
    };
    for (auto& f : fresh) { snprintf(p, sizeof p, "%s/files/%s", root, f.name); write_file(p, f.size, f.seed); }
    for (auto& f : files) { snprintf(p, sizeof p, "%s/files/%s", root, f.name); write_file(p, f.size, f.seed); }
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    cfg.disc_dir = root;
    NpSdk_Init(&cfg);
    DVDInit();
    int failedReads = 0;
    static unsigned char buf[1500064] __attribute__((aligned(32)));
    // every file, whole, then in windows at 32-byte offsets, then past the end (the tail is zero filled)
    for (int round = 0; round < 3; ++round) {
        for (auto& f : files) {
            const s32 e = DVDConvertPathToEntrynum(f.name);
            CHECK(e > 0);
            DVDFileInfo fi;
            CHECK(DVDFastOpen(e, &fi));
            const size_t rounded = (f.size + 31) & ~(size_t)31;
            struct { size_t off, len; } windows[] = {{0, rounded}, {32, rounded > 96 ? 64 : rounded - 32 > 0 ? rounded - 32 : 32}, {rounded >= 4096 ? rounded - 4096 : 0, rounded >= 4096 ? 4096 : rounded}};
            for (auto& w : windows) {
                if (w.off >= rounded) continue;
                if (w.off + w.len > rounded) w.len = rounded - w.off;
                memset(buf, 0xEE, w.len + 32);
                DVDReadAsyncPrio(&fi, buf, (u32)w.len, (u32)w.off, 0, 2);
                wait_idle(&fi);
                if (DVDGetCommandBlockStatus(&fi.block) == DVD_STATE_FATAL) { ++failedReads; break; }
                for (size_t i = 0; i < w.len; ++i) {
                    const size_t at = w.off + i;
                    const unsigned char want = at < f.size ? gen(at, f.seed) : 0;
                    if (buf[i] != want) { printf("FAIL %s: byte %zu is %02x, wanted %02x\n", f.name, at, buf[i], want); g_fail++; break; }
                }
                CHECK(buf[w.len] == 0xEE);   // nothing written past the request
            }
            DVDClose(&fi);
            if (failedReads) break;
        }
        if (failedReads) break;
    }
    // a burst: a long read of the big file first, then the first window of six files not opened yet, all queued at once, so that the worker has requests in line
    // behind the one it reads (the pre-open helper's input)
    if (!failedReads) {
        DVDFileInfo lead, fis[6];
        static unsigned char bigbuf[1500064] __attribute__((aligned(32)));
        static unsigned char bufs[6][4096] __attribute__((aligned(32)));
        DVDFastOpen(DVDConvertPathToEntrynum("c.bin"), &lead);
        DVDReadAsyncPrio(&lead, bigbuf, 1500000, 0, 0, 2);
        for (int k = 0; k < 6; ++k) {
            DVDFastOpen(DVDConvertPathToEntrynum(fresh[k].name), &fis[k]);
            memset(bufs[k], 0xEE, sizeof bufs[k]);
            DVDReadAsyncPrio(&fis[k], bufs[k], (u32)((fresh[k].size + 31) & ~(size_t)31) > 4096 ? 4096 : (u32)((fresh[k].size + 31) & ~(size_t)31), 0, 0, 2);
        }
        wait_idle(&lead);
        for (int k = 0; k < 6; ++k) {
            wait_idle(&fis[k]);
            for (size_t i = 0; i < (fresh[k].size < 4096 ? fresh[k].size : 4096); ++i)
                if (bufs[k][i] != (i < fresh[k].size ? gen(i, fresh[k].seed) : 0)) { printf("FAIL burst %s byte %zu\n", fresh[k].name, i); g_fail++; break; }
            DVDClose(&fis[k]);
        }
        DVDClose(&lead);
    }
    printf("  child: raw calls %u, retries %u, pre-open hits %u, failed reads %d\n", (unsigned)np_stat_dvd_raw_calls, (unsigned)np_stat_dvd_retries, (unsigned)np_stat_dvd_preopen_hits, failedReads);
    if (expectFail) { CHECK(failedReads > 0); }
    else {
        CHECK(failedReads == 0);
        const char* d = getenv("MSC_DVD_DIRECT");
        if (d && *d == '1') CHECK(np_stat_dvd_raw_calls > 0);
        else CHECK(np_stat_dvd_raw_calls == 0);
        if (getenv("MSC_DVD_PREOPEN") && *getenv("MSC_DVD_PREOPEN") == '1') CHECK(np_stat_dvd_preopen_hits > 0);
        if (getenv("MSC_DVD_FAULT") && *getenv("MSC_DVD_FAULT") != '0') CHECK(np_stat_dvd_retries > 0);
        else CHECK(np_stat_dvd_retries == 0);
    }
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd) != 0) {}
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && !strcmp(argv[1], "child")) return child(atoi(argv[2]));
    struct { const char* env; int expectFail; } runs[] = {
        {"MSC_DVD_DIRECT=0", 0},
        {"MSC_DVD_DIRECT=1 MSC_DVD_CHUNK_KIB=4", 0},
        {"MSC_DVD_DIRECT=1 MSC_DVD_CHUNK_KIB=0", 0},
        {"MSC_DVD_DIRECT=1 MSC_DVD_CHUNK_KIB=4 MSC_DVD_FAULT=3", 0},
        {"MSC_DVD_DIRECT=0 MSC_DVD_RETRY=0", 0},
        {"MSC_DVD_DIRECT=1 MSC_DVD_CHUNK_KIB=4 MSC_DVD_FAULT=3 MSC_DVD_RETRY=0", 1},   // without the retry the injected error is the old fatal error
        {"MSC_DVD_DIRECT=1 MSC_DVD_CHUNK_KIB=64 MSC_DVD_PREOPEN=1", 0},
    };
    int bad = 0;
    for (auto& r : runs) {
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "env %s '%s' child %d", r.env, argv[0], r.expectFail);
        printf("%s\n", r.env);
        fflush(stdout);
        if (system(cmd) != 0) { printf("  FAILED: %s\n", r.env); ++bad; }
    }
    if (bad) { printf("test_dvd_direct: %d run(s) FAILED\n", bad); return 1; }
    printf("test_dvd_direct: ok\n");
    return 0;
}
