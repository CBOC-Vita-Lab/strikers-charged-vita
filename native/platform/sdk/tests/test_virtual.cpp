// Unit test of virtual time and the virtual interrupt controller (src/os_virtual.cpp).
//
// The scenario (retrace callbacks, a periodic and a one-shot alarm, three disc reads of different priority, a NAND
// write chain, audio DMA blocks, a main thread that waits for retraces and sleeps) runs twice in child processes with
// NP_VIRTUAL_TIME=1; the logs must be identical, the virtual time covered must match what the main thread asked for, and the
// real time spent must be a small fraction of it (the idle scheduler warps the clock).
#include <revolution/ai.h>
#include <revolution/dvd.h>
#include <revolution/nand.h>
#include <revolution/os.h>
#include <revolution/vi.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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

static s64 t0;
static double ms_since_start() { return (double)(OSGetTime() - t0) * 1000.0 / (double)OS_TIME_SPEED; }
static double real_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void log_line(const char* what, long a = 0, long b = 0) {
    // the callbacks run in interrupt context on the running thread: printf is fine
    printf("%10.3f %s %ld %ld\n", ms_since_start(), what, a, b);
}

// ---- events -------------------------------------------------------------------------------------
static volatile int g_retraces = 0;
static void on_retrace(u32 count) { g_retraces++; if (count % 10 == 0) log_line("retrace", count); }

static OSAlarm g_periodic, g_once;
static OSAlarm g_odd_periodic, g_odd_once;
static int g_odd_hits, g_odd_once_hits;
static void on_odd_periodic(OSAlarm*, OSContext*) { g_odd_hits++; }
static void on_odd_once(OSAlarm*, OSContext*) { g_odd_once_hits++; }
static volatile int g_periodic_hits = 0;
static void on_periodic(OSAlarm*, OSContext*) { g_periodic_hits++; if (g_periodic_hits % 5 == 1) log_line("periodic", g_periodic_hits); }
static void on_once(OSAlarm*, OSContext*) { log_line("once"); }

static DVDFileInfo g_fi[3];
static unsigned char g_buf[3][70016] __attribute__((aligned(32)));
static volatile int g_dvd_done = 0;
static void on_dvd(s32 r, DVDFileInfo* fi) { g_dvd_done++; log_line("dvd", (long)(fi - g_fi), r); }

static NANDCommandBlock g_nb;
static NANDFileInfo g_nf;
static volatile int g_nand_step = 0;
static char g_wbuf[4096] __attribute__((aligned(32)));
static void on_nand(s32 r, NANDCommandBlock*) {
    log_line("nand", g_nand_step, r);
    switch (g_nand_step++) {
    case 0: NANDOpenAsync("vt.dat", &g_nf, NAND_ACCESS_WRITE, on_nand, &g_nb); break;
    case 1: NANDWriteAsync(&g_nf, g_wbuf, sizeof g_wbuf, on_nand, &g_nb); break;
    case 2: NANDCloseAsync(&g_nf, on_nand, &g_nb); break;
    default: break;
    }
}

static volatile int g_ai_blocks = 0;
static short g_ai_buf[96 * 2] __attribute__((aligned(32)));
static void on_ai() { g_ai_blocks++; if (g_ai_blocks % 100 == 1) log_line("ai", g_ai_blocks); }

static void write_file(const char* path, size_t n, unsigned seed) {
    FILE* f = fopen(path, "wb");
    for (size_t i = 0; i < n; ++i) fputc((int)((i * 7 + seed) & 0xFF), f);
    fclose(f);
}

// ---- the child: one run of the scenario -----------------------------------------------------------
static int child(const char* root) {
    setenv("NP_VIRTUAL_TIME", "1", 1);
    alarm(60);   // a hang of the idle scheduler ends the test (SIGALRM) instead of the runner
    char p[600];
    snprintf(p, sizeof p, "%s/data", root);
    mkdir(p, 0777);
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    cfg.disc_dir = root;
    char data[600];
    snprintf(data, sizeof data, "%s/data", root);
    cfg.data_dir = data;
    NpSdk_Init(&cfg);
    OSInit();
    t0 = OSGetTime();
    const double r0 = real_seconds();

    VIInit();
    VISetPostRetraceCallback(on_retrace);
    OSCreateAlarm(&g_periodic);
    OSCreateAlarm(&g_once);
    OSSetPeriodicAlarm(&g_periodic, OSGetTime() + OSMillisecondsToTicks(5), OSMillisecondsToTicks(10), on_periodic);
    OSSetAlarm(&g_once, OSMillisecondsToTicks(25), on_once);

    DVDInit();
    const char* names[3] = {"a.bin", "b.bin", "c.bin"};
    for (int i = 0; i < 3; ++i) {
        s32 e = DVDConvertPathToEntrynum(names[i]);
        if (e < 0 || !DVDFastOpen(e, &g_fi[i])) { printf("no file %s\n", names[i]); return 2; }
    }
    // three requests at once: the high priority one (prio 0) is served first, then the others in order
    DVDReadAsyncPrio(&g_fi[0], g_buf[0], 32768, 0, on_dvd, 2);
    DVDReadAsyncPrio(&g_fi[1], g_buf[1], 65536, 0, on_dvd, 2);
    DVDReadAsyncPrio(&g_fi[2], g_buf[2], 4096, 0, on_dvd, 0);

    NANDInit();
    memset(g_wbuf, 0x5A, sizeof g_wbuf);
    NANDChangeDir("/");
    NANDCreateAsync("vt.dat", 0x34, 0, on_nand, &g_nb);   // steps 0..3

    AIInit(0);
    AIRegisterDMACallback(on_ai);
    AIInitDMA(g_ai_buf, sizeof g_ai_buf);
    AIStartDMA();

    // the main thread: 30 retraces, then a sleep of 50 ms, then the DVD / NAND chain must be done
    for (int i = 0; i < 30; ++i) VIWaitForRetrace();
    log_line("30 retraces", g_retraces);
    OSSleepTicks(OSMillisecondsToTicks(50));
    log_line("slept");
    // a loop of yields on a status, as the game's disc code does
    OSAlarm a;
    OSCreateAlarm(&a);
    OSSetAlarm(&a, OSMillisecondsToTicks(20), on_once);
    int spins = 0;
    while (a.handler != 0 && ++spins < 10000000) OSYieldThread();
    log_line("yield loop", spins > 0 && spins < 10000000);
    // deadlines that are not whole nanoseconds (the game's 405000-tick = 6.67 ms periodic alarm): the idle scheduler must not
    // warp to an instant at which the alarm is still one tick away
    OSCreateAlarm(&g_odd_periodic);
    OSCreateAlarm(&g_odd_once);
    OSSetPeriodicAlarm(&g_odd_periodic, 405000, 405000, on_odd_periodic);   // as AudioBackend's speaker service: start == period
    OSSetAlarm(&g_odd_once, 12345677, on_odd_once);   // 203.2 ms
    OSSleepTicks(OSMillisecondsToTicks(250));
    log_line("odd alarms", g_odd_once_hits, g_odd_hits >= 30);

    const double virt_ms = ms_since_start();
    const double real_ms = (real_seconds() - r0) * 1000.0;
    printf("virtual %.3f ms retraces %d periodic %d dvd %d nand_step %d ai %d\n", virt_ms, g_retraces, g_periodic_hits,
           g_dvd_done, g_nand_step, g_ai_blocks);
    fprintf(stderr, "(real %.1f ms)\n", real_ms);
    fflush(stdout);
    if (!(g_dvd_done == 3 && g_nand_step == 4 && g_retraces >= 30 && g_ai_blocks > 100)) return 3;
    if (!(g_odd_once_hits == 1 && g_odd_hits >= 30)) return 5;
    // the data of the three reads is right
    if (g_buf[0][5] != (unsigned char)((5 * 7 + 1) & 0xFF) || g_buf[2][5] != (unsigned char)((5 * 7 + 3) & 0xFF)) return 4;
    return 0;
}

static int run_child(const char* self, const char* root, char* out, size_t cap) {
    char cmd[1400];
    const char* runner = getenv("NP_TEST_RUNNER");   // e.g. qemu-arm for an armhf build
    snprintf(cmd, sizeof cmd, "%s %s child %s 2>/dev/null", runner ? runner : "", self, root);
    FILE* f = popen(cmd, "r");
    if (!f) return -1;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = 0;
    return pclose(f);
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "child")) return child(argv[2]);
    char root[] = "/tmp/np_vt_test_XXXXXX";
    CHECK(mkdtemp(root) != 0);
    char p[600];
    snprintf(p, sizeof p, "%s/files", root); mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/files/a.bin", root); write_file(p, 40000, 1);
    snprintf(p, sizeof p, "%s/files/b.bin", root); write_file(p, 70000, 2);
    snprintf(p, sizeof p, "%s/files/c.bin", root); write_file(p, 8192, 3);

    static char log1[64 * 1024], log2[64 * 1024];
    double t = real_seconds();
    int r1 = run_child(argv[0], root, log1, sizeof log1);
    double t1 = real_seconds() - t;
    // the second run starts from the same files but a fresh NAND
    snprintf(p, sizeof p, "rm -rf %s/data", root);
    system(p);
    int r2 = run_child(argv[0], root, log2, sizeof log2);
    printf("%s", log1);
    printf("child 1 rc=%d (%.2f s real), child 2 rc=%d\n", r1, t1, r2);
    CHECK(r1 == 0 && r2 == 0);
    CHECK(strlen(log1) > 100);
    CHECK(strcmp(log1, log2) == 0);   // the same events at the same virtual times
    if (strcmp(log1, log2)) printf("--- second run ---\n%s", log2);
    // 30 retraces + 50 ms + 20 ms is about 570 ms of virtual time: it must not have taken that long for real
    CHECK(t1 < 3.0);
    snprintf(p, sizeof p, "rm -rf %s", root);
    system(p);
    printf(g_fail ? "FAILED (%d)\n" : "test_virtual: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
