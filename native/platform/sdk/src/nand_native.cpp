// NAND file system: saves and settings of the console, stored under <data_dir>/NAND on the host
// (the Vita: ux0:data/mscharged/NAND, the recompiled build's directory). Paths are the console's ("/title/00010000/52345145/data/..."),
// relative ones resolve against the current directory (NANDChangeDir). The game works through the
// *Async functions with a command block and callback; those run on a worker thread and the callback
// is delivered in interrupt context, like the SDK's IOS completion. Synchronous variants run inline.
#include <revolution/nand.h>
#include <revolution/os.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "np_internal.h"
#include "../save/np_safe_file.h"

namespace {

char g_root[512];
char g_cwd[128] = "/";
int g_inited = 0;
const char kHome[] = "/title/00010000/52345145/data";   // "R4QE" as hex: the title's data directory

// ---- path handling ----------------------------------------------------------------------------

// Makes an absolute, normalised NAND path from `path` (relative to the current directory).
bool make_abs(char* out, size_t cap, const char* path) {
    char tmp[512];
    if (!path || !*path) return false;
    if (path[0] == '/') snprintf(tmp, sizeof(tmp), "%s", path);
    else snprintf(tmp, sizeof(tmp), "%s/%s", g_cwd, path);
    // split on '/', resolve "." and ".."
    char* parts[64];
    int n = 0;
    char* save = 0;
    for (char* p = strtok_r(tmp, "/", &save); p; p = strtok_r(0, "/", &save)) {
        if (!strcmp(p, ".")) continue;
        if (!strcmp(p, "..")) { if (n) --n; continue; }
        if (n >= 64) return false;
        parts[n++] = p;
    }
    size_t len = 0;
    out[0] = 0;
    if (n == 0) { snprintf(out, cap, "/"); return true; }
    for (int i = 0; i < n; ++i) {
        int w = snprintf(out + len, cap - len, "/%s", parts[i]);
        if (w < 0 || (size_t)w >= cap - len) return false;
        len += (size_t)w;
    }
    return true;
}

void host_path(char* out, size_t cap, const char* abs) { snprintf(out, cap, "%s%s", g_root, abs); }

// The game's save files ("Strikers2", and "Online" in the nocopy directory) are the player's progress. They are written the way the Wii's NANDSafeOpen does
// it: into a copy, which replaces the file when it is closed (platform/sdk/save/np_safe_file.h: synced, the previous good save kept as <name>.bak, a damaged
// file set aside as <name>.bad and a restart's first open completes or undoes an interrupted replace). The format of the files is not changed. Other
// files of the directory (banner.bin, the replay and cache files) are written in place as before.
bool is_save_file(const char* abs) {
    const char* base = strrchr(abs, '/');
    base = base ? base + 1 : abs;
    return !strcmp(base, "Strikers2") || !strcmp(base, "Online");
}
bool is_tmp_file(const char* abs) {
    return !strncmp(abs, "/tmp/", 5);
}
void clear_tmp_dir(const char* tmp_dir) {
    DIR* d = opendir(tmp_dir);
    if (!d) return;
    while (struct dirent* de = readdir(d)) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char hp[1200];
        int w = snprintf(hp, sizeof(hp), "%s/%s", tmp_dir, de->d_name);
        if (w < 0 || (size_t)w >= sizeof(hp)) continue;
        struct stat st;
        if (stat(hp, &st) == 0 && S_ISREG(st.st_mode)) unlink(hp);
    }
    closedir(d);
}
const u8 kStageShadow = 1;   // NANDFileInfo::stage of a file opened for writing through a copy

void note_recovery(const char* hp, NpSafeFile::Found f) {
    if (f == NpSafeFile::Found::Nothing || f == NpSafeFile::Found::Primary) return;
    fprintf(stderr, "[nand] %s: %s\n", hp, NpSafeFile::FoundName(f));
}

s32 errno_result() {
    switch (errno) {
    case ENOENT: return NAND_RESULT_NOEXISTS;
    case EEXIST: return NAND_RESULT_EXISTS;
    case ENOTEMPTY: return NAND_RESULT_NOTEMPTY;
    case EACCES:
    case EPERM:
    case EROFS: return NAND_RESULT_ACCESS;
    case ENOSPC:
    case EDQUOT: return NAND_RESULT_MAXBLOCKS;
    case EMFILE:
    case ENFILE: return NAND_RESULT_MAXFD;
    case EINVAL:
    case ENOTDIR:
    case EISDIR:
    case EBADF: return NAND_RESULT_INVALID;
    default: return NAND_RESULT_UNKNOWN;
    }
}


// MSC_BOOT_MATCH (the test-only fast boot, platform/harness/src/np_harness.cpp): the run skips the menus where the game loads and creates its save, and the
// player's progress must never be touched by a test run, so while the switch is on every operation that would change the NAND (create, create directory,
// delete, move, open for writing and what is written through such a handle) is DROPPED: it reports success, nothing reaches the files, and the first few
// are logged ("boot: ... dropped"). A handle opened for writing is a stub (stage kStageDrop): reads give 0 bytes, writes and the close succeed.
// The crash recovery of a save on open (NpSafeFile::Recover: finishes or undoes an interrupted replace, sets a damaged file aside) renames files too, so it
// is skipped as well: a save is only ever READ. Off (the default): one flag test per call, nothing else changes.
const u8 kStageDrop = 2;
bool nosave() {
    // (read at each call, not once: a route's `bootmatch` line sets the variable at the harness's start, after the first NAND call)
    const char* e = getenv("MSC_BOOT_MATCH");
    return e && *e && strcmp(e, "0") != 0 && strcmp(e, "off") != 0;
}
unsigned g_dropped = 0;
void dropped(const char* what, const char* path) {
    ++g_dropped;
    if (g_dropped <= 12) fprintf(stderr, "boot: NAND %s dropped (MSC_BOOT_MATCH: saves are never written) %s\n", what, path ? path : "");
}

// ---- operations (synchronous cores) ------------------------------------------------------------

s32 op_create(const char* path, u8, u8) {
    if (nosave()) { dropped("create", path); return NAND_RESULT_OK; }
    char abs[512], hp[1100];
    if (!make_abs(abs, sizeof(abs), path)) return NAND_RESULT_INVALID;
    host_path(hp, sizeof(hp), abs);
    if (is_save_file(abs) && !nosave()) note_recovery(hp, NpSafeFile::Recover(hp, NpSafeFile::GameSaveOptions()));   // (an interrupted replace is finished before "does it exist" is asked)
    int fd = open(hp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) return errno_result();
    close(fd);
    return NAND_RESULT_OK;
}
s32 op_create_dir(const char* path, u8, u8) {
    if (nosave()) { dropped("create directory", path); return NAND_RESULT_OK; }
    char abs[512], hp[1100];
    if (!make_abs(abs, sizeof(abs), path)) return NAND_RESULT_INVALID;
    host_path(hp, sizeof(hp), abs);
    if (mkdir(hp, 0777) != 0) return errno_result();
    return NAND_RESULT_OK;
}
s32 op_delete(const char* path) {
    if (nosave()) { dropped("delete", path); return NAND_RESULT_OK; }
    char abs[512], hp[1100];
    if (!make_abs(abs, sizeof(abs), path)) return NAND_RESULT_INVALID;
    host_path(hp, sizeof(hp), abs);
    struct stat st;
    const bool present = stat(hp, &st) == 0;
    if (is_save_file(abs) && !(present && S_ISDIR(st.st_mode))) {
        // The player's delete (the "save is damaged, delete?" popup, or the game's own delete): the live file is set aside (<name>.bad, at most three kept) and the
        // .bak and .tmp go with it, so the older backup does not come back at the next start. Set-aside .bad copies stay, for a manual rescue. This is done even
        // when the live file is gone (a damaged one set aside when it was opened): its backup must not outlive the delete.
        if (!NpSafeFile::Discard(hp)) { errno = ENOENT; return errno_result(); }
        return NAND_RESULT_OK;
    }
    if (!present) return errno_result();
    int r = S_ISDIR(st.st_mode) ? rmdir(hp) : unlink(hp);
    return r == 0 ? NAND_RESULT_OK : errno_result();
}
s32 op_open(const char* path, NANDFileInfo* info, u8 mode) {
    if (nosave() && mode != NAND_ACCESS_READ) {
        dropped("open for writing", path);
        memset(info, 0, sizeof(*info));
        info->fd = -1;
        info->tempFd = -1;
        info->access = mode;
        info->stage = kStageDrop;
        snprintf(info->openPath, sizeof(info->openPath), "%s", path);
        return NAND_RESULT_OK;
    }
    char abs[512], hp[1100];
    if (!make_abs(abs, sizeof(abs), path)) return NAND_RESULT_INVALID;
    // int29: under MSC_BOOT_MATCH (nosave), writes to /tmp (e.g. REPLAY_0..2) are dropped, so reading a stale /tmp file
    // left over from a prior run would load stale heap pointers into ReplayManager::mMemory and crash during highlights.
    if (nosave() && is_tmp_file(abs)) return NAND_RESULT_NOEXISTS;
    host_path(hp, sizeof(hp), abs);
    const bool save = is_save_file(abs);
    int fd;
    bool shadow = false;
    if (save) {
        NpSafeFile::Options o = NpSafeFile::GameSaveOptions();
        o.removeEmpty = mode == NAND_ACCESS_READ;   // (a file the game has just created is empty and about to be written)
        if (!nosave()) note_recovery(hp, NpSafeFile::Recover(hp, o));   // (Recover renames and deletes files: never while MSC_BOOT_MATCH is on)
        if (mode == NAND_ACCESS_READ) {
            fd = open(hp, O_RDONLY);
        } else {
            struct stat st;
            if (stat(hp, &st) != 0) return errno_result();   // (the game creates the file first and gets "no file" here, as before)
            NpSafeFile::Writer w;
            if (!NpSafeFile::BeginWrite(hp, &w, o)) return NAND_RESULT_ACCESS;
            fd = w.fd;
            shadow = true;
        }
    } else {
        int flags = mode == NAND_ACCESS_READ ? O_RDONLY : mode == NAND_ACCESS_WRITE ? O_WRONLY : O_RDWR;
        fd = open(hp, flags);
    }
    if (fd < 0) return errno_result();
    memset(info, 0, sizeof(*info));
    info->fd = fd;
    info->tempFd = -1;
    info->access = mode;
    info->stage = shadow ? kStageShadow : 0;
    snprintf(info->openPath, sizeof(info->openPath), "%s", abs);
    return NAND_RESULT_OK;
}
s32 op_close(NANDFileInfo* info) {
    if (info->stage == kStageDrop) { info->stage = 0; return NAND_RESULT_OK; }
    if (info->fd < 0) return NAND_RESULT_INVALID;
    if (info->stage == kStageShadow) {
        char hp[1100];
        host_path(hp, sizeof(hp), info->openPath);
        NpSafeFile::Writer w;
        w.fd = info->fd;
        w.path = hp;
        w.keepBackup = true;
        info->fd = -1;
        info->stage = 0;
        return NpSafeFile::Commit(&w, NpSafeFile::GameSaveOptions()) ? NAND_RESULT_OK : NAND_RESULT_UNKNOWN;
    }
    close(info->fd);
    info->fd = -1;
    return NAND_RESULT_OK;
}
s32 op_read(NANDFileInfo* info, void* buf, u32 len) {
    if (info->stage == kStageDrop) return 0;
    if (info->fd < 0) return NAND_RESULT_INVALID;
    ssize_t r = read(info->fd, buf, len);
    return r < 0 ? errno_result() : (s32)r;
}
s32 op_write(NANDFileInfo* info, const void* buf, u32 len) {
    if (info->stage == kStageDrop) { dropped("write", info->openPath); return (s32)len; }
    if (info->fd < 0) return NAND_RESULT_INVALID;
    ssize_t r = write(info->fd, buf, len);
    if (r < 0) return errno_result();
    return (s32)r;
}
s32 op_seek(NANDFileInfo* info, s32 off, NANDSeekMode whence) {
    if (info->stage == kStageDrop) return 0;
    if (info->fd < 0) return NAND_RESULT_INVALID;
    off_t r = lseek(info->fd, off, whence == NAND_SEEK_BEG ? SEEK_SET : whence == NAND_SEEK_CUR ? SEEK_CUR : SEEK_END);
    return r < 0 ? errno_result() : (s32)r;
}
s32 op_getlength(NANDFileInfo* info, u32* length) {
    if (info->stage == kStageDrop) { *length = 0; return NAND_RESULT_OK; }
    struct stat st;
    if (info->fd < 0 || fstat(info->fd, &st) != 0) return NAND_RESULT_INVALID;
    *length = (u32)st.st_size;
    return NAND_RESULT_OK;
}
s32 op_changedir(const char* path) {
    char abs[512], hp[1100];
    if (!make_abs(abs, sizeof(abs), path)) return NAND_RESULT_INVALID;
    host_path(hp, sizeof(hp), abs);
    if (!np_dir_exists(hp)) return NAND_RESULT_NOEXISTS;
    snprintf(g_cwd, sizeof(g_cwd), "%s", abs);
    return NAND_RESULT_OK;
}
s32 op_check(u32 blocks, u32 files, u32* answer) {
    (void)blocks; (void)files;
    if (answer) *answer = 0;   // enough room (the host's free space is not modelled)
    return NAND_RESULT_OK;
}
s32 op_getstatus(const char* path, NANDStatus* status) {
    char abs[512], hp[1100];
    if (!make_abs(abs, sizeof(abs), path)) return NAND_RESULT_INVALID;
    if (nosave() && is_tmp_file(abs)) return NAND_RESULT_NOEXISTS;
    host_path(hp, sizeof(hp), abs);
    struct stat st;
    if (stat(hp, &st) != 0) return errno_result();
    memset(status, 0, sizeof(*status));
    status->perm = 0x3F;
    return NAND_RESULT_OK;
}
s32 op_gettype(const char* path, u8* type) {
    char abs[512], hp[1100];
    if (!make_abs(abs, sizeof(abs), path)) return NAND_RESULT_INVALID;
    if (nosave() && is_tmp_file(abs)) return NAND_RESULT_NOEXISTS;
    host_path(hp, sizeof(hp), abs);
    struct stat st;
    if (stat(hp, &st) != 0) return errno_result();
    *type = S_ISDIR(st.st_mode) ? NAND_FILE_TYPE_DIR : NAND_FILE_TYPE_FILE;
    return NAND_RESULT_OK;
}
s32 op_move(const char* from, const char* to) {
    if (nosave()) { dropped("move", to); return NAND_RESULT_OK; }
    char a1[512], a2[512], h1[1100], h2[1100];
    if (!make_abs(a1, sizeof(a1), from) || !make_abs(a2, sizeof(a2), to)) return NAND_RESULT_INVALID;
    host_path(h1, sizeof(h1), a1);
    host_path(h2, sizeof(h2), a2);
    return rename(h1, h2) == 0 ? NAND_RESULT_OK : errno_result();
}

// ---- asynchronous completion on a worker thread -------------------------------------------------

enum Kind { K_CREATE, K_CREATEDIR, K_DELETE, K_OPEN, K_CLOSE, K_READ, K_WRITE, K_SEEK, K_GETLEN, K_CHDIR, K_CHECK, K_STATUS, K_TYPE };

struct Job {
    Job* next;
    Kind kind;
    char path[256];
    u8 perm, attr, mode;
    NANDFileInfo* info;
    void* buf;
    u32 len;
    s32 off;
    NANDSeekMode whence;
    u32* out32;
    NANDStatus* status;
    u8* type;
    NANDCommandBlock* block;
    NANDAsyncCallback cb;
};

Job* g_head = 0;
Job* g_tail = 0;
NpSem g_sem;
NpMutex g_qlock;
int g_worker_started = 0;

s32 run(Job* j) {
    switch (j->kind) {
    case K_CREATE: return op_create(j->path, j->perm, j->attr);
    case K_CREATEDIR: return op_create_dir(j->path, j->perm, j->attr);
    case K_DELETE: return op_delete(j->path);
    case K_OPEN: return op_open(j->path, j->info, j->mode);
    case K_CLOSE: return op_close(j->info);
    case K_READ: return op_read(j->info, j->buf, j->len);
    case K_WRITE: return op_write(j->info, j->buf, j->len);
    case K_SEEK: return op_seek(j->info, j->off, j->whence);
    case K_GETLEN: return op_getlength(j->info, j->out32);
    case K_CHDIR: return op_changedir(j->path);
    case K_CHECK: return op_check(j->len, (u32)j->off, j->out32);
    case K_STATUS: return op_getstatus(j->path, j->status);
    case K_TYPE: return op_gettype(j->path, j->type);
    }
    return NAND_RESULT_INVALID;
}

void* worker(void*) {
    for (;;) {
        np_sem_wait(&g_sem);
        np_mutex_lock(&g_qlock);
        Job* j = g_head;
        if (j) {
            g_head = j->next;
            if (!g_head) g_tail = 0;
        }
        np_mutex_unlock(&g_qlock);
        if (!j) continue;
        s32 r = run(j);
        np_irq_enter();
        if (j->cb) j->cb(r, j->block);
        np_irq_leave();
        free(j);
    }
    return 0;
}

// ---- virtual time: completions as events -------------------------------------------------------------
// The operation itself runs when the request is made (requests are served in order, so the effects are the same); the
// completion (callback in interrupt context) falls due after a modelled latency on the virtual clock, one after the other like
// a single flash controller: NP_NAND_US per request (default 500) + 1 us per KiB of data.
struct Done {
    Done* next;
    uint64_t due;
    s32 result;
    NANDAsyncCallback cb;
    NANDCommandBlock* block;
};
Done* g_done_head = 0;
Done* g_done_tail = 0;
uint64_t g_flash_free_ns = 0;
int g_vic_id = -1;

void vnand_complete(void*, uint64_t) {
    np_irq_enter();
    const uint64_t now = np_vt_now();
    while (g_done_head && g_done_head->due <= now) {
        Done* d = g_done_head;
        g_done_head = d->next;
        if (!g_done_head) g_done_tail = 0;
        if (d->cb) d->cb(d->result, d->block);
        free(d);
    }
    np_vic_arm(g_vic_id, g_done_head ? g_done_head->due : 0);
    np_irq_leave();
}

s32 vnand_submit(Job* j, NANDAsyncCallback cb, NANDCommandBlock* block) {
    if (g_vic_id < 0) g_vic_id = np_vic_add("nand", vnand_complete, 0);
    if (block) block->callback = cb;
    s32 r = run(j);
    static double us = -1.0;
    if (us < 0.0) {
        const char* e = getenv("NP_NAND_US");
        us = (e && *e) ? atof(e) : 500.0;
    }
    const uint64_t now = np_vt_now();
    const uint64_t start = g_flash_free_ns > now ? g_flash_free_ns : now;
    g_flash_free_ns = start + (uint64_t)(us * 1000.0) + (uint64_t)((j->kind == K_READ || j->kind == K_WRITE) ? j->len : 0u) ;
    Done* d = (Done*)calloc(1, sizeof(Done));
    d->due = g_flash_free_ns;
    d->result = r;
    d->cb = cb;
    d->block = block;
    BOOL en = OSDisableInterrupts();
    if (g_done_tail) g_done_tail->next = d; else g_done_head = d;
    g_done_tail = d;
    np_vic_arm(g_vic_id, g_done_head->due);
    OSRestoreInterrupts(en);
    free(j);
    return NAND_RESULT_OK;
}

s32 submit(Job* j, NANDAsyncCallback cb, NANDCommandBlock* block) {
    if (!g_inited) { free(j); return NAND_RESULT_FATAL_ERROR; }
    if (np_vt_on) return vnand_submit(j, cb, block);
    if (!g_worker_started) {
        g_worker_started = 1;
        np_mutex_init(&g_qlock);
        np_sem_init(&g_sem, 0);
        NpThread* t = np_thread_create("nand-io", NP_ROLE_IO, worker, 0, 0, 262144);
        if (t) np_thread_detach(t);
    }
    j->cb = cb;
    j->block = block;
    if (block) block->callback = cb;
    j->next = 0;
    np_mutex_lock(&g_qlock);
    if (g_tail) g_tail->next = j; else g_head = j;
    g_tail = j;
    np_mutex_unlock(&g_qlock);
    np_sem_post(&g_sem);
    return NAND_RESULT_OK;
}

Job* newjob(Kind k, const char* path = 0) {
    Job* j = (Job*)calloc(1, sizeof(Job));
    j->kind = k;
    if (path) snprintf(j->path, sizeof(j->path), "%s", path);
    return j;
}

}  // namespace

extern "C" {

s32 NANDInit(void) {
    if (g_inited) return NAND_RESULT_OK;
    np_os_lazy_init();
    // "NAND" like the recompiled build (ux0:data/mscharged/NAND): on the console both builds share one save directory, and a
    // data directory of the recompiled build works as NP_DATA_DIR
    snprintf(g_root, sizeof(g_root), "%s/NAND", NpSdk_GetConfig()->data_dir);
    char p[1100];
    np_mkdir_p(g_root);
    snprintf(p, sizeof(p), "%s%s", g_root, kHome);
    np_mkdir_p(p);
    snprintf(p, sizeof(p), "%s/tmp", g_root);
    np_mkdir_p(p);
    // Real Wii IOS clears /tmp on launch so stale per-session scratch files (REPLAY_0..2) never outlive the process.
    // Never touch disk while MSC_BOOT_MATCH (nosave) is active.
    if (!nosave()) clear_tmp_dir(p);
    g_inited = 1;
    snprintf(g_cwd, sizeof(g_cwd), "/");
    np_log("nand", "root %s, home %s", g_root, kHome);
    return NAND_RESULT_OK;
}

s32 NANDGetHomeDir(char* out) {
    if (!g_inited) return NAND_RESULT_FATAL_ERROR;
    strcpy(out, kHome);
    return NAND_RESULT_OK;
}
s32 NANDGetCurrentDir(char* out) {
    if (!g_inited) return NAND_RESULT_FATAL_ERROR;
    strcpy(out, g_cwd);
    return NAND_RESULT_OK;
}
const char* nandGetHomeDir(void) { return kHome; }
BOOL nandIsInitialized(void) { return g_inited; }

s32 NANDCreate(const char* path, u8 perm, u8 attr) { if (!g_inited) return NAND_RESULT_FATAL_ERROR; return op_create(path, perm, attr); }
s32 NANDPrivateCreate(const char* path, u8 perm, u8 attr) { return NANDCreate(path, perm, attr); }
s32 NANDCreateAsync(const char* path, u8 perm, u8 attr, NANDAsyncCallback cb, NANDCommandBlock* block) {
    Job* j = newjob(K_CREATE, path); j->perm = perm; j->attr = attr; return submit(j, cb, block);
}
s32 NANDPrivateCreateAsync(const char* path, u8 perm, u8 attr, NANDAsyncCallback cb, NANDCommandBlock* block) { return NANDCreateAsync(path, perm, attr, cb, block); }
s32 NANDCreateDir(const char* path, u8 perm, u8 attr) { if (!g_inited) return NAND_RESULT_FATAL_ERROR; return op_create_dir(path, perm, attr); }
s32 NANDPrivateCreateDir(const char* path, u8 perm, u8 attr) { return NANDCreateDir(path, perm, attr); }
s32 NANDCreateDirAsync(const char* path, u8 perm, u8 attr, NANDAsyncCallback cb, NANDCommandBlock* block) {
    Job* j = newjob(K_CREATEDIR, path); j->perm = perm; j->attr = attr; return submit(j, cb, block);
}
s32 NANDPrivateCreateDirAsync(const char* path, u8 perm, u8 attr, NANDAsyncCallback cb, NANDCommandBlock* block) { return NANDCreateDirAsync(path, perm, attr, cb, block); }
s32 NANDDelete(const char* path) { if (!g_inited) return NAND_RESULT_FATAL_ERROR; return op_delete(path); }
s32 NANDPrivateDelete(const char* path) { return NANDDelete(path); }
s32 NANDDeleteAsync(const char* path, NANDAsyncCallback cb, NANDCommandBlock* block) { return submit(newjob(K_DELETE, path), cb, block); }
s32 NANDPrivateDeleteAsync(const char* path, NANDAsyncCallback cb, NANDCommandBlock* block) { return NANDDeleteAsync(path, cb, block); }

s32 NANDOpen(const char* path, NANDFileInfo* info, u8 mode) { if (!g_inited) return NAND_RESULT_FATAL_ERROR; return op_open(path, info, mode); }
s32 NANDPrivateOpen(const char* path, NANDFileInfo* info, u8 mode) { return NANDOpen(path, info, mode); }
s32 NANDOpenAsync(const char* path, NANDFileInfo* info, u8 mode, NANDAsyncCallback cb, NANDCommandBlock* block) {
    Job* j = newjob(K_OPEN, path); j->info = info; j->mode = mode; return submit(j, cb, block);
}
s32 NANDPrivateOpenAsync(const char* path, NANDFileInfo* info, u8 mode, NANDAsyncCallback cb, NANDCommandBlock* block) { return NANDOpenAsync(path, info, mode, cb, block); }
s32 NANDPrivateSafeOpenAsync(const char* path, NANDFileInfo* info, u8 access, void* buffer, u32 bufferSize, NANDAsyncCallback cb, NANDCommandBlock* block) {
    (void)buffer; (void)bufferSize;
    return NANDOpenAsync(path, info, access, cb, block);
}
s32 NANDClose(NANDFileInfo* info) { return op_close(info); }
s32 NANDCloseAsync(NANDFileInfo* info, NANDAsyncCallback cb, NANDCommandBlock* block) {
    Job* j = newjob(K_CLOSE); j->info = info; return submit(j, cb, block);
}
s32 NANDSafeCloseAsync(NANDFileInfo* info, NANDAsyncCallback cb, NANDCommandBlock* block) { return NANDCloseAsync(info, cb, block); }

s32 NANDRead(NANDFileInfo* info, void* buf, u32 len) { return op_read(info, buf, len); }
s32 NANDReadAsync(NANDFileInfo* info, void* buf, u32 len, NANDAsyncCallback cb, NANDCommandBlock* block) {
    Job* j = newjob(K_READ); j->info = info; j->buf = buf; j->len = len; return submit(j, cb, block);
}
s32 NANDWrite(NANDFileInfo* info, const void* buf, u32 len) { return op_write(info, buf, len); }
s32 NANDWriteAsync(NANDFileInfo* info, const void* buf, u32 len, NANDAsyncCallback cb, NANDCommandBlock* block) {
    Job* j = newjob(K_WRITE); j->info = info; j->buf = (void*)buf; j->len = len; return submit(j, cb, block);
}
s32 NANDSeek(NANDFileInfo* info, s32 offset, NANDSeekMode whence) { return op_seek(info, offset, whence); }
s32 NANDSeekAsync(NANDFileInfo* info, s32 offset, NANDSeekMode whence, NANDAsyncCallback cb, NANDCommandBlock* block) {
    Job* j = newjob(K_SEEK); j->info = info; j->off = offset; j->whence = whence; return submit(j, cb, block);
}
s32 NANDGetLength(NANDFileInfo* info, u32* length) { return op_getlength(info, length); }
s32 NANDGetLengthAsync(NANDFileInfo* info, u32* lengthOut, NANDAsyncCallback cb, NANDCommandBlock* block) {
    Job* j = newjob(K_GETLEN); j->info = info; j->out32 = lengthOut; return submit(j, cb, block);
}
s32 NANDChangeDir(const char* path) { if (!g_inited) return NAND_RESULT_FATAL_ERROR; return op_changedir(path); }
s32 NANDChangeDirAsync(const char* path, NANDAsyncCallback cb, NANDCommandBlock* block) { return submit(newjob(K_CHDIR, path), cb, block); }
s32 NANDCheck(u32 neededBlocks, u32 neededFiles, u32* answer) { return op_check(neededBlocks, neededFiles, answer); }
s32 NANDCheckAsync(u32 neededBlocks, u32 neededFiles, u32* answer, NANDAsyncCallback cb, NANDCommandBlock* block) {
    Job* j = newjob(K_CHECK); j->len = neededBlocks; j->off = (s32)neededFiles; j->out32 = answer; return submit(j, cb, block);
}
s32 NANDGetStatus(const char* path, NANDStatus* status) { return op_getstatus(path, status); }
s32 NANDPrivateGetStatus(const char* path, NANDStatus* status) { return op_getstatus(path, status); }
s32 NANDPrivateGetStatusAsync(const char* path, NANDStatus* status, NANDAsyncCallback cb, NANDCommandBlock* block) {
    Job* j = newjob(K_STATUS, path); j->status = status; return submit(j, cb, block);
}
s32 NANDGetType(const char* path, u8* type) { return op_gettype(path, type); }
s32 NANDPrivateGetTypeAsync(const char* path, u8* type, NANDAsyncCallback cb, NANDCommandBlock* block) {
    Job* j = newjob(K_TYPE, path); j->type = type; return submit(j, cb, block);
}
s32 NANDMove(const char* from, const char* to) { return op_move(from, to); }
void NANDSetUserData(NANDCommandBlock* block, void* data) { block->userData = data; }
void* NANDGetUserData(NANDCommandBlock* block) { return block->userData; }

void NANDInitBanner(NANDBanner* banner, u32 flags, const wchar_t* title, const wchar_t* subtitle) {
    memset(banner, 0, sizeof(NANDBanner));
    banner->flags = flags;
    banner->magic = 0x5749424E;   // 'WIBN'
    for (int i = 0; i < NAND_BANNER_TITLE_MAX - 1 && title[i]; ++i) banner->title[i] = title[i];
    for (int i = 0; i < NAND_BANNER_TITLE_MAX - 1 && subtitle[i]; ++i) banner->subtitle[i] = subtitle[i];
}

}  // extern "C"
