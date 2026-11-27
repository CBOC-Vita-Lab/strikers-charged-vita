// 1.3 MSC_DVD_RETRY (np/dvd_retry.h, dvd_native.cpp do_read): a read whose handle died (standby) is tried again on a fresh handle, up to n times.
// Part 1: the policy with fakes. Part 2: the real DVD layer (direct reads, MSC_DVD_DIRECT=1) over a temporary tree, with one handle that stays dead for
// five raw calls in a row (MSC_DVD_FAULT=3 MSC_DVD_FAULT_RUN=5): every read must still deliver the right bytes. Deliberate breaks: the single retry of
// the first design (MSC_DVD_RETRY=1) and none (0) must fail: `MSC_DVD_RETRY=1 test_dvd_retry --expect-fail`. test_dvd_direct covers the other switches.
#include <revolution/dvd.h>
#include <revolution/os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <initializer_list>

#include "np/dvd_retry.h"
#include "np/np_sdk.h"

extern "C" volatile uint32_t np_stat_dvd_retries;
// wave 14: the platform's wake-up counter (np_vita_wake_epoch, weak in dvd_native.cpp). --resume pretends the console woke up before the run.
static volatile unsigned g_wakeEpoch = 0;
extern "C" unsigned np_vita_wake_epoch(void) { return g_wakeEpoch; }

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static void test_policy() {
    // success at once: one call, no reset, no pause
    {
        int calls = 0, resets = 0, pauses = 0, used = 0, lastSeen = 0;
        long r = np::RetryRead(20, [&](bool last) { ++calls; lastSeen += last; return 64L; }, [&] { ++resets; }, [&] { ++pauses; }, &used);
        CHECK(r == 64 && calls == 1 && resets == 0 && pauses == 0 && used == 1 && lastSeen == 0);
    }
    // fails twice, then reads: three calls, two resets and pauses, only the 3rd may not be "last"
    {
        int calls = 0, resets = 0, pauses = 0, used = 0, lastSeen = 0;
        long r = np::RetryRead(20, [&](bool last) { lastSeen += last; return ++calls < 3 ? -1L : 32L; }, [&] { ++resets; }, [&] { ++pauses; }, &used);
        CHECK(r == 32 && calls == 3 && resets == 2 && pauses == 2 && used == 3 && lastSeen == 0);
    }
    // never reads: retries + 1 calls, the final one marked last (the only one that reports), no reset after it
    {
        int calls = 0, resets = 0, pauses = 0, used = 0, lastSeen = 0, lastAt = -1;
        long r = np::RetryRead(5, [&](bool last) { if (last) { ++lastSeen; lastAt = calls; } ++calls; return -1L; }, [&] { ++resets; }, [&] { ++pauses; }, &used);
        CHECK(r == -1 && calls == 6 && resets == 5 && pauses == 5 && used == 6 && lastSeen == 1 && lastAt == 5);
    }
    // off (0) and negative: one attempt, marked last
    for (int n : {0, -3}) {
        int calls = 0, lastSeen = 0;
        long r = np::RetryRead(n, [&](bool last) { ++calls; lastSeen += last; return -1L; }, [] {}, [] {});
        CHECK(r == -1 && calls == 1 && lastSeen == 1);
    }
    CHECK(np::DvdRetryCount(nullptr) == 60 && np::DvdRetryCount("") == 60 && np::DvdRetryCount("0") == 0 && np::DvdRetryCount("off") == 0);
    CHECK(np::DvdRetryCount("OFF") == 0 && np::DvdRetryCount("on") == 60 && np::DvdRetryCount("1") == 1);
    CHECK(np::DvdRetryCount("7") == 7 && np::DvdRetryCount("999") == 200 && np::DvdRetryCount("x") == 60);
}

static void write_file(const char* path, size_t n, unsigned seed) {
    FILE* f = fopen(path, "wb");
    for (size_t i = 0; i < n; ++i) fputc((int)((i * 7 + seed) & 0xFF), f);
    fclose(f);
}

// returns the number of reads that delivered wrong bytes or an error
static int test_layer() {
    char root[] = "/tmp/np_dvd_retry_XXXXXX";
    if (!mkdtemp(root)) return 1;
    char p[512];
    snprintf(p, sizeof p, "%s/files", root); mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/files/a.bin", root); write_file(p, 70000, 3);
    snprintf(p, sizeof p, "%s/files/b.bin", root); write_file(p, 4096, 5);

    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    cfg.disc_dir = root;
    NpSdk_Init(&cfg);
    DVDInit();
    const s32 ea = DVDConvertPathToEntrynum("a.bin"), eb = DVDConvertPathToEntrynum("b.bin");
    CHECK(ea > 0 && eb > 0);
    static unsigned char buf[32768] __attribute__((aligned(32)));
    int bad = 0;
    for (int i = 0; i < 24; ++i) {
        DVDFileInfo fi;
        const bool useA = (i & 1) == 0;
        if (!DVDFastOpen(useA ? ea : eb, &fi)) { ++bad; continue; }
        const u32 off = useA ? (u32)(i * 2048) : 0;
        const u32 len = useA ? 8192 : 4096;
        const unsigned seed = useA ? 3 : 5;
        memset(buf, 0xEE, sizeof buf);
        const s32 got = DVDReadPrio(&fi, buf, (s32)len, (s32)off, 2);
        int wrong = got != (s32)len;
        for (u32 k = 0; k < len && !wrong; ++k)
            if (buf[k] != (unsigned char)(((off + k) * 7 + seed) & 0xFF)) wrong = 1;
        DVDClose(&fi);
        if (wrong) {
            ++bad;
            printf("  read %d: got %d of %u, %s\n", i, (int)got, (unsigned)len, got == (s32)len ? "wrong bytes" : "failed");
            break;   // a failed read leaves the layer in its fatal state
        }
    }
    return bad;
}

int main(int argc, char** argv) {
    const bool expectFail = argc > 1 && strcmp(argv[1], "--expect-fail") == 0;
    // wave 14: `--resume`: the console has just woken up and the card stays dead for 80 raw calls in a row (more than the 60 + 1 attempts of the plain policy):
    // the read must still be delivered (the 20 s window adds MSC_DVD_RETRY_RESUME attempts). `--resume --expect-fail` with MSC_DVD_RETRY_RESUME=0 must fail.
    const bool resume = argc > 1 && strcmp(argv[1], "--resume") == 0;
    const bool resumeFail = resume && argc > 2 && strcmp(argv[2], "--expect-fail") == 0;
    if (resume) g_wakeEpoch = 1;
    printf("policy\n");
    test_policy();
    setenv("MSC_DVD_DIRECT", "1", 1);
    setenv("MSC_DVD_FAULT", "3", 1);
    setenv("MSC_DVD_FAULT_RUN", resume ? "80" : "5", 1);
    printf("layer, a handle dead for 5 raw calls (MSC_DVD_RETRY=%s)\n", getenv("MSC_DVD_RETRY") ? getenv("MSC_DVD_RETRY") : "default");
    const int bad = test_layer();
    printf("  retries %u\n", (unsigned)np_stat_dvd_retries);
    if (expectFail || resumeFail) CHECK(bad > 0);
    else if (resume) CHECK(bad == 0 && np_stat_dvd_retries == 80);   // eighty failed attempts: only the resume window lets the 81st read
    else CHECK(bad == 0 && np_stat_dvd_retries == 5);   // five failed attempts, the sixth reads
    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("test_dvd_retry: ok%s\n", expectFail || resumeFail ? " (the break was seen)" : "");
    return 0;
}
