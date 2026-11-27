// The game's own file layer (nlFileGC.cpp, built into this test) on the SDK's DVD layer over a temporary disc tree: the admission of
// asynchronous reads. A read of an unaligned size is two queue entries (an aligned head and a 32 byte tail) that must be admitted as a
// whole or not at all: a head whose tail was refused never completes, and the caller was told the read was queued (STR-156).
// Checked: a read that fits completes as before (callback once, the full size, the bytes of the file, the position advanced); a split
// read (bufferSize 0 = exactly the bytes asked for) made while only one entry is free (from a completion callback, where waiting for the queue is not possible) is refused with
// nothing queued, no pending count, the position untouched, and the same request goes through once the queue has room.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <revolution/dvd.h>
#include <revolution/os.h>

#include "NL/nlFile.h"
#include "NL/nlMemory.h"
#include "np/np_sdk.h"

// ---- what the file layer needs from the rest of the game ------------------------------------------------------------------------
void* nlMalloc(unsigned long size, unsigned int alignment, bool) {
    void* p = 0;
    if (posix_memalign(&p, alignment < 32 ? 32 : alignment, size ? size : 1) != 0) return 0;
    return p;
}
void* nlMalloc(unsigned long size) { return nlMalloc(size, 32, false); }
extern "C" int NpDiscReportReadError(void) { return 0; }
nlFile::nlFile() {}
nlFile::~nlFile() {}

#include "../../../../src/NL/nlFileGC.cpp"

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static const unsigned kFileSize = 16384;
static unsigned char Byte(unsigned i) { return (unsigned char)((i * 13 + 5) & 0xFF); }

static int g_hits;
static unsigned g_lastSize;
static unsigned long g_lastParam;
static void OnRead(nlFile*, void*, unsigned int size, unsigned long param) {
    ++g_hits;
    g_lastSize = size;
    g_lastParam = param;
}

// A completion callback that makes one more request, as the movie player's does: inside AsyncManager::Service
static nlFile* g_file;
static unsigned char* g_nestedBuf;
static unsigned g_nestedSize;
static AsyncEntry* g_nestedResult;
static unsigned long g_posBefore, g_posAfter, g_pendingBefore, g_pendingAfter;
static bool g_nestedActiveBefore, g_nestedActiveAfter;
static void OnReadNested(nlFile* f, void*, unsigned int, unsigned long) {
    DolphinFile* d = (DolphinFile*)f;
    g_posBefore = d->m_Position;
    g_pendingBefore = d->PendingAsync;
    g_nestedActiveBefore = s_pAsyncManager->m_activeEntryList != 0;
    g_nestedResult = nlReadAsync(f, g_nestedBuf, g_nestedSize, OnRead, 77, 0);
    g_posAfter = d->m_Position;
    g_pendingAfter = d->PendingAsync;
    g_nestedActiveAfter = s_pAsyncManager->m_activeEntryList != 0;
}

static unsigned CountFree() {
    unsigned n = 0;
    AsyncEntry* e = s_pAsyncManager->m_freeEntryList;
    if (!e) return 0;
    AsyncEntry* start = nlDLRingGetStart(e);
    AsyncEntry* c = start;
    do {
        ++n;
        c = c->m_next;
    } while (c != start && n < 100);
    return n;
}

static void Drain() {
    for (int spins = 0; nlAsyncReadsPending(0) && spins < 5000000; ++spins) {
        OSYieldThread();
        nlServiceFileSystem();
    }
    CHECK(!nlAsyncReadsPending(0));
}

int main() {
    char root[] = "/tmp/np_nl_async_XXXXXX";
    if (!mkdtemp(root)) return 1;
    char p[512];
    snprintf(p, sizeof p, "%s/files", root);
    mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/files/data.bin", root);
    FILE* f = fopen(p, "wb");
    for (unsigned i = 0; i < kFileSize; ++i) fputc(Byte(i), f);
    fclose(f);

    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    cfg.disc_dir = root;
    NpSdk_Init(&cfg);
    nlInitFileSystem();
    CHECK(s_pAsyncManager != 0);
    CHECK(CountFree() == 64);

    nlFile* file = nlOpen("data.bin");
    CHECK(file != 0);
    if (!file) return 1;
    DolphinFile* d = (DolphinFile*)file;
    g_file = file;
    static unsigned char buf[8192] __attribute__((aligned(32)));
    static unsigned char buf2[8192] __attribute__((aligned(32)));

    // (1) a read that fits: unaligned size = head + tail, one callback with the full size, the file's bytes, the position advanced
    memset(buf, 0xEE, sizeof buf);
    g_hits = 0;
    nlSeek(file, 0, 0);
    AsyncEntry* e = nlReadAsync(file, buf, 1000, OnRead, 5, 0);
    CHECK(e != 0);
    CHECK(d->m_Position == 1000);
    CHECK(d->PendingAsync == 2);
    CHECK(CountFree() == 62);
    Drain();
    CHECK(g_hits == 1 && g_lastSize == 1000 && g_lastParam == 5);
    CHECK(d->PendingAsync == 0);
    CHECK(CountFree() == 64);
    bool same = true;
    for (unsigned i = 0; i < 1000; ++i) same = same && buf[i] == Byte(i);
    CHECK(same);
    CHECK(buf[1000] == 0xEE);   // nothing past the request is written

    // (2) the same read from inside a completion callback with the queue full but one entry (the callback's own, just returned):
    // 63 aligned reads of 32 bytes are queued behind the one whose callback makes the request
    memset(buf2, 0xEE, sizeof buf2);
    g_hits = 0;
    g_nestedBuf = buf2;
    g_nestedSize = 1000;   // head + tail = 2 entries
    g_nestedResult = (AsyncEntry*)1;
    nlSeek(file, 0, 0);
    static unsigned char filler[64][32] __attribute__((aligned(32)));
    AsyncEntry* first = nlReadAsync(file, filler[0], 32, OnReadNested, 1, 0x10000);
    CHECK(first != 0);
    for (int i = 1; i < 64; ++i) CHECK(nlReadAsync(file, filler[i], 32, 0, 0, 0x10000) != 0);
    CHECK(CountFree() == 0);
    CHECK(d->PendingAsync == 64);
    const unsigned long posBeforeAll = d->m_Position;   // 64 * 32
    CHECK(posBeforeAll == 64 * 32);
    for (int spins = 0; g_nestedResult == (AsyncEntry*)1 && spins < 5000000; ++spins) {
        OSYieldThread();
        nlServiceFileSystem();
    }
    CHECK(g_nestedResult == 0);                             // refused: the caller learns that nothing was queued
    CHECK(g_posAfter == g_posBefore);                       // ... the position is where it was
    CHECK(g_pendingAfter == g_pendingBefore);               // ... no entry counts against the file
    CHECK(g_hits == 0);                                     // ... and no callback is owed
    Drain();
    CHECK(g_hits == 0);
    CHECK(d->PendingAsync == 0);
    CHECK(CountFree() == 64);

    // (3) a single-entry request in the same situation is admitted with the one free entry
    g_hits = 0;
    g_nestedSize = 1024;   // aligned: one entry
    g_nestedResult = (AsyncEntry*)1;
    nlSeek(file, 0, 0);
    CHECK(nlReadAsync(file, filler[0], 32, OnReadNested, 1, 0x10000) != 0);
    for (int i = 1; i < 64; ++i) CHECK(nlReadAsync(file, filler[i], 32, 0, 0, 0x10000) != 0);
    for (int spins = 0; g_nestedResult == (AsyncEntry*)1 && spins < 5000000; ++spins) {
        OSYieldThread();
        nlServiceFileSystem();
    }
    CHECK(g_nestedResult != 0 && g_nestedResult != (AsyncEntry*)1);
    CHECK(g_posAfter == g_posBefore + 1024);
    Drain();
    CHECK(g_hits == 1 && g_lastSize == 1024 && g_lastParam == 77);
    CHECK(d->PendingAsync == 0);
    CHECK(CountFree() == 64);

    // (4) from outside a callback the same split request waits for room (Service) and is admitted
    g_hits = 0;
    nlSeek(file, 0, 0);
    for (int i = 0; i < 64; ++i) CHECK(nlReadAsync(file, filler[i], 32, 0, 0, 0x10000) != 0);
    CHECK(CountFree() == 0);
    memset(buf, 0xEE, sizeof buf);
    e = nlReadAsync(file, buf, 1000, OnRead, 9, 0);
    CHECK(e != 0);
    Drain();
    CHECK(g_hits == 1 && g_lastSize == 1000 && g_lastParam == 9);
    same = true;
    for (unsigned i = 0; i < 1000; ++i) same = same && buf[i] == Byte(64 * 32 + i);
    CHECK(same);
    CHECK(d->PendingAsync == 0);

    snprintf(p, sizeof p, "rm -rf %s", root);
    if (system(p) != 0) {}
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_nl_async: ok\n");
    return 0;
}
