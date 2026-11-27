// MSC_DVD_KEEP_KIB (1.3, np/dvd_keep.h): the keep cache of dvd_native.cpp. The parent re-runs itself with different environments; every child checks that
//   - a hinted file is read whole by the background thread and then answered from memory (the file is deleted from the folder to prove it) with the same bytes
//     as the card would give, at any offset and length, with the zero fill past the end;
//   - a file that is not keepable (extension, size) is never kept and still reads from the folder;
//   - a keepable file that was not hinted is kept after its first read from the card (a miss), and answered from memory the second time;
//   - the budget holds, files of an earlier match (NpDvdKeepBegin) make room for the next one's, and files of this one are never dropped;
//   - with the cache off nothing is kept and every read goes to the folder.
#include <revolution/dvd.h>
#include <revolution/os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "np/dvd_keep.h"
#include "np/np_sdk.h"

extern "C" volatile uint32_t np_stat_dvd_keep_released, np_stat_dvd_reads, np_stat_dvd_keep_hits, np_stat_dvd_keep_fills, np_stat_dvd_keep_misses;

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static unsigned char gen(size_t i, unsigned seed) { return (unsigned char)((i * 31 + (i >> 8) * 7 + seed) & 0xFF); }

static char g_root[64];
static void write_file(const char* name, size_t n, unsigned seed) {
    char p[512];
    snprintf(p, sizeof p, "%s/files/%s", g_root, name);
    FILE* f = fopen(p, "wb");
    for (size_t i = 0; i < n; ++i) fputc(gen(i, seed), f);
    fclose(f);
}
static void remove_file(const char* name) {
    char p[512];
    snprintf(p, sizeof p, "%s/files/%s", g_root, name);
    unlink(p);
}

static void wait_idle(DVDFileInfo* fi) {
    for (long spins = 0; DVDGetCommandBlockStatus(&fi->block) != DVD_STATE_IDLE; ++spins) {
        if (DVDGetCommandBlockStatus(&fi->block) == DVD_STATE_FATAL) return;
        OSYieldThread();
        if (spins > 50000000) { printf("timeout\n"); g_fail++; return; }
    }
}

// Reads [off, off+len) of the named file through the DVD layer and compares with the generator; false if the read failed.
static bool read_check(const char* name, size_t size, unsigned seed, size_t off, size_t len) {
    static unsigned char buf[600000] __attribute__((aligned(32)));
    const s32 e = DVDConvertPathToEntrynum(name);
    if (e <= 0) return false;
    DVDFileInfo fi;
    if (!DVDFastOpen(e, &fi)) return false;
    memset(buf, 0xEE, len + 32);
    DVDReadAsyncPrio(&fi, buf, (u32)len, (u32)off, 0, 2);
    wait_idle(&fi);
    const bool failed = DVDGetCommandBlockStatus(&fi.block) == DVD_STATE_FATAL;
    DVDClose(&fi);
    if (failed) return false;
    for (size_t i = 0; i < len; ++i) {
        const size_t at = off + i;
        if (buf[i] != (at < size ? gen(at, seed) : 0)) { printf("FAIL %s: byte %zu is %02x\n", name, at, buf[i]); g_fail++; return true; }
    }
    CHECK(buf[len] == 0xEE);
    return true;
}

// Reads [off, off+len) in one go (like read_check) but returns the number of card reads it cost.
static unsigned card_reads() { return np_stat_dvd_reads; }

static unsigned files_held() {
    unsigned f = 0;
    NpDvdKeepStats(&f, 0, 0, 0);
    return f;
}
static bool wait_files(unsigned n) {
    for (int i = 0; i < 400; ++i) {
        if (files_held() >= n) return true;
        usleep(10000);
    }
    return false;
}
static size_t up32(size_t n) { return (n + 31) & ~(size_t)31; }

static int child(const char* mode) {
    snprintf(g_root, sizeof g_root, "/tmp/np_dvd_keep_XXXXXX");
    if (!mkdtemp(g_root)) return 2;
    char p[512];
    snprintf(p, sizeof p, "%s/files", g_root);
    mkdir(p, 0777);
    write_file("a.nis", 70000, 1);
    write_file("b.sanim", 30000, 2);
    write_file("big.nis", 400000, 3);   // over the 256 KiB file limit
    write_file("c.bin", 5000, 4);       // not a keepable extension
    write_file("d.fen", 13856, 5);
    write_file("e.nis", 20000, 6);      // never hinted
    write_file("f.nis", 60000, 7);
    {
        char d[512];
        snprintf(d, sizeof d, "%s/files/anim", g_root);
        mkdir(d, 0777);
        snprintf(d, sizeof d, "%s/files/anim/sub", g_root);
        mkdir(d, 0777);
    }
    write_file("anim/x.sanim", 9000, 8);
    write_file("anim/y.sanim", 12000, 9);
    write_file("anim/note.txt", 700, 10);       // a directory hint takes any type
    write_file("anim/sub/deep.sanim", 500, 11); // but not what is in a sub-directory
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    cfg.disc_dir = g_root;
    NpSdk_Init(&cfg);
    DVDInit();
    const bool off = !strcmp(mode, "off");
    if (off) {
        CHECK(NpDvdKeepHint("a.nis") == 0);
        CHECK(NpDvdKeepStats(0, 0, 0, 0) == 0);
        CHECK(read_check("a.nis", 70000, 1, 0, up32(70000)));
        CHECK(np_stat_dvd_keep_hits == 0 && np_stat_dvd_keep_fills == 0 && np_stat_dvd_keep_misses == 0);
        CHECK(read_check("e.nis", 20000, 6, 0, up32(20000)));
        CHECK(files_held() == 0);
    } else if (!strcmp(mode, "floor")) {
        // heap pressure (pretend figures): under the add floor (12 MiB) nothing is added; once files are held and the free heap falls under the release floor (6 MiB) they are given back,
        // and reads that run while that happens still return the right bytes (a copy never sees a freed file)
        setenv("NP_TEST_HEAP_FREE_MIB", "5", 1);
        CHECK(NpDvdKeepHint("a.nis") == 1);
        usleep(300000);
        CHECK(files_held() == 0);                      // refused: 5 MiB free is under the add floor
        setenv("NP_TEST_HEAP_FREE_MIB", "40", 1);
        CHECK(NpDvdKeepHint("a.nis") == 1 && NpDvdKeepHint("b.sanim") == 1 && NpDvdKeepHint("d.fen") == 1 && NpDvdKeepHint("f.nis") == 1);
        CHECK(wait_files(4));
        usleep(200000);
        CHECK(files_held() == 4);
        // reads while the release happens
        const unsigned drops0 = NpDvdKeepDrops();
        setenv("NP_TEST_HEAP_FREE_MIB", "3", 1);
        struct { const char* n; size_t sz; unsigned seed; } all[] = {{"a.nis", 70000, 1}, {"b.sanim", 30000, 2}, {"d.fen", 13856, 5}, {"f.nis", 60000, 7}};
        unsigned long reads = 0;
        for (int round = 0; round < 3000 && files_held() > 0; ++round) {
            auto& k = all[round % 4];
            CHECK(read_check(k.n, k.sz, k.seed, 0, up32(k.sz)));
            ++reads;
            usleep(500);
        }
        CHECK(files_held() == 0);                      // all given back
        CHECK(NpDvdKeepDrops() >= drops0 + 4);          // the line-up's hand-over is told it is no longer whole (NisPlayer::PrefetchMatchFiles asks again)
        CHECK(reads > 4);
        CHECK(np_stat_dvd_keep_released >= 150);       // KiB (about 175 KiB held)
        CHECK(read_check("a.nis", 70000, 1, 0, up32(70000)));   // still readable from the folder
        // a failed allocation anywhere: the new handler frees what is held (nothing here) and passes on
        setenv("NP_TEST_HEAP_FREE_MIB", "40", 1);
        CHECK(NpDvdKeepHint("d.fen") == 1);
        CHECK(wait_files(1));
        usleep(200000);
        CHECK(files_held() == 1);
        setenv("NP_TEST_HEAP_FREE_MIB", "3", 1);
        for (int i = 0; i < 40 && files_held() > 0; ++i) usleep(100000);
        CHECK(files_held() == 0);
    } else if (!strcmp(mode, "small")) {
        // 90 KiB: a (70000) fits, b (30000) does not; nothing of the earlier epoch to drop for it
        CHECK(NpDvdKeepStats(0, 0, 0, 0) == 1);
        CHECK(NpDvdKeepHint("a.nis") == 1 && NpDvdKeepHint("b.sanim") == 1);
        CHECK(wait_files(1));
        usleep(300000);
        CHECK(files_held() == 1);
        remove_file("a.nis");
        CHECK(read_check("a.nis", 70000, 1, 0, up32(70000)));
        CHECK(np_stat_dvd_keep_hits >= 1);
        // a new match: the old file may go to make room, the new one is kept
        NpDvdKeepBegin();
        CHECK(NpDvdKeepHint("b.sanim") == 1);
        usleep(300000);
        CHECK(files_held() == 1 && np_stat_dvd_keep_fills == 2);
        remove_file("b.sanim");
        CHECK(read_check("b.sanim", 30000, 2, 0, up32(30000)));
        // but a file of the same match is never dropped for another: d.fen (13856) fits beside b (30000)
        CHECK(NpDvdKeepHint("d.fen") == 1);
        usleep(300000);
        CHECK(files_held() == 2);
        CHECK(NpDvdKeepHint("f.nis") == 1);   // 60000 more: over the budget, and both held belong to this epoch
        usleep(300000);
        CHECK(files_held() == 2);
    } else {
        CHECK(NpDvdKeepStats(0, 0, 0, 0) == 1);
        CHECK(NpDvdKeepHint("a.nis") == 1);
        CHECK(NpDvdKeepHint("b.sanim") == 1);
        CHECK(NpDvdKeepHint("big.nis") == 0);
        CHECK(NpDvdKeepHint("c.bin") == 0);
        CHECK(NpDvdKeepHint("nothere.nis") == 0);
        CHECK(NpDvdKeepHint("d.fen") == 1);
        CHECK(NpDvdKeepHint("a.nis") == 1);   // asked twice: kept once
        CHECK(wait_files(3));
        usleep(200000);
        CHECK(files_held() == 3 && np_stat_dvd_keep_fills == 3);
        const uint32_t reads0 = np_stat_dvd_reads;
        remove_file("a.nis");
        remove_file("b.sanim");
        remove_file("d.fen");
        // from memory with the files gone: whole, in windows at 32-byte offsets, and past the end (zero filled)
        struct { const char* n; size_t sz; unsigned seed; } kept[] = {{"a.nis", 70000, 1}, {"b.sanim", 30000, 2}, {"d.fen", 13856, 5}};
        for (auto& k : kept) {
            const size_t rounded = up32(k.sz);
            CHECK(read_check(k.n, k.sz, k.seed, 0, rounded));
            CHECK(read_check(k.n, k.sz, k.seed, 32, 64));
            CHECK(read_check(k.n, k.sz, k.seed, rounded - 32, 32));
            CHECK(read_check(k.n, k.sz, k.seed, 0, 32));
        }
        CHECK(np_stat_dvd_reads == reads0);          // no card read at all
        CHECK(np_stat_dvd_keep_hits >= 12);
        // not keepable: read from the folder, never kept
        CHECK(read_check("c.bin", 5000, 4, 0, up32(5000)));
        CHECK(read_check("big.nis", 400000, 3, 0, 262144));
        CHECK(np_stat_dvd_reads > reads0);
        usleep(100000);
        CHECK(files_held() == 3);
        // a keepable file nobody hinted: a miss now, kept behind it, answered from memory after
        const uint32_t misses0 = np_stat_dvd_keep_misses;
        CHECK(read_check("e.nis", 20000, 6, 0, up32(20000)));
        CHECK(np_stat_dvd_keep_misses == misses0 + 1);
        CHECK(wait_files(4));
        usleep(100000);
        remove_file("e.nis");
        const uint32_t hits0 = np_stat_dvd_keep_hits;
        CHECK(read_check("e.nis", 20000, 6, 0, up32(20000)));
        CHECK(np_stat_dvd_keep_hits == hits0 + 1);
        // read-through from the read's own bytes: a file read front to back in two pieces costs exactly those two card reads and is then answered from memory;
        // the first piece alone starts nothing, a read that does not continue it throws the half-made copy away
        {
            const unsigned r0 = card_reads();
            const unsigned f0 = files_held();
            CHECK(read_check("f.nis", 60000, 7, 0, 30016));
            CHECK(card_reads() == r0 + 1 && files_held() == f0);   // half a file: nothing kept yet
            // (read_check always reads from `off`, so the continuation is the same call with an offset)
            CHECK(read_check("f.nis", 60000, 7, 30016, up32(60000) - 30016));
            CHECK(card_reads() == r0 + 2);
            CHECK(files_held() == f0 + 1);                          // kept from those two reads: no third read of the file
            remove_file("f.nis");
            CHECK(read_check("f.nis", 60000, 7, 0, up32(60000)));
            CHECK(card_reads() == r0 + 2);
        }
        // a directory: every file directly inside (3), not the sub-directory's, of any type; nothing for a file path or a missing one
        CHECK(NpDvdKeepHintDir("anim") == 3);
        CHECK(NpDvdKeepHintDir("a.nis") == 0 && NpDvdKeepHintDir("nowhere") == 0);
        CHECK(wait_files(8));
        usleep(100000);
        CHECK(files_held() == 8);
        remove_file("anim/x.sanim");
        remove_file("anim/y.sanim");
        remove_file("anim/note.txt");
        CHECK(read_check("anim/x.sanim", 9000, 8, 0, up32(9000)));
        CHECK(read_check("anim/y.sanim", 12000, 9, 0, up32(12000)));
        CHECK(read_check("anim/note.txt", 700, 10, 0, up32(700)));
    }
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_root);
    if (system(cmd) != 0) {}
    printf("  child %s: %s\n", mode, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && !strcmp(argv[1], "child")) return child(argv[2]);
    struct { const char* env; const char* mode; } runs[] = {
        {"MSC_DVD_KEEP_KIB=1024 MSC_DVD_DIRECT=1", "full"},
        {"MSC_DVD_KEEP_KIB=1024 MSC_DVD_DIRECT=0", "full"},
        {"MSC_DVD_KEEP_KIB=90 MSC_DVD_DIRECT=1", "small"},
        {"MSC_DVD_KEEP_KIB=1024 MSC_DVD_DIRECT=1", "floor"},
        {"MSC_DVD_KEEP_KIB=0", "off"},
    };
    int bad = 0;
    for (auto& r : runs) {
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "env %s '%s' child %s", r.env, argv[0], r.mode);
        printf("%s (%s)\n", r.env, r.mode);
        fflush(stdout);
        if (system(cmd) != 0) { printf("  FAILED: %s\n", r.env); ++bad; }
    }
    if (bad) { printf("test_dvd_keep: %d run(s) FAILED\n", bad); return 1; }
    printf("test_dvd_keep: ok\n");
    return 0;
}
