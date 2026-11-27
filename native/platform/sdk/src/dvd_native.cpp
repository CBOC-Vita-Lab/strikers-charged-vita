// DVD: the disc is the extracted game folder.
//
//   <disc_dir>/sys/fst.bin   the retail file system table (entry numbers, disc offsets, sizes);
//                            when absent a table is built from a scan of <disc_dir>/files
//   <disc_dir>/sys/boot.bin  the disc ID (first 32 bytes); defaults to "R4QE01"
//   <disc_dir>/files/...     the file contents
//
// Path lookup, DVDOpen/FastOpen/Close and the read wrappers are the SDK's own dvdfs.c (dvd_fs.c, fed
// with the table loaded here). What replaces the SDK's driver (dvd.c, dvd_broadway.c, the IOS DI
// ioctls) is a command queue served by one worker thread: DVDReadAbsAsyncPrio queues a read by
// absolute disc address, the worker maps the address back to a host file and reads it, then the
// command block goes to DVD_STATE_IDLE and the callback runs in interrupt context, as the SDK's
// DI interrupt handler did. Behaviour follows the recomp's HLE (hle/storage/dvd.cpp).
#include <revolution/dvd.h>
#include <revolution/os.h>

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <new>
#include <sys/stat.h>
#if defined(__vita__)
#include <malloc.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
extern unsigned int _newlib_heap_size_user __attribute__((weak));   // the C heap's size (native/platform/vita/src/np_vita_process.cpp)
#include "../../vita/include/np_boot_io.h"   // the stdio lock's start-up retry
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "np_internal.h"
#include "np/disc_error.h"
#include "np/dvd_retry.h"

extern "C" void np_dvdfs_set_fst(const void* nodes, char* strings, u32 count);

// 1.2 (MSC_MEGA_CUTLOG): disc reads made so far (the renderer's [mega-cut] lines print the difference per frame). Counting only.
extern "C" {
volatile uint32_t np_stat_dvd_reads = 0;
volatile uint32_t np_stat_dvd_kib = 0;
// 1.3 loading switches: reads retried after an error, reads/handles opened ahead and used (tests and the log read these)
volatile uint32_t np_stat_dvd_retries = 0;
volatile uint32_t np_stat_dvd_preopen_hits = 0;
volatile uint32_t np_stat_dvd_raw_calls = 0;
// 1.3 MSC_DVD_KEEP_KIB: reads answered from the keep cache, files filled into it (by hint or after a miss), bytes it holds, misses of keepable files
volatile uint32_t np_stat_dvd_keep_hits = 0;
volatile uint32_t np_stat_dvd_keep_fills = 0;
volatile uint32_t np_stat_dvd_keep_released = 0;   // KiB given back to the heap
volatile uint32_t np_stat_dvd_keep_fail[5] = {0, 0, 0, 0, 0};   // why a fill did not keep the file: heap floor, malloc, short read or open, over the budget, table full
volatile uint32_t np_stat_dvd_keep_bytes = 0;
volatile uint32_t np_stat_dvd_keep_misses = 0;
}

volatile u32 __DVDLayoutFormat = 0;   // Wii: table offsets and DI addresses are in units of 4 bytes
DVDCommandBlock __DVDStopMotorCommandBlock;

namespace {

struct Extent {
    uint32_t start;   // disc address in words (4 bytes)
    uint32_t end;
    int entry;
};

uint32_t* g_nodes = 0;      // host-endian words, 3 per entry
char* g_names = 0;
uint32_t g_count = 0;
char** g_hostpath = 0;      // per entry: path of the host file (files only)
Extent* g_extents = 0;
int g_nextents = 0;
DVDDiskID g_diskid;
int g_inited = 0;
int g_fatal = 0;
char g_read_error[1200];
char g_disc[512];

// command queue (four priorities, FIFO inside each), guarded by the interrupt lock
DVDCommandBlock* g_qhead[4];
DVDCommandBlock* g_qtail[4];
DVDCommandBlock* g_executing = 0;
NpSem g_io_sem;
OSThreadQueue g_wait_queue;

// file handle cache of the worker thread. A slot holds a stdio stream (f) or, with MSC_DVD_DIRECT, a raw handle (fd >= 0).
struct OpenFile { int entry; FILE* f; int fd; uint32_t stamp; };
OpenFile g_open[16] = {{0, 0, -1, 0}, {0, 0, -1, 0}, {0, 0, -1, 0}, {0, 0, -1, 0}, {0, 0, -1, 0}, {0, 0, -1, 0}, {0, 0, -1, 0}, {0, 0, -1, 0},
                       {0, 0, -1, 0}, {0, 0, -1, 0}, {0, 0, -1, 0}, {0, 0, -1, 0}, {0, 0, -1, 0}, {0, 0, -1, 0}, {0, 0, -1, 0}, {0, 0, -1, 0}};
uint32_t g_stamp = 0;

// ---- 1.3 loading switches (read once, all documented in docs/TECHNICAL.md) -----------------------------------------------------
//   MSC_DVD_DIRECT=0|1       default 1 on the Vita, 0 elsewhere. Disc files are read with one positional call per chunk (sceIoPread / pread) straight into the
//                            game's buffer. Newlib's fread has no direct path (the libc objects of the SDK only copy out of the stream buffer and call __srefill_r),
//                            so every read went through the 1 KiB (BUFSIZ) buffer, one system call per KiB, and every fseek threw the buffer away.
//   MSC_DVD_CHUNK_KIB=<n>    default 512; the most one call asks for (0 = the whole request).
//   MSC_DVD_RETRY=<n>|off    default 60 (about three seconds; 20 until 11 Nov). A read or open that fails is tried again on a freshly opened handle (the handles of a card can be invalid after the
//                            console slept), where it used to end in the fatal disc error: the first retry at once, the next ones 50 ms apart (about one second
//                            in all; np/dvd_retry.h). 0 or off restores the old behaviour, 1 is a single retry.
//   MSC_DVD_PREOPEN=0|1      default 0 (console A/B). A helper thread opens the files of the next queued requests while the worker reads the current one.
//   MSC_DVD_FAULT=<n>        tests only: every n-th raw read returns an error once (proves the retry), 0 = off.
//   MSC_DVD_FAULT_RUN=<k>    tests only: such a fault repeats for k raw calls in a row (default 1), as a handle that stays dead for a while would.
//   MSC_DVD_KEEP_KIB=<n>     default 0 here (the Vita plan sets 4096). 1.3 "no loading in play": a RAM cache of at most n KiB for the small files a match reads after kick-off
//                            (goal and Mega Strike cut-scene data, character animations, the Mega Strike defence HUD): np/dvd_keep.h. The game hints the files of the match's
//                            characters while the walk-out plays (NpDvdKeepHint), a background thread reads them whole, and the worker then answers from memory; a keepable file
//                            read from the card in play is also kept (once) for the next goal. 0 = off, every read goes to the card as in 1.2.
//   MSC_DVD_KEEP_FILE_KIB=<n> default 256: the largest file kept. MSC_DVD_KEEP_HEAP_MIB=<n> default 12 (Vita): nothing is added while the C heap has less free.
struct DvdSwitches {
    bool direct, preopen;
    uint32_t keepBudget, keepFileMax;
    int keepHeapMib, keepReleaseMib;
    int retries;
    uint32_t chunk;
    uint32_t faultEvery, faultRun;
};
DvdSwitches g_sw;
bool g_sw_done = false;

bool env_flag(const char* name, bool dflt) {
    const char* e = getenv(name);
    if (!e || !*e) return dflt;
    return !(e[0] == '0' || !strcasecmp(e, "off"));
}

void switches_init() {
    if (g_sw_done) return;
    g_sw_done = true;
#if defined(__vita__)
    const bool dflt = true;
#else
    const bool dflt = false;
#endif
    g_sw.direct = env_flag("MSC_DVD_DIRECT", dflt);
    g_sw.retries = np::DvdRetryCount(getenv("MSC_DVD_RETRY"));
    g_sw.preopen = g_sw.direct && env_flag("MSC_DVD_PREOPEN", false);
    {
        const char* k = getenv("MSC_DVD_KEEP_KIB");
        unsigned long kib = (k && *k) ? strtoul(k, 0, 10) : 0ul;
        g_sw.keepBudget = (uint32_t)(kib > 65536ul ? 65536ul : kib) * 1024u;
        k = getenv("MSC_DVD_KEEP_FILE_KIB");
        kib = (k && *k) ? strtoul(k, 0, 10) : 256ul;
        g_sw.keepFileMax = (uint32_t)(kib > 8192ul ? 8192ul : kib) * 1024u;
        k = getenv("MSC_DVD_KEEP_HEAP_MIB");
        g_sw.keepHeapMib = (k && *k) ? atoi(k) : 12;
        k = getenv("MSC_DVD_KEEP_RELEASE_MIB");
        g_sw.keepReleaseMib = (k && *k) ? atoi(k) : 6;
    }
    const char* e = getenv("MSC_DVD_CHUNK_KIB");
    unsigned long kib = (e && *e) ? strtoul(e, 0, 10) : 512ul;
    g_sw.chunk = kib == 0 ? 0u : (uint32_t)(kib > 65536ul ? 65536ul : kib) * 1024u;
    e = getenv("MSC_DVD_FAULT");
    g_sw.faultEvery = (e && *e) ? (uint32_t)strtoul(e, 0, 10) : 0u;
    e = getenv("MSC_DVD_FAULT_RUN");
    g_sw.faultRun = (e && *e) ? (uint32_t)strtoul(e, 0, 10) : 1u;
    if (g_sw.faultRun < 1) g_sw.faultRun = 1;
}

// The raw layer: the Vita's own calls, POSIX elsewhere (the host tests run the same code). A handle is >= 0, an error is negative.
int raw_open(const char* path) {
#if defined(__vita__)
    return sceIoOpen(path, SCE_O_RDONLY, 0);
#else
    return open(path, O_RDONLY);
#endif
}
long raw_pread(int fd, void* dst, size_t n, uint64_t off) {
#if defined(__vita__)
    return sceIoPread(fd, dst, (SceSize)n, (SceOff)off);
#else
    return (long)pread(fd, dst, n, (off_t)off);
#endif
}
void raw_close(int fd) {
#if defined(__vita__)
    sceIoClose(fd);
#else
    close(fd);
#endif
}

// Reads up to `size` bytes at `off` in chunks; returns the bytes read (short only at the end of the file) or -1 on an error. Counts calls.
volatile uint32_t g_raw_reopens = 0;
long raw_read_all(int fd, void* dst, uint32_t size, uint64_t off) {
    uint8_t* p = (uint8_t*)dst;
    uint32_t done = 0;
    while (done < size) {
        uint32_t want = size - done;
        if (g_sw.chunk && want > g_sw.chunk) want = g_sw.chunk;
        const uint32_t call = __atomic_add_fetch(&np_stat_dvd_raw_calls, 1u, __ATOMIC_RELAXED);
        // (tests) one injected error (MSC_DVD_FAULT_RUN: k in a row), then none for the next 1000 calls: the retry of the same request must be able to finish
        static uint32_t lastFault = 0, runLeft = 0;
        if (runLeft) {
            --runLeft;
            lastFault = call;
            return -1;
        }
        if (g_sw.faultEvery && call % g_sw.faultEvery == 0 && (lastFault == 0 || call - lastFault > 1000)) {
            lastFault = call;
            runLeft = g_sw.faultRun - 1;
            return -1;
        }
        const long r = raw_pread(fd, p + done, want, off + done);
        if (r < 0) return -1;
        if (r == 0) break;   // end of the file
        done += (uint32_t)r;
    }
    return (long)done;
}

uint32_t swap32(uint32_t v) { return __builtin_bswap32(v); }

uint32_t rd32be(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

void read_error(const char* format, ...) __attribute__((format(printf, 1, 2)));
void read_error(const char* format, ...) {
    const BOOL interrupts = OSDisableInterrupts();
    if (!g_read_error[0]) {
        va_list args;
        va_start(args, format);
        vsnprintf(g_read_error, sizeof g_read_error, format, args);
        va_end(args);
    }
    OSRestoreInterrupts(interrupts);
}

// ---- building the table -----------------------------------------------------------------------

// One try at the table. On failure *err is the errno (ENOENT: the file is not there; anything else: it is, but did not open or read).
// (11 Nov) Every failure leaves the globals untouched and frees what it allocated; before, a failed malloc was dereferenced and a short read of a good file
// (the fopen race on the console: a stream with descriptor -1 reads nothing) ended in "no extracted game files".
bool load_fst_once(const char* path, int* err) {
#if defined(__vita__)
    FILE* f = np_boot_fopen_once(path, "rb");   // (MSC_BOOTIO_INJECT: the first n opens fail, tests)
#else
    FILE* f = fopen(path, "rb");
#endif
    if (!f) {
        *err = errno ? errno : EIO;
        return false;
    }
    *err = EIO;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 12 || size > 16 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    uint8_t* raw = (uint8_t*)malloc((size_t)size);
    if (!raw) {
        fclose(f);
        return false;
    }
    size_t got = fread(raw, 1, (size_t)size, f);
    fclose(f);
    if ((long)got != size) {
        free(raw);
        return false;
    }
    uint32_t count = rd32be(raw + 8);   // the root entry's size field is the entry count
    if (count < 2 || (size_t)count * 12 > (size_t)size) {
        free(raw);
        return false;
    }
    uint32_t* nodes = (uint32_t*)malloc((size_t)count * 12);
    size_t strBytes = (size_t)size - (size_t)count * 12;
    char* names = (char*)malloc(strBytes + 1);
    if (!nodes || !names) {
        free(nodes);
        free(names);
        free(raw);
        return false;
    }
    for (uint32_t i = 0; i < count * 3; ++i) nodes[i] = rd32be(raw + i * 4);
    memcpy(names, raw + (size_t)count * 12, strBytes);
    names[strBytes] = 0;
    g_nodes = nodes;
    g_names = names;
    g_count = count;
    free(raw);
    return true;
}

// The table of the disc, retried for up to 3 s (np_boot_io.h) where the open or the read failed for another reason than a file that is not there.
bool load_fst_file(const char* path) {
#if defined(__vita__)
    NpBootTry t;
    np_boot_try_begin(&t);
    for (;;) {
        int err = 0;
        if (load_fst_once(path, &err)) return true;
        if (err == ENOENT || !np_boot_try_again(&t, "disc table (fst.bin)", path, err)) return false;
    }
#else
    int err = 0;
    return load_fst_once(path, &err);
#endif
}

// Scan <disc>/files into a table: preorder, folders and files sorted by name (case-insensitive).
struct ScanEntry {
    char* name;
    bool dir;
    uint32_t size;
};
int scan_cmp(const void* a, const void* b) {
    return strcasecmp(((const ScanEntry*)a)->name, ((const ScanEntry*)b)->name);
}

struct Builder {
    uint32_t* nodes;
    uint32_t cap;
    uint32_t n;
    char* names;
    uint32_t namecap;
    uint32_t namelen;
    uint64_t cursor;   // disc address in bytes
};

uint32_t add_name(Builder& b, const char* name) {
    size_t len = strlen(name) + 1;
    if (b.namelen + len > b.namecap) {
        b.namecap = (b.namecap + len) * 2;
        b.names = (char*)realloc(b.names, b.namecap);
    }
    uint32_t off = b.namelen;
    memcpy(b.names + off, name, len);
    b.namelen += (uint32_t)len;
    return off;
}

uint32_t add_node(Builder& b) {
    if (b.n == b.cap) {
        b.cap = b.cap ? b.cap * 2 : 4096;
        b.nodes = (uint32_t*)realloc(b.nodes, (size_t)b.cap * 12);
    }
    return b.n++;
}

void scan_dir(Builder& b, const char* path, uint32_t parent) {
    DIR* d = opendir(path);
    if (!d) return;
    ScanEntry* list = 0;
    int n = 0, cap = 0;
    while (struct dirent* de = readdir(d)) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            list = (ScanEntry*)realloc(list, (size_t)cap * sizeof(ScanEntry));
        }
        list[n].name = strdup(de->d_name);
        list[n].dir = S_ISDIR(st.st_mode);
        list[n].size = (uint32_t)st.st_size;
        ++n;
    }
    closedir(d);
    qsort(list, (size_t)n, sizeof(ScanEntry), scan_cmp);
    for (int i = 0; i < n; ++i) {
        uint32_t idx = add_node(b);
        uint32_t nameoff = add_name(b, list[i].name);
        if (list[i].dir) {
            b.nodes[idx * 3 + 0] = 0x01000000u | nameoff;
            b.nodes[idx * 3 + 1] = parent;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", path, list[i].name);
            scan_dir(b, full, idx);
            b.nodes[idx * 3 + 2] = b.n;
        } else {
            b.nodes[idx * 3 + 0] = nameoff;
            b.nodes[idx * 3 + 1] = (uint32_t)(b.cursor >> 2);
            b.nodes[idx * 3 + 2] = list[i].size;
            b.cursor += (list[i].size + 0x1Fu) & ~0x1Fu;
        }
        free(list[i].name);
    }
    free(list);
}

bool build_fst_from_scan(const char* filesdir) {
    if (!np_dir_exists(filesdir)) return false;
    Builder b;
    memset(&b, 0, sizeof(b));
    b.cursor = 0x1000000;   // after the system area
    uint32_t root = add_node(b);
    add_name(b, "");
    scan_dir(b, filesdir, root);
    b.nodes[root * 3 + 0] = 0x01000000u;
    b.nodes[root * 3 + 1] = 0;
    b.nodes[root * 3 + 2] = b.n;
    g_nodes = b.nodes;
    g_names = b.names;
    g_count = b.n;
    np_log("dvd", "no sys/fst.bin: built a table of %u entries from %s", g_count, filesdir);
    return true;
}

// The host path of a file whose retail name is `rel`, tolerating different letter case.
void resolve_case(char* path, size_t cap, const char* base, const char* rel) {
    snprintf(path, cap, "%s/%s", base, rel);
    if (np_file_exists(path)) return;
    // walk the components case-insensitively
    char cur[1024];
    snprintf(cur, sizeof(cur), "%s", base);
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", rel);
    char* save = 0;
    for (char* comp = strtok_r(tmp, "/", &save); comp; comp = strtok_r(0, "/", &save)) {
        char next[1024];
        snprintf(next, sizeof(next), "%s/%s", cur, comp);
        struct stat st;
        if (stat(next, &st) != 0) {
            DIR* d = opendir(cur);
            bool found = false;
            if (d) {
                while (struct dirent* de = readdir(d)) {
                    if (!strcasecmp(de->d_name, comp)) {
                        snprintf(next, sizeof(next), "%s/%s", cur, de->d_name);
                        found = true;
                        break;
                    }
                }
                closedir(d);
            }
            if (!found) return;   // leave the exact spelling: the open will fail and be reported
        }
        snprintf(cur, sizeof(cur), "%s", next);
    }
    snprintf(path, cap, "%s", cur);
}

int ext_cmp(const void* a, const void* b) {
    uint32_t x = ((const Extent*)a)->start, y = ((const Extent*)b)->start;
    return x < y ? -1 : x > y;
}

// The host paths are made from the table's names alone, with no file system call per file: on the Vita's memory card a
// stat costs about 4-8 ms, and the two per file this did (2,637 entries) held the start for 22.5 s on the maintainer console
// (17 Oct, the [+N ms] boot lines; Vita3K and the host do not show it). A name whose letter case differs on the host is
// found when the file is first opened (open_cached: resolve_case, once, then the path is kept). MSC_DVD_CHECK=1 brings
// back the start-up check that counts the table's files missing from the folder.
char g_filesdir[1024];

void build_paths(const char* filesdir) {
    snprintf(g_filesdir, sizeof(g_filesdir), "%s", filesdir);
    const char* chk = getenv("MSC_DVD_CHECK");
    const bool check = chk && *chk && *chk != '0';
    g_hostpath = (char**)calloc(g_count, sizeof(char*));
    g_extents = (Extent*)calloc(g_count, sizeof(Extent));
    // directory stack
    struct Frame { uint32_t end; size_t len; };
    Frame stack[64];
    int sp = 0;
    char rel[1024];
    rel[0] = 0;
    size_t rlen = 0;
    stack[0].end = g_count;
    stack[0].len = 0;
    int missing = 0;
    for (uint32_t i = 1; i < g_count; ++i) {
        while (sp > 0 && i >= stack[sp].end) {
            rlen = stack[sp].len;
            rel[rlen] = 0;
            --sp;
        }
        uint32_t w0 = g_nodes[i * 3], w1 = g_nodes[i * 3 + 1], w2 = g_nodes[i * 3 + 2];
        const char* name = g_names + (w0 & 0xFFFFFF);
        if (w0 >> 24) {
            if (sp < 63) {
                ++sp;
                stack[sp].end = w2;
                stack[sp].len = rlen;
                rlen += (size_t)snprintf(rel + rlen, sizeof(rel) - rlen, "%s/", name);
            }
        } else {
            char relfile[1100];
            snprintf(relfile, sizeof(relfile), "%s%s", rel, name);
            char full[1100];
            if (check) {
                resolve_case(full, sizeof(full), filesdir, relfile);
                if (!np_file_exists(full)) ++missing;
            } else {
                snprintf(full, sizeof(full), "%s/%s", filesdir, relfile);
            }
            g_hostpath[i] = strdup(full);
            Extent& e = g_extents[g_nextents++];
            e.start = w1;
            e.end = w1 + (w2 + 3) / 4;
            e.entry = (int)i;
        }
    }
    qsort(g_extents, (size_t)g_nextents, sizeof(Extent), ext_cmp);
    if (missing) np_log("dvd", "%d of the table's files are not in %s", missing, filesdir);
}

// ---- reading -----------------------------------------------------------------------------------

const Extent* find_extent(uint32_t word) {
    int lo = 0, hi = g_nextents - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (word < g_extents[mid].start) hi = mid - 1;
        else if (word >= g_extents[mid].end && g_extents[mid].end > g_extents[mid].start) lo = mid + 1;
        else if (word >= g_extents[mid].start && word < g_extents[mid].end) return &g_extents[mid];
        else {
            // zero-length file at this address: look at the neighbours
            lo = mid + 1;
        }
    }
    return 0;
}

// ---- opening --------------------------------------------------------------------------------------------------------------------
// A case-corrected path replaces the table's (open_cached); the old string is never freed (the pre-open helper may be reading it) and the pointer is read atomically.
const char* host_path(int entry) { return __atomic_load_n(&g_hostpath[entry], __ATOMIC_ACQUIRE); }

// The letter-case search for a path that did not open (build_paths no longer does this for every file). Returns the corrected path (leaked, see above) or 0.
const char* corrected_path(int entry) {
    const size_t base = strlen(g_filesdir);
    const char* path = host_path(entry);
    if (strncmp(path, g_filesdir, base) != 0 || path[base] != '/') return 0;
    char full[1100];
    resolve_case(full, sizeof(full), g_filesdir, path + base + 1);
    if (strcmp(full, path) == 0) return 0;
    return strdup(full);
}

// Makes `alt` the entry's path unless another thread corrected it first (then `alt` is nobody's and is freed; the other thread's string stays). The old string is never freed.
void publish_path(int entry, const char* alt) {
    const char* expect = host_path(entry);
    if (strcmp(expect, alt) == 0 || !__atomic_compare_exchange_n(&g_hostpath[entry], (char**)&expect, (char*)alt, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) free((void*)alt);
}

// MSC_DVD_PREOPEN: handles opened ahead by the helper thread (a small table under a mutex; the worker takes one by entry number).
const int kPreSlots = 8;
struct Pre { int entry; int fd; };
Pre g_pre[kPreSlots];
int g_pre_entries[16];   // requests: entry numbers, a ring
uint32_t g_pre_head = 0, g_pre_tail = 0;
NpMutex g_pre_mutex;
NpSem g_pre_sem;
bool g_pre_started = false;
volatile uint32_t g_pre_made = 0, g_pre_wasted = 0;

int take_preopened(int entry) {
    if (!g_pre_started) return -1;
    int fd = -1;
    np_mutex_lock(&g_pre_mutex);
    for (int i = 0; i < kPreSlots; ++i)
        if (g_pre[i].fd >= 0 && g_pre[i].entry == entry) {
            fd = g_pre[i].fd;
            g_pre[i].fd = -1;
            break;
        }
    np_mutex_unlock(&g_pre_mutex);
    if (fd >= 0) __atomic_fetch_add(&np_stat_dvd_preopen_hits, 1u, __ATOMIC_RELAXED);
    return fd;
}

void* preopen_thread(void*) {
    for (;;) {
        np_sem_wait(&g_pre_sem);
        for (;;) {
            int entry = -1;
            np_mutex_lock(&g_pre_mutex);
            if (g_pre_head != g_pre_tail) {
                entry = g_pre_entries[g_pre_head % 16];
                ++g_pre_head;
                for (int i = 0; i < kPreSlots; ++i)
                    if (g_pre[i].fd >= 0 && g_pre[i].entry == entry) { entry = -1; break; }   // already open ahead
            } else {
                np_mutex_unlock(&g_pre_mutex);
                break;
            }
            np_mutex_unlock(&g_pre_mutex);
            if (entry < 0) continue;
            const int fd = raw_open(host_path(entry));   // (a name with another letter case fails here and is found by the worker, as before)
            if (fd < 0) continue;
            __atomic_fetch_add(&g_pre_made, 1u, __ATOMIC_RELAXED);
            np_mutex_lock(&g_pre_mutex);
            int slot = -1;
            for (int i = 0; i < kPreSlots && slot < 0; ++i)
                if (g_pre[i].fd < 0) slot = i;
            int evict = -1;
            if (slot < 0) {   // all taken: the oldest request's handle was never used (the table is a ring by position)
                static int next = 0;
                slot = next++ % kPreSlots;
                evict = g_pre[slot].fd;
                __atomic_fetch_add(&g_pre_wasted, 1u, __ATOMIC_RELAXED);
            }
            g_pre[slot].entry = entry;
            g_pre[slot].fd = fd;
            np_mutex_unlock(&g_pre_mutex);
            if (evict >= 0) raw_close(evict);
        }
    }
    return 0;
}

void preopen_start() {
    if (g_pre_started || !g_sw.preopen) return;
    for (int i = 0; i < kPreSlots; ++i) g_pre[i].fd = -1;
    np_mutex_init(&g_pre_mutex);
    np_sem_init(&g_pre_sem, 0);
    NpThread* t = np_thread_create("dvd-open", NP_ROLE_IO, preopen_thread, 0, 0, 65536);
    if (t) {
        np_thread_detach(t);
        g_pre_started = true;
    } else {
        g_sw.preopen = false;
    }
}

// Asks the helper to open the file of a request that was just queued (the worker reaches it later). A file the worker holds open already, or the one asked for last, is not asked for.
void preopen_hint(uint32_t word) {
    if (!g_pre_started) return;
    const Extent* ex = find_extent(word);
    if (!ex) return;
    const int entry = ex->entry;
    static volatile int last = -1;
    if (entry == last) return;
    last = entry;
    for (int i = 0; i < 16; ++i)
        if (((volatile OpenFile*)&g_open[i])->entry == entry && (((volatile OpenFile*)&g_open[i])->fd >= 0 || ((volatile OpenFile*)&g_open[i])->f)) return;   // (a racy look: only a hint)
    np_mutex_lock(&g_pre_mutex);
    const bool room = g_pre_tail - g_pre_head < 16;
    if (room) g_pre_entries[g_pre_tail++ % 16] = entry;
    np_mutex_unlock(&g_pre_mutex);
    if (room) np_sem_post(&g_pre_sem);
}

void close_slot(OpenFile& o) {
    if (o.f) fclose(o.f);
    if (o.fd >= 0) raw_close(o.fd);
    o.f = 0;
    o.fd = -1;
}

// Opens the entry's file into `o` (stdio, or raw with MSC_DVD_DIRECT). `fresh`: do not use a handle opened ahead (a retry after an error).
bool open_into(OpenFile& o, int entry, bool fresh, bool helper = false) {
    if (g_sw.direct) {
        int fd = fresh ? -1 : take_preopened(entry);
        if (fd < 0) fd = raw_open(host_path(entry));
        if (fd < 0) {
            // the exact name is not there: look for it with another letter case
            if (const char* alt = corrected_path(entry)) {
                fd = raw_open(alt);
                if (fd >= 0) {
                    np_log("dvd", "%s found as %s", host_path(entry), alt);
                    publish_path(entry, alt);
                } else {
                    free((void*)alt);
                }
            }
        }
        if (fd >= 0) {
            o.fd = fd;
            return true;
        }
        // A handle the raw call cannot make but stdio can would be a platform quirk of the raw call: leave it for good.
        if (FILE* t = fopen(host_path(entry), "rb")) {
            // (only the worker changes the mode: the keep thread's open runs beside it and must not write what the worker reads)
            if (!helper) {
                np_log("dvd", "raw open of %s failed, stdio works: MSC_DVD_DIRECT is off for this run", host_path(entry));
                __atomic_store_n(&g_sw.direct, false, __ATOMIC_RELAXED);
            }
            o.f = t;
            return true;
        }
        return false;
    }
    o.f = fopen(host_path(entry), "rb");
    if (!o.f) {
        if (const char* alt = corrected_path(entry)) {
            o.f = fopen(alt, "rb");
            if (o.f) {
                np_log("dvd", "%s found as %s", host_path(entry), alt);
                publish_path(entry, alt);
            } else {
                free((void*)alt);
            }
        }
    }
    return o.f != 0;
}

OpenFile* open_cached(int entry) {
    int victim = 0;
    for (int i = 0; i < 16; ++i) {
        const bool used = g_open[i].f || g_open[i].fd >= 0;
        if (used && g_open[i].entry == entry) {
            g_open[i].stamp = ++g_stamp;
            return &g_open[i];
        }
        if (!used) victim = i;
        else if ((g_open[victim].f || g_open[victim].fd >= 0) && g_open[i].stamp < g_open[victim].stamp) victim = i;
    }
    close_slot(g_open[victim]);
    const bool ok = open_into(g_open[victim], entry, false);
    g_open[victim].entry = entry;
    g_open[victim].stamp = ++g_stamp;
    return ok ? &g_open[victim] : 0;
}

// ---- MSC_DVD_KEEP_KIB: the keep cache --------------------------------------------------------------------------------------------
// Whole small files in RAM (np/dvd_keep.h). A hit costs a memcpy on the worker thread instead of an open and a read on the card (4-8 ms to open, 5-51 ms to read on the
// maintainer console, by the notes of this file and of vgx_hdtex.cpp). Entries are immutable once filled; a lookup copies under the mutex, so an entry is never freed under
// a copy. Files are filled by a background thread (a hint, or a keepable file the worker had to read from the card); with the virtual clock (the host harness, which has no
// helper threads) a hint fills at once on the calling thread, so the run stays deterministic.
struct KeepEntry { int entry; uint32_t size; uint32_t stamp; uint32_t epoch; uint8_t* data; };
const int kKeepMax = 384;
KeepEntry g_keep[kKeepMax];
int g_keepCount = 0;
uint32_t g_keepDrops = 0;   // files dropped from the cache (to make room, or given back to the heap): a hand-over of a line-up asked for earlier may be gone
uint32_t g_keepBytes = 0, g_keepStamp = 0, g_keepEpoch = 1;
NpMutex g_keep_mutex;
NpSem g_keep_sem;
bool g_keep_ready = false;     // table, mutex and (outside the virtual clock) the filling thread exist
int g_keepQ[512];
uint32_t g_keepQHead = 0, g_keepQTail = 0;

bool keep_extension_ok(const char* path) {
    const size_t n = strlen(path);
    auto ends = [&](const char* e) { const size_t k = strlen(e); return n > k && strcasecmp(path + n - k, e) == 0; };
    return ends(".nis") || ends(".sanim") || ends(".fen");
}

// With the mutex held: the index of the entry's slot, or -1.
int keep_find_locked(int entry) {
    for (int i = 0; i < g_keepCount; ++i)
        if (g_keep[i].entry == entry) return i;
    return -1;
}

// The worker's lookup: copies [fileoff, fileoff + size) of the kept file (zero past its end, as a read of the tail does) and returns true; false = not kept.
bool keep_copy(int entry, uint32_t fileoff, uint32_t size, void* dst) {
    if (!g_keep_ready) return false;
    np_mutex_lock(&g_keep_mutex);
    const int i = keep_find_locked(entry);
    if (i < 0) {
        np_mutex_unlock(&g_keep_mutex);
        return false;
    }
    KeepEntry& k = g_keep[i];
    k.stamp = ++g_keepStamp;
    k.epoch = g_keepEpoch;
    uint32_t have = fileoff < k.size ? k.size - fileoff : 0;
    if (have > size) have = size;
    if (have) memcpy(dst, k.data + fileoff, have);
    if (have < size) memset((char*)dst + have, 0, size - have);
    np_mutex_unlock(&g_keep_mutex);
    return true;
}

// Bytes of C heap still free (Vita: mallinfo against the heap's size); a large number where there is no such limit.
long heap_free_mib() {
#if defined(__vita__)
    if (&_newlib_heap_size_user == 0) return 1 << 20;
    const struct mallinfo mi = mallinfo();
    const unsigned long total = _newlib_heap_size_user, used = (unsigned long)mi.uordblks;
    return total > used ? (long)((total - used) / 1048576ul) : 0;
#else
    const char* t = getenv("NP_TEST_HEAP_FREE_MIB");   // (host tests: a pretend figure)
    return t && *t ? atol(t) : 1 << 20;
#endif
}

// Puts a whole file (malloc'ed, size + 32 bytes) into the table; the cache owns `buf` afterwards, or it is freed here. False: not kept (over the budget with nothing older to drop, table full).
bool keep_store(int entry, uint32_t size, uint8_t* buf) {
    np_mutex_lock(&g_keep_mutex);
    bool stored = false;
    if (keep_find_locked(entry) < 0) {
        // over the budget: drop files of an earlier epoch (a match that is over), oldest use first, never one of this epoch
        while (g_keepBytes + size > g_sw.keepBudget) {
            int victim = -1;
            for (int i = 0; i < g_keepCount; ++i)
                if (g_keep[i].epoch != g_keepEpoch && (victim < 0 || g_keep[i].stamp < g_keep[victim].stamp)) victim = i;
            if (victim < 0) break;
            g_keepBytes -= g_keep[victim].size;
            free(g_keep[victim].data);
        ++g_keepDrops;
            g_keep[victim] = g_keep[--g_keepCount];
        }
        if (g_keepBytes + size <= g_sw.keepBudget && g_keepCount < kKeepMax) {
            g_keep[g_keepCount++] = KeepEntry{entry, size, ++g_keepStamp, g_keepEpoch, buf};
            g_keepBytes += size;
            stored = true;
            __atomic_store_n(&np_stat_dvd_keep_bytes, g_keepBytes, __ATOMIC_RELAXED);
        }
    }
    np_mutex_unlock(&g_keep_mutex);
    if (!stored) {
        free(buf);
        __atomic_fetch_add(&np_stat_dvd_keep_fail[g_keepBytes + size > g_sw.keepBudget ? 3 : 4], 1u, __ATOMIC_RELAXED);
        return false;
    }
    __atomic_fetch_add(&np_stat_dvd_keep_fills, 1u, __ATOMIC_RELAXED);
    return true;
}

// Gives memory back: drops cached files, the ones least needed first (an earlier match's, then the longest unused), until `wantKib` KiB are freed or none is left. Returns KiB freed.
// Called when the free heap falls under MSC_DVD_KEEP_RELEASE_MIB (the keep thread looks twice a second), and by the allocation-failure handler below. A read that is copying
// from a file is not hurt: keep_copy copies under the same mutex.
uint32_t keep_release(uint32_t wantKib) {
    uint32_t freed = 0;
    if (!g_keep_ready) return 0;
    np_mutex_lock(&g_keep_mutex);
    while (g_keepCount > 0 && freed < wantKib * 1024u) {
        int victim = 0;
        for (int i = 1; i < g_keepCount; ++i) {
            const bool vOld = g_keep[victim].epoch != g_keepEpoch, iOld = g_keep[i].epoch != g_keepEpoch;
            if (iOld != vOld ? iOld : g_keep[i].stamp < g_keep[victim].stamp) victim = i;
        }
        freed += g_keep[victim].size;
        g_keepBytes -= g_keep[victim].size;
        free(g_keep[victim].data);
        ++g_keepDrops;
        g_keep[victim] = g_keep[--g_keepCount];
    }
    __atomic_store_n(&np_stat_dvd_keep_bytes, g_keepBytes, __ATOMIC_RELAXED);
    np_mutex_unlock(&g_keep_mutex);
    if (freed) {
        __atomic_fetch_add(&np_stat_dvd_keep_released, freed / 1024u, __ATOMIC_RELAXED);
        fprintf(stderr, "[dvdkeep] released %u KiB (%u files, %u KiB still held): the C heap is short (%ld MiB free)\n", freed / 1024u, static_cast<unsigned>(g_keepCount), g_keepBytes / 1024u, heap_free_mib());
    }
    return freed / 1024u;
}

// A failed C++ allocation anywhere in the port: free cached files and let the allocation try again; with nothing to free, the handler that was there before takes over.
std::new_handler g_prevNewHandler = 0;
void keep_new_handler() {
    if (keep_release(2048) != 0) return;
    std::set_new_handler(g_prevNewHandler);
    if (g_prevNewHandler) g_prevNewHandler();
}

// What a miss reads from the card is kept from that very read (the bytes are in hand), not by a second read of the file. At most four files are being put together; each is
// extended only by a read that continues it (offset = what is held), which is how the game reads a file: front to back, in pieces. Only the worker thread touches this.
struct Assembly { int entry; uint32_t have, size; uint8_t* buf; };
Assembly g_asm[4];
uint32_t g_asmNext = 0;

void asm_drop(Assembly& a) {
    free(a.buf);
    a = Assembly{0, 0, 0, 0};
}

// A disc read of [fileoff, fileoff+size) of `entry` has just completed into `dst`.
void asm_feed(int entry, uint32_t fileoff, uint32_t size, const void* dst) {
    const uint32_t filesize = g_nodes[entry * 3 + 2];
    Assembly* a = 0;
    for (int i = 0; i < 4; ++i)
        if (g_asm[i].entry == entry) a = &g_asm[i];
    if (!a) {
        if (fileoff != 0 || filesize == 0 || filesize > g_sw.keepFileMax) return;   // (only a read from the start begins a file)
        if (g_sw.keepHeapMib > 0 && heap_free_mib() < g_sw.keepHeapMib) {
            __atomic_fetch_add(&np_stat_dvd_keep_fail[0], 1u, __ATOMIC_RELAXED);
            return;
        }
        a = &g_asm[g_asmNext++ % 4];
        asm_drop(*a);
        a->buf = (uint8_t*)malloc(filesize + 32);
        if (!a->buf) {
            keep_release(filesize / 1024u + 64u);
            a->buf = (uint8_t*)malloc(filesize + 32);
        }
        if (!a->buf) {
            __atomic_fetch_add(&np_stat_dvd_keep_fail[1], 1u, __ATOMIC_RELAXED);
            return;
        }
        a->entry = entry;
        a->have = 0;
        a->size = filesize;
    }
    if (fileoff != a->have) {   // not the continuation: give up on this file (a later miss starts again)
        asm_drop(*a);
        return;
    }
    uint32_t n = size;
    if (a->have + n > a->size) n = a->size - a->have;   // (the game rounds a read up to 32 bytes past the end)
    memcpy(a->buf + a->have, dst, n);
    a->have += n;
    if (a->have >= a->size) {
        memset(a->buf + a->size, 0, 32);
        uint8_t* b = a->buf;
        const uint32_t sz = a->size;
        *a = Assembly{0, 0, 0, 0};
        if (keep_store(entry, sz, b)) __atomic_fetch_add(&np_stat_dvd_keep_fills, 1u, __ATOMIC_RELAXED);
    }
}

// Reads the whole file into the cache. False: not kept (too big, over the budget with nothing older to drop, the heap is short, the file did not read).
bool keep_fill(int entry, bool anyExt = false) {
    if (!g_keep_ready || !g_sw.keepBudget || entry <= 0 || (uint32_t)entry >= g_count) return false;
    const uint32_t size = g_nodes[entry * 3 + 2];
    if (size == 0 || size > g_sw.keepFileMax || !(anyExt || keep_extension_ok(host_path(entry)))) return false;
    np_mutex_lock(&g_keep_mutex);
    const bool have = keep_find_locked(entry) >= 0;
    np_mutex_unlock(&g_keep_mutex);
    if (have) return true;
    if (g_sw.keepHeapMib > 0 && heap_free_mib() < g_sw.keepHeapMib) {
        __atomic_fetch_add(&np_stat_dvd_keep_fail[0], 1u, __ATOMIC_RELAXED);
        return false;
    }
    uint8_t* buf = (uint8_t*)malloc(size + 32);
    if (!buf) {
        keep_release(size / 1024u + 64u);
        buf = (uint8_t*)malloc(size + 32);
    }
    if (!buf) {
        __atomic_fetch_add(&np_stat_dvd_keep_fail[1], 1u, __ATOMIC_RELAXED);
        return false;
    }
    OpenFile o = {entry, 0, -1, 0};
    bool ok = open_into(o, entry, true);
    size_t got = 0;
    if (ok) {
        if (o.fd >= 0) {
            const long r = raw_read_all(o.fd, buf, size, 0);
            got = r < 0 ? 0 : (size_t)r;
        } else if (fseek(o.f, 0, SEEK_SET) == 0) {
            got = fread(buf, 1, size, o.f);
        }
    }
    close_slot(o);
    if (!ok || got != size) {
        free(buf);
        __atomic_fetch_add(&np_stat_dvd_keep_fail[2], 1u, __ATOMIC_RELAXED);
        return false;
    }
    return keep_store(entry, size, buf);
}
void keep_report() {
    static uint32_t lastFills = 0xFFFFFFFFu;
    const uint32_t fills = np_stat_dvd_keep_fills;
    if (fills == lastFills) return;
    lastFills = fills;
    fprintf(stderr, "[dvdkeep] queue empty: %u files, %u KiB held; left out: heap floor %u, malloc %u, read %u, budget %u, table %u (heap free %ld MiB)\n", fills, np_stat_dvd_keep_bytes / 1024u,
            np_stat_dvd_keep_fail[0], np_stat_dvd_keep_fail[1], np_stat_dvd_keep_fail[2], np_stat_dvd_keep_fail[3], np_stat_dvd_keep_fail[4], heap_free_mib());
}

void* keep_thread(void*) {
    for (;;) {
        // twice a second: the heap under the release floor gives the cache back (a wait that ends at once when a file is asked for)
        if (np_sem_timedwait_ns(&g_keep_sem, 500000000ull) == 0) {
            if (g_sw.keepReleaseMib > 0 && g_keepBytes && heap_free_mib() < g_sw.keepReleaseMib) keep_release(((uint32_t)(g_sw.keepReleaseMib - heap_free_mib()) + 2u) * 1024u);
            continue;
        }
        for (;;) {
            np_mutex_lock(&g_keep_mutex);
            int entry = -1;
            if (g_keepQHead != g_keepQTail) entry = g_keepQ[g_keepQHead++ % 512];
            np_mutex_unlock(&g_keep_mutex);
            if (entry < 0) {
                keep_report();
                break;
            }
            // a short courtesy to the reads of play (the card is shared with the music stream, which is never idle for long): at most 10 ms a file,
            // so a list of a hundred files is not 40 seconds long (the first version waited up to 400 ms each and the Vita3K run had most files still queued at the first Mega Strike)
            for (int waits = 0; waits < 5; ++waits) {
                np_irq_enter();
                const bool busy = g_executing != 0 || g_qhead[0] || g_qhead[1] || g_qhead[2] || g_qhead[3];
                np_irq_leave();
                if (!busy) break;
                np_sleep_ns(2000000ull);
            }
            keep_fill(entry & 0xFFFFFF, (entry & 0x1000000) != 0);
        }
    }
    return 0;
}

void keep_start() {
    if (g_keep_ready || !g_sw.keepBudget) return;
    np_mutex_init(&g_keep_mutex);
    if (!np_vt_on) {
        np_sem_init(&g_keep_sem, 0);
        NpThread* t = np_thread_create("dvd-keep", NP_ROLE_IO, keep_thread, 0, 0, 65536);
        if (!t) {
            g_sw.keepBudget = 0;
            return;
        }
        np_thread_detach(t);
    }
    g_prevNewHandler = std::set_new_handler(keep_new_handler);
    g_keep_ready = true;
}

// Asks for a file to be kept (the game's hint, or the worker after a miss). Cheap and never blocks on the card.
void keep_request(int entry, bool front = false, bool anyExt = false) {
    if (!g_keep_ready || entry <= 0 || (uint32_t)entry >= g_count) return;
    if (np_vt_on) {
        keep_fill(entry, anyExt);
        return;
    }
    const int tagged = entry | (anyExt ? 0x1000000 : 0);
    np_mutex_lock(&g_keep_mutex);
    bool queued = false;
    if (keep_find_locked(entry) < 0 && g_keepQTail - g_keepQHead < 512) {
        for (uint32_t i = g_keepQHead; i != g_keepQTail; ++i)
            if ((g_keepQ[i % 512] & 0xFFFFFF) == entry) { queued = true; break; }
        if (!queued) {
            if (front) g_keepQ[--g_keepQHead % 512] = tagged;   // a file play just had to wait for goes first
            else g_keepQ[g_keepQTail++ % 512] = tagged;
        }
    } else {
        queued = true;
    }
    np_mutex_unlock(&g_keep_mutex);
    if (!queued) np_sem_post(&g_keep_sem);
}

// One attempt: `got` bytes into dst from `fileoff`. Returns 0 when the read is complete or ended at the file's end, 1 for an error (a short read before the end).
int read_once(OpenFile& o, int entry, void* dst, uint32_t size, uint32_t fileoff, size_t* got) {
    const uint32_t filesize = g_nodes[entry * 3 + 2];
    if (o.fd >= 0) {
        const long r = raw_read_all(o.fd, dst, size, fileoff);
        *got = r < 0 ? 0 : (size_t)r;
        return (r < 0 || (*got < size && fileoff + *got < filesize)) ? 1 : 0;
    }
    if (fseek(o.f, (long)fileoff, SEEK_SET) != 0) {
        *got = 0;
        return 1;
    }
    *got = fread(dst, 1, size, o.f);
    return (*got < size && (ferror(o.f) || fileoff + *got < filesize)) ? 1 : 0;
}

void retry_pause() {
#if defined(__vita__)
    sceKernelDelayThread(50000);
#else
    usleep(50000);
#endif
}

// (wave 14, from the Galaxy port's 20 Nov fix "music after sleep / Home": its reopen ladder grows to 3 s steps within 20 s of a reported resume, because the card can take
// seconds to come back) MSC_DVD_RETRY_RESUME=<n> (default 100 extra attempts, about 5 s more; 0 = off, 1.3's three seconds always): within 20 s of the console waking up (the
// platform's power callback counts it: np_vita_wake_epoch, weak, so a build or test without it never sees a resume) a read or open that fails keeps trying that much longer
// before it ends in the fatal disc error. Elsewhere a failing card still gives up after MSC_DVD_RETRY attempts, so nothing else is slowed down. Only the failure path changes.
extern "C" unsigned np_vita_wake_epoch(void) __attribute__((weak));
static int g_resumeExtra = -1;
static unsigned g_resumeSeen = 0;
static uint64_t g_resumeStampNs = 0;
static int retries_for_read() {
    int n = g_sw.retries;
    if (n <= 0 || !np_vita_wake_epoch) return n;
    if (g_resumeExtra < 0) {
        const char* e = getenv("MSC_DVD_RETRY_RESUME");
        const long v = (e && *e) ? strtol(e, nullptr, 10) : 100;
        g_resumeExtra = v < 0 ? 0 : v > 400 ? 400 : (int)v;
    }
    if (g_resumeExtra == 0) return n;
    const unsigned epoch = np_vita_wake_epoch();
    const uint64_t now = np_time_real_ns();
    if (epoch != g_resumeSeen) { g_resumeSeen = epoch; g_resumeStampNs = now ? now : 1; }
    return (g_resumeStampNs != 0 && now - g_resumeStampNs < 20000000000ull) ? n + g_resumeExtra : n;
}

// Reads `size` bytes at disc address `word` (+ byte remainder is always 0 here). -1 on error.
bool g_lastReadKept = false;   // the last do_read was answered from the keep cache (the virtual-time drive then does not occupy the card)

long do_read(void* dst, uint32_t size, uint32_t word) {
    const Extent* e = find_extent(word);
    if (!e) {
        np_log("dvd", "read at disc address 0x%x (%u bytes) is outside every file", word, size);
        char address[16];   // (1.3) the fatal screen translates this line as a template (np_text.h: %s %u only), so the hex address is made here
        snprintf(address, sizeof address, "0x%x", (unsigned)word);
        read_error("The game requested an unknown disc address (%s, %u bytes).", address, size);
        return -1;
    }
    // MSC_DVD_RETRY (np/dvd_retry.h): attempt 1 through the handle cache; each later one reopens the file (never a pre-opened handle), the first at once,
    // the next 50 ms apart. A read that succeeds the first time runs as before.
    const uint32_t fileoff = (word - e->start) * 4;
    bool wantFeed = false;
    g_lastReadKept = false;
    if (g_sw.keepBudget) {
        if (keep_copy(e->entry, fileoff, size, dst)) {
            __atomic_fetch_add(&np_stat_dvd_keep_hits, 1u, __ATOMIC_RELAXED);
            g_lastReadKept = true;
            static const bool trace = getenv("MSC_DVD_TRACE") != nullptr;
            if (trace) fprintf(stderr, "[dvdtrace] t=%llu %s +%u %u (kept)\n", (unsigned long long)(np_time_ns() / 1000000u), host_path(e->entry), fileoff, size);
            np_notify_write(dst, size);
            return (long)size;
        }
        if (g_nodes[e->entry * 3 + 2] <= g_sw.keepFileMax && keep_extension_ok(host_path(e->entry))) {
            __atomic_fetch_add(&np_stat_dvd_keep_misses, 1u, __ATOMIC_RELAXED);
            wantFeed = true;   // (kept from the bytes this read brings: no second read of the file)
        }
    }
    size_t got = 0;
    int bad = 1;
    OpenFile* of = 0;
    bool openFailed = false;
    int attempt = 0, used = 1;
    np::RetryRead(
        retries_for_read(),
        [&](bool) -> long {
            if (attempt++ == 0 || !of) {
                of = open_cached(e->entry);
            } else if (open_into(*of, e->entry, true)) {
                __atomic_fetch_add(&g_raw_reopens, 1u, __ATOMIC_RELAXED);
            } else {
                of = 0;
            }
            openFailed = !of;
            if (openFailed) {
                got = 0;
                return -1;
            }
            bad = read_once(*of, e->entry, dst, size, fileoff, &got);
            return bad ? -1 : 0;
        },
        [&] {
            if (openFailed) np_log("dvd", "cannot open %s: trying again", host_path(e->entry));
            else np_log("dvd", "read of %s (%u bytes at %u) failed after %lu: reopening and trying again", host_path(e->entry), size, fileoff, (unsigned long)got);
            __atomic_fetch_add(&np_stat_dvd_retries, 1u, __ATOMIC_RELAXED);
            if (of) close_slot(*of);
        },
        [&] {
            if (attempt >= 2) retry_pause();
        },
        &used);
    if (used > 1) np_log("dvd", "%s: %s after %d attempts (MSC_DVD_RETRY=%d)", host_path(e->entry), openFailed || bad ? "still failing" : "recovered", used, g_sw.retries);
    if (openFailed) {
        np_log("dvd", "cannot open %s", host_path(e->entry));
        read_error("Cannot open game file:\n%s", host_path(e->entry));
        return -1;
    }
    __atomic_fetch_add(&np_stat_dvd_reads, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&np_stat_dvd_kib, (uint32_t)((size + 1023u) / 1024u), __ATOMIC_RELAXED);
    // MSC_DVD_TRACE=1: one line per disc read (clock in ms, file, offset, size) for the analysis of what is read twice
    static const bool trace = getenv("MSC_DVD_TRACE") != nullptr;
    if (trace) fprintf(stderr, "[dvdtrace] t=%llu %s +%u %u\n", (unsigned long long)(np_time_ns() / 1000000u), host_path(e->entry), fileoff, size);
    np_notify_write(dst, size);   // a disc read wrote there (the renderer may hold a decode of the old contents)
    if (wantFeed && !bad && got == size) asm_feed(e->entry, fileoff, size, dst);
    if (got < size) {
        // reads past the end of a file are rounded up to 32 bytes by the game: zero the tail
        uint32_t filesize = g_nodes[e->entry * 3 + 2];
        if (!bad) {
            memset((char*)dst + got, 0, size - got);
        } else {
            np_log("dvd", "short read in %s (%lu of %u at %u)", host_path(e->entry), (unsigned long)got, size, fileoff);
            read_error("Game file short read:\n%s\nRead %lu of %u bytes at %u; disc expects %u bytes.",
                       host_path(e->entry), (unsigned long)got, size, fileoff, filesize);
            return -1;
        }
    }
    return (long)size;
}

DVDCommandBlock* dequeue_locked() {
    for (int p = 0; p < 4; ++p) {
        DVDCommandBlock* b = g_qhead[p];
        if (b) {
            g_qhead[p] = b->next;
            if (g_qhead[p]) g_qhead[p]->prev = 0;
            else g_qtail[p] = 0;
            b->next = b->prev = 0;
            return b;
        }
    }
    return 0;
}

void* io_thread(void*) {
    for (;;) {
        np_sem_wait(&g_io_sem);
        for (;;) {
            np_irq_enter();
            DVDCommandBlock* blk = dequeue_locked();
            if (!blk) {
                np_irq_leave();
                break;
            }
            blk->state = DVD_STATE_BUSY;
            g_executing = blk;
            void* dst = blk->addr;
            uint32_t len = blk->length;
            uint32_t off = blk->offset;
            np_irq_leave();

            long r = do_read(dst, len, off);

            np_irq_enter();
            g_executing = 0;
            if (blk->state == DVD_STATE_BUSY) {
                if (r < 0) {
                    blk->state = DVD_STATE_FATAL;
                    g_fatal = 1;
                } else {
                    blk->transferTotal = (u32)r;
                    blk->transferSize = (u32)r;
                    blk->state = DVD_STATE_IDLE;   // "END"
                }
                DVDCommandCallback cb = blk->callback;
                if (cb) cb(r < 0 ? DVD_RESULT_FATAL_ERROR : (s32)r, blk);
            }
            OSWakeupThread(&g_wait_queue);
            OSWakeupThread(&__DVDThreadQueue);
            np_irq_leave();
        }
    }
    return 0;
}

// ---- virtual time: the drive as an event source -------------------------------------------------
// One request at a time, best priority first (FIFO inside a priority), served from the moment the drive is free: the host read
// happens when the request STARTS (deterministic: it is driven by the running thread's own calls), the completion event
// (callback in interrupt context, wake-ups) falls due after a modelled seek + transfer time on the virtual clock.
// NP_DVD_MBPS (default 20) and NP_DVD_SEEK_US (default 300) set the model.
int g_vic_id = -1;
uint64_t g_drive_free_ns = 0;
long g_vresult = 0;

uint64_t dvd_latency_ns(uint32_t bytes) {
    static double mbps = -1.0;
    static uint64_t seek_ns = 0;
    if (mbps < 0.0) {
        const char* e = getenv("NP_DVD_MBPS");
        mbps = (e && *e) ? atof(e) : 20.0;
        if (mbps <= 0.0) mbps = 20.0;
        e = getenv("NP_DVD_SEEK_US");
        seek_ns = (uint64_t)((e && *e) ? atof(e) : 300.0) * 1000ull;
    }
    return seek_ns + (uint64_t)((double)bytes * 1000.0 / mbps);
}

// With the interrupt lock held: starts the next queued request if the drive is idle.
void vdvd_kick() {
    if (g_executing) return;
    DVDCommandBlock* blk = dequeue_locked();
    if (!blk) return;
    blk->state = DVD_STATE_BUSY;
    g_executing = blk;
    g_vresult = do_read(blk->addr, blk->length, blk->offset);
    const uint64_t now = np_vt_now();
    if (g_lastReadKept) {
        // answered from memory: a copy (NP_DVD_KEEP_HIT_US, default 50), the card is not occupied. NP_DVD_KEEP_HIT_US=-1: charge the card's time as for a miss (a control).
        static long hitUs = -2;
        if (hitUs == -2) {
            const char* h = getenv("NP_DVD_KEEP_HIT_US");
            hitUs = (h && *h) ? atol(h) : 50;
        }
        if (hitUs >= 0) {
            np_vic_arm(g_vic_id, now + (uint64_t)hitUs * 1000ull);
            return;
        }
    }
    const uint64_t start = g_drive_free_ns > now ? g_drive_free_ns : now;
    g_drive_free_ns = start + dvd_latency_ns(blk->length);
    np_vic_arm(g_vic_id, g_drive_free_ns);
}

void vdvd_complete(void*, uint64_t) {
    np_irq_enter();
    DVDCommandBlock* blk = g_executing;
    static const bool compLog = getenv("NP_DVD_COMPLOG") != nullptr;
    if (compLog && blk) fprintf(stderr, "[dvdcomp] t=%llu us len %u\n", (unsigned long long)(np_vt_now() / 1000ull), (unsigned)blk->length);
    long r = g_vresult;
    g_executing = 0;
    if (blk && blk->state == DVD_STATE_BUSY) {
        if (r < 0) {
            blk->state = DVD_STATE_FATAL;
            g_fatal = 1;
        } else {
            blk->transferTotal = (u32)r;
            blk->transferSize = (u32)r;
            blk->state = DVD_STATE_IDLE;   // "END"
        }
        DVDCommandCallback cb = blk->callback;
        if (cb) cb(r < 0 ? DVD_RESULT_FATAL_ERROR : (s32)r, blk);
    }
    OSWakeupThread(&g_wait_queue);
    OSWakeupThread(&__DVDThreadQueue);
    vdvd_kick();   // the next request starts where this one ended
    np_irq_leave();
}

void remove_from_queue(DVDCommandBlock* b) {
    for (int p = 0; p < 4; ++p) {
        DVDCommandBlock* it = g_qhead[p];
        while (it && it != b) it = it->next;
        if (it != b) continue;
        if (b->prev) b->prev->next = b->next; else g_qhead[p] = b->next;
        if (b->next) b->next->prev = b->prev; else g_qtail[p] = b->prev;
        b->next = b->prev = 0;
        return;
    }
}

}  // namespace

extern "C" {

int NpDiscGetReadError(char* details, size_t capacity) {
    const BOOL interrupts = OSDisableInterrupts();
    const int present = g_read_error[0] != 0;
    if (details && capacity) snprintf(details, capacity, "%s", g_read_error);
    OSRestoreInterrupts(interrupts);
    return present;
}


// np/dvd_keep.h
unsigned NpDvdKeepDrops(void) {
    if (!g_keep_ready) return 0;
    np_mutex_lock(&g_keep_mutex);
    const unsigned d = g_keepDrops;
    np_mutex_unlock(&g_keep_mutex);
    return d;
}

void NpDvdKeepBegin(void) {
    if (!g_inited) DVDInit();
    if (!g_keep_ready) return;
    np_mutex_lock(&g_keep_mutex);
    ++g_keepEpoch;   // files of the earlier epochs may be dropped to make room for this one's
    np_mutex_unlock(&g_keep_mutex);
}

int NpDvdKeepHint(const char* path) {
    if (!path || !g_inited || !g_keep_ready) return 0;
    const long e = DVDConvertPathToEntrynum(path);
    if (e <= 0) return 0;
    const uint32_t size = g_nodes[e * 3 + 2];
    if (size == 0 || size > g_sw.keepFileMax || !keep_extension_ok(host_path((int)e))) return 0;
    keep_request((int)e, false, true);
    return 1;
}

// Hints every file (of any type up to the size cap) directly inside the directory, in the order of the table. Returns how many were hinted.
int NpDvdKeepHintDir(const char* path) {
    if (!path || !g_inited || !g_keep_ready) return 0;
    const long d = DVDConvertPathToEntrynum(path);
    if (d <= 0 || (g_nodes[d * 3] >> 24) == 0) return 0;
    const uint32_t end = g_nodes[d * 3 + 2];
    int n = 0;
    for (uint32_t i = (uint32_t)d + 1; i < end && i < g_count;) {
        if (g_nodes[i * 3] >> 24) {
            i = g_nodes[i * 3 + 2];   // a sub-directory: skipped whole
            continue;
        }
        const uint32_t size = g_nodes[i * 3 + 2];
        if (size && size <= g_sw.keepFileMax) {
            keep_request((int)i, false, true);
            ++n;
        }
        ++i;
    }
    return n;
}

int NpDvdKeepStats(unsigned* files, unsigned* bytes, unsigned* hits, unsigned* misses) {
    if (files) {
        unsigned n = 0;
        if (g_keep_ready) {
            np_mutex_lock(&g_keep_mutex);
            n = (unsigned)g_keepCount;
            np_mutex_unlock(&g_keep_mutex);
        }
        *files = n;
    }
    if (bytes) *bytes = np_stat_dvd_keep_bytes;
    if (hits) *hits = np_stat_dvd_keep_hits;
    if (misses) *misses = np_stat_dvd_keep_misses;
    return g_sw.keepBudget != 0;
}

void DVDInit(void) {
    if (g_inited) return;
    g_inited = 1;
    np_os_lazy_init();
    const NpSdkConfig* cfg = NpSdk_GetConfig();
    snprintf(g_disc, sizeof(g_disc), "%s", cfg->disc_dir);
    char path[1024];
    char filesdir[1024];
    snprintf(filesdir, sizeof(filesdir), "%s/files", g_disc);
    snprintf(path, sizeof(path), "%s/sys/fst.bin", g_disc);
    bool ok = load_fst_file(path);
    if (ok) np_log("dvd", "file system table %s: %u entries", path, g_count);
    if (!ok) ok = build_fst_from_scan(filesdir);
    if (!ok) {
        np_log("dvd", "no disc at %s (expected sys/fst.bin or files/)", g_disc);
        read_error("No extracted game files at:\n%s\nExpected sys/fst.bin and files/.", g_disc);
        g_fatal = 1;
        static const uint32_t emptyroot[3] = {0x01000000u, 0, 1};
        static char emptynames[1] = {0};
        np_dvdfs_set_fst(emptyroot, emptynames, 1);
    } else {
        build_paths(filesdir);
        np_dvdfs_set_fst(g_nodes, g_names, g_count);
    }
    memset(&g_diskid, 0, sizeof(g_diskid));
    memcpy(g_diskid.game, "R4QE", 4);
    memcpy(g_diskid.company, "01", 2);
    snprintf(path, sizeof(path), "%s/sys/boot.bin", g_disc);
    if (FILE* f = fopen(path, "rb")) {
        uint8_t id[32];
        if (fread(id, 1, 32, f) == 32) {
            memcpy(g_diskid.game, id, 4);
            memcpy(g_diskid.company, id + 4, 2);
            g_diskid.disk = id[6];
            g_diskid.version = id[7];
            g_diskid.strmEnable = id[8];
            g_diskid.strmBufSize = id[9];
            g_diskid.rvlMagic = rd32be(id + 0x18);
            g_diskid.gcMagic = rd32be(id + 0x1C);
        }
        fclose(f);
    }
    OSInitThreadQueue(&g_wait_queue);
    OSInitThreadQueue(&__DVDThreadQueue);
    np_sem_init(&g_io_sem, 0);
    switches_init();
    if (!np_vt_on) preopen_start();
    keep_start();
    np_log("dvd", "loading: direct %d, chunk %u KiB, retry %d, pre-open %d, keep %u KiB (files up to %u KiB)", g_sw.direct, g_sw.chunk / 1024u, g_sw.retries, g_sw.preopen,
           g_sw.keepBudget / 1024u, g_sw.keepFileMax / 1024u);
    if (np_vt_on) {
        g_vic_id = np_vic_add("dvd", vdvd_complete, 0);
        return;
    }
    NpThread* t = np_thread_create("dvd-io", NP_ROLE_IO, io_thread, 0, 0, 262144);
    if (t) np_thread_detach(t);
}

BOOL DVDReadAbsAsyncPrio(DVDCommandBlock* block, void* dst, u32 size, u32 offset,
                         DVDCommandCallback callback, s32 prio) {
    if (!g_inited) DVDInit();
    if (prio < 0) prio = 0;
    if (prio > 3) prio = 3;
    BOOL en = OSDisableInterrupts();
    block->command = 1;   // read
    block->state = DVD_STATE_WAITING;
    block->addr = dst;
    block->length = size;
    block->offset = offset;
    block->transferTotal = 0;
    block->transferSize = 0;
    block->callback = callback;
    block->id = &g_diskid;
    block->next = 0;
    block->prev = g_qtail[prio];
    if (g_qtail[prio]) g_qtail[prio]->next = block; else g_qhead[prio] = block;
    g_qtail[prio] = block;
    if (np_vt_on) {
        vdvd_kick();
        OSRestoreInterrupts(en);
        return TRUE;
    }
    OSRestoreInterrupts(en);
    preopen_hint(offset);   // (MSC_DVD_PREOPEN)
    np_sem_post(&g_io_sem);
    return TRUE;
}

BOOL DVDInquiryAsync(DVDCommandBlock* block, DVDDriveInfo* info, DVDCommandCallback callback) {
    memset(info, 0, sizeof(*info));
    info->revision = 0x0001;
    info->deviceCode = 0x8201;
    info->releaseDate = 0x20070101;
    block->state = DVD_STATE_IDLE;
    block->transferTotal = 0x20;
    if (callback) callback(0x20, block);
    return TRUE;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) {
    np_vt_poll_api();   // (virtual time) a loop on the status sees time pass
    BOOL en = OSDisableInterrupts();
    s32 s = block->state;
    OSRestoreInterrupts(en);
    return s;
}

s32 DVDGetDriveStatus(void) {
    np_vt_poll_api();
    BOOL en = OSDisableInterrupts();
    s32 s = DVD_STATE_IDLE;
    if (g_fatal) s = DVD_STATE_FATAL;
    else if (g_executing) s = DVD_STATE_BUSY;
    OSRestoreInterrupts(en);
    return s;
}

static BOOL cancel_common(DVDCommandBlock* block, DVDCommandCallback callback, BOOL wait) {
    BOOL en = OSDisableInterrupts();
    switch (block->state) {
    case DVD_STATE_WAITING:
        remove_from_queue(block);
        block->state = DVD_STATE_CANCELED;
        if (block->callback) block->callback(DVD_RESULT_CANCELED, block);
        if (callback) callback(0, block);
        break;
    case DVD_STATE_BUSY:
        // the worker is reading into the caller's buffer: let it finish
        block->callback = 0;
        if (wait) {
            while (block->state == DVD_STATE_BUSY) OSSleepThread(&g_wait_queue);
        }
        if (callback) callback(0, block);
        break;
    default:
        if (callback) callback(0, block);
        break;
    }
    OSRestoreInterrupts(en);
    return TRUE;
}

BOOL DVDCancelAsync(DVDCommandBlock* block, DVDCommandCallback callback) {
    return cancel_common(block, callback, FALSE);
}
s32 DVDCancel(DVDCommandBlock* block) {
    cancel_common(block, 0, TRUE);
    return 0;
}
BOOL DVDCancelAllAsync(DVDCommandCallback callback) {
    BOOL en = OSDisableInterrupts();
    for (int p = 0; p < 4; ++p) {
        while (DVDCommandBlock* b = g_qhead[p]) {
            remove_from_queue(b);
            b->state = DVD_STATE_CANCELED;
            if (b->callback) b->callback(DVD_RESULT_CANCELED, b);
        }
    }
    OSRestoreInterrupts(en);
    if (callback) callback(0, 0);
    return TRUE;
}

void DVDPause(void) {}
void DVDResume(void) {}
BOOL DVDSetAutoInvalidation(BOOL autoInval) { (void)autoInval; return TRUE; }
const DVDDiskID* DVDGetCurrentDiskID(void) { if (!g_inited) DVDInit(); return &g_diskid; }
u32 __DVDGetCoverStatus(void) { return 2; /* closed */ }
void __DVDPrepareResetAsync(DVDCommandCallback callback) { if (callback) callback(0, 0); }
void __DVDPrepareReset(void) {}
BOOL __DVDTestAlarm(const OSAlarm* alarm) { (void)alarm; return FALSE; }
BOOL __DVDLowBreak(void) { return TRUE; }
BOOL __DVDStopMotorAsync(DVDCommandBlock* block, DVDCommandCallback callback) {
    block->state = DVD_STATE_IDLE;
    if (callback) callback(0, block);
    return TRUE;
}
void __DVDRestartMotor(void) {}

}  // extern "C"
