// MSC_BOOT_MATCH (the test-only fast boot) must never touch a save: with it on, nand_native.cpp drops every NAND create / delete / move / write.
// `test_nand_nosave on`: the switch is set; an existing save and an existing other file stay byte for byte and keep their times, nothing new appears,
// every call reports success. `test_nand_nosave off`: the contrast (the switch unset): the same calls change the files (so the test can fail).
// Built by run_nand_nosave_test.py (nand_native.cpp + stubs for the SDK's thread and clock layer).
#include <revolution/nand.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "np/np_sdk.h"
#include "np_internal.h"
static char g_dataDir[256];
static NpSdkConfig g_cfg;
extern "C" {
const NpSdkConfig* NpSdk_GetConfig(void) { return &g_cfg; }
BOOL OSDisableInterrupts(void) { return 0; }
BOOL OSRestoreInterrupts(BOOL) { return 0; }
int np_dir_exists(const char* p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
void np_irq_enter(void) {}
void np_irq_leave(void) {}
void np_log(const char*, const char*, ...) {}
int np_mkdir_p(const char* path) {
    char buf[1100];
    snprintf(buf, sizeof buf, "%s", path);
    for (char* c = buf + 1; *c; ++c) if (*c == '/') { *c = 0; mkdir(buf, 0777); *c = '/'; }
    mkdir(buf, 0777);
    return 0;
}
void np_os_lazy_init(void) {}
int np_vt_on = 0;
uint64_t np_vt_now(void) { return 0; }
int np_mutex_init(NpMutex*) { return 0; }
void np_mutex_lock(NpMutex*) {}
void np_mutex_unlock(NpMutex*) {}
int np_sem_init(NpSem*, int) { return 0; }
void np_sem_post(NpSem*) {}
void np_sem_wait(NpSem*) {}
NpThread* np_thread_create(const char*, NpRole, NpThreadFn, void*, void*, size_t) { return 0; }
void np_thread_detach(NpThread*) {}
int np_vic_add(const char*, NpVicRun, void*) { return 0; }
void np_vic_arm(int, uint64_t) {}
}

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL %s:%d: %s ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::string Slurp(const std::string& p, bool* ok) {
    FILE* f = fopen(p.c_str(), "rb");
    *ok = f != 0;
    std::string s;
    if (f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); }
    return s;
}

int main(int argc, char** argv) {
    const bool on = argc > 1 && !strcmp(argv[1], "on");
    if (on) setenv("MSC_BOOT_MATCH", "vice", 1); else unsetenv("MSC_BOOT_MATCH");
    snprintf(g_dataDir, sizeof g_dataDir, "/tmp/np_nosave_XXXXXX");
    if (!mkdtemp(g_dataDir)) return 2;
    g_cfg.data_dir = g_dataDir;
    const std::string root = std::string(g_dataDir) + "/NAND";
    const char kSave[] = "/title/00010000/52345145/data/Strikers2";
    CHECK(NANDInit() == NAND_RESULT_OK, "init");
    NANDCreateDir("/title/00010000", 0, 0);   // (a real directory tree the save sits in, made before the guard matters: off in the contrast run, dropped in the on run)
    const std::string dir = root + "/title/00010000/52345145/data";
    const std::string tmpDir = root + "/tmp";
    np_mkdir_p(dir.c_str());
    np_mkdir_p(tmpDir.c_str());
    const std::string save = root + kSave, other = dir + "/banner.bin", staleReplay = tmpDir + "/REPLAY_0";
    const std::string kSaveBytes(35616, 'S'), kOtherBytes(100, 'B'), kReplayBytes(64, 'R');
    { FILE* f = fopen(save.c_str(), "wb"); fwrite(kSaveBytes.data(), 1, kSaveBytes.size(), f); fclose(f); }
    { FILE* f = fopen(other.c_str(), "wb"); fwrite(kOtherBytes.data(), 1, kOtherBytes.size(), f); fclose(f); }
    { FILE* f = fopen(staleReplay.c_str(), "wb"); fwrite(kReplayBytes.data(), 1, kReplayBytes.size(), f); fclose(f); }
    struct stat st0; stat(save.c_str(), &st0);

    // the game's save sequence (SaveLoad.cpp): create, open for writing, write, close; then a delete, a move, a directory
    s32 r = NANDCreate("/title/00010000/52345145/data/Newfile", 0, 0);
    CHECK(on ? r == NAND_RESULT_OK : r == NAND_RESULT_OK, "create r=%d", r);
    NANDFileInfo info;
    memset(&info, 0xA5, sizeof info);   // (garbage in the handle: the layer must not depend on it)
    r = NANDOpen("/title/00010000/52345145/data/Newfile", &info, NAND_ACCESS_WRITE);
    if (on) {
        CHECK(r == NAND_RESULT_OK, "open for write r=%d", r);
        const char payload[] = "overwrite";
        CHECK(NANDWrite(&info, payload, sizeof payload) == (s32)sizeof payload, "write reports the length");
        CHECK(NANDClose(&info) == NAND_RESULT_OK, "close");
    } else if (r == NAND_RESULT_OK) {
        const char payload[] = "overwrite";
        NANDWrite(&info, payload, sizeof payload);
        NANDClose(&info);
    }
    NANDFileInfo sinfo;
    memset(&sinfo, 0xA5, sizeof sinfo);
    r = NANDOpen(kSave, &sinfo, NAND_ACCESS_WRITE);   // the save itself, for writing
    if (on) {
        CHECK(r == NAND_RESULT_OK, "save open for write r=%d", r);
        const std::string junk(35616, 'J');
        CHECK(NANDWrite(&sinfo, junk.data(), (u32)junk.size()) == (s32)junk.size(), "save write reports success");
        CHECK(NANDClose(&sinfo) == NAND_RESULT_OK, "save close");
    } else if (r == NAND_RESULT_OK) {
        const std::string junk(35616, 'J');
        NANDWrite(&sinfo, junk.data(), (u32)junk.size());
        NANDClose(&sinfo);
    }
    r = NANDDelete("/title/00010000/52345145/data/banner.bin");
    if (on) CHECK(r == NAND_RESULT_OK, "delete reports success r=%d", r);
    r = NANDMove("/title/00010000/52345145/data/Strikers2", "/title/00010000/52345145/data/Moved");
    if (on) CHECK(r == NAND_RESULT_OK, "move reports success r=%d", r);
    r = NANDCreateDir("/title/00010000/52345145/newdir", 0, 0);
    if (on) CHECK(r == NAND_RESULT_OK, "create dir reports success r=%d", r);
    // a read of the save still reads it (the guard drops writes only)
    NANDFileInfo rinfo;
    r = NANDOpen(kSave, &rinfo, NAND_ACCESS_READ);
    if (on) {
        CHECK(r == NAND_RESULT_OK, "read open r=%d", r);
        char buf[16] = {0};
        CHECK(NANDRead(&rinfo, buf, 16) == 16 && buf[0] == 'S', "the save reads back");
        NANDClose(&rinfo);
        // stale /tmp/REPLAY_0 must NOT be readable under nosave (writes to /tmp were dropped) and must stay untouched on disk
        NANDFileInfo tinfo;
        bool tok = false;
        CHECK(NANDOpen("/tmp/REPLAY_0", &tinfo, NAND_ACCESS_READ) == NAND_RESULT_NOEXISTS, "stale /tmp replay hidden under nosave");
        CHECK(Slurp(staleReplay, &tok) == kReplayBytes && tok, "stale /tmp replay untouched on disk under nosave");
    }

    bool ok;
    const std::string sNow = Slurp(save, &ok);
    struct stat st1; const bool statOk = stat(save.c_str(), &st1) == 0;
    struct stat tmp;
    if (on) {
        CHECK(ok && sNow == kSaveBytes, "the save is byte for byte what it was");
        CHECK(statOk && st1.st_mtime == st0.st_mtime && st1.st_size == st0.st_size, "the save's size and time are unchanged");
        CHECK(Slurp(other, &ok) == kOtherBytes && ok, "the other file is untouched (delete dropped)");
        CHECK(stat((dir + "/Newfile").c_str(), &tmp) != 0, "no new file appeared");
        CHECK(stat((dir + "/Moved").c_str(), &tmp) != 0, "no moved file appeared");
        CHECK(stat((root + "/title/00010000/52345145/newdir").c_str(), &tmp) != 0, "no new directory appeared");
        CHECK(stat((save + ".bak").c_str(), &tmp) != 0 && stat((save + ".tmp").c_str(), &tmp) != 0, "no .bak / .tmp of the save");
    } else {
        // the contrast: the switch off, the same calls DO change the world (this is what the 'on' run's checks must be able to see)
        const bool changed = !ok || sNow != kSaveBytes || stat((dir + "/Newfile").c_str(), &tmp) == 0 || stat((dir + "/Moved").c_str(), &tmp) == 0 ||
                             Slurp(other, &ok) != kOtherBytes || !ok;
        CHECK(changed, "with the switch off the calls changed nothing: the test could not fail");
    }
    printf("%s: %d checks, %d failed\n", on ? "nosave on" : "nosave off (contrast)", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
