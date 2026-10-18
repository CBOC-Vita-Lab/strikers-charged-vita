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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "np_internal.h"

extern "C" void np_dvdfs_set_fst(const void* nodes, char* strings, u32 count);

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
char g_disc[512];

// command queue (four priorities, FIFO inside each), guarded by the interrupt lock
DVDCommandBlock* g_qhead[4];
DVDCommandBlock* g_qtail[4];
DVDCommandBlock* g_executing = 0;
NpSem g_io_sem;
OSThreadQueue g_wait_queue;

// file handle cache of the worker thread
struct OpenFile { int entry; FILE* f; uint32_t stamp; };
OpenFile g_open[16];
uint32_t g_stamp = 0;

uint32_t swap32(uint32_t v) { return __builtin_bswap32(v); }

uint32_t rd32be(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// ---- building the table -----------------------------------------------------------------------

bool load_fst_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 12 || size > 16 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    uint8_t* raw = (uint8_t*)malloc((size_t)size);
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
    g_nodes = (uint32_t*)malloc((size_t)count * 12);
    for (uint32_t i = 0; i < count * 3; ++i) g_nodes[i] = rd32be(raw + i * 4);
    size_t strBytes = (size_t)size - (size_t)count * 12;
    g_names = (char*)malloc(strBytes + 1);
    memcpy(g_names, raw + (size_t)count * 12, strBytes);
    g_names[strBytes] = 0;
    g_count = count;
    free(raw);
    return true;
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

FILE* open_cached(int entry) {
    int victim = 0;
    for (int i = 0; i < 16; ++i) {
        if (g_open[i].f && g_open[i].entry == entry) {
            g_open[i].stamp = ++g_stamp;
            return g_open[i].f;
        }
        if (!g_open[i].f) victim = i;
        else if (g_open[victim].f && g_open[i].stamp < g_open[victim].stamp) victim = i;
    }
    if (g_open[victim].f) fclose(g_open[victim].f);
    g_open[victim].f = fopen(g_hostpath[entry], "rb");
    if (!g_open[victim].f) {
        // the exact name is not there: look for it with another letter case (build_paths no longer does this for every file)
        const size_t base = strlen(g_filesdir);
        const char* path = g_hostpath[entry];
        if (strncmp(path, g_filesdir, base) == 0 && path[base] == '/') {
            char full[1100];
            resolve_case(full, sizeof(full), g_filesdir, path + base + 1);
            if (strcmp(full, path) != 0) {
                g_open[victim].f = fopen(full, "rb");
                if (g_open[victim].f) {
                    np_log("dvd", "%s found as %s", path, full);
                    free(g_hostpath[entry]);
                    g_hostpath[entry] = strdup(full);
                }
            }
        }
    }
    g_open[victim].entry = entry;
    g_open[victim].stamp = ++g_stamp;
    return g_open[victim].f;
}

// Reads `size` bytes at disc address `word` (+ byte remainder is always 0 here). -1 on error.
long do_read(void* dst, uint32_t size, uint32_t word) {
    const Extent* e = find_extent(word);
    if (!e) {
        np_log("dvd", "read at disc address 0x%x (%u bytes) is outside every file", word, size);
        return -1;
    }
    FILE* f = open_cached(e->entry);
    if (!f) {
        np_log("dvd", "cannot open %s", g_hostpath[e->entry]);
        return -1;
    }
    uint32_t fileoff = (word - e->start) * 4;
    if (fseek(f, (long)fileoff, SEEK_SET) != 0) return -1;
    size_t got = fread(dst, 1, size, f);
    np_notify_write(dst, size);   // a disc read wrote there (the renderer may hold a decode of the old contents)
    if (got < size) {
        // reads past the end of a file are rounded up to 32 bytes by the game: zero the tail
        uint32_t filesize = g_nodes[e->entry * 3 + 2];
        if (fileoff + got >= filesize) {
            memset((char*)dst + got, 0, size - got);
        } else {
            np_log("dvd", "short read in %s (%lu of %u at %u)", g_hostpath[e->entry], (unsigned long)got, size, fileoff);
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
    const uint64_t start = g_drive_free_ns > now ? g_drive_free_ns : now;
    g_drive_free_ns = start + dvd_latency_ns(blk->length);
    np_vic_arm(g_vic_id, g_drive_free_ns);
}

void vdvd_complete(void*, uint64_t) {
    np_irq_enter();
    DVDCommandBlock* blk = g_executing;
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
