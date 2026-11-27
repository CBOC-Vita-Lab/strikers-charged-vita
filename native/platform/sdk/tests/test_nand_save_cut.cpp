// Cut test of the game's save through the real nand_native.cpp (NANDCreate / NANDOpen / NANDWrite / NANDClose / NANDDelete, the calls SaveLoad.cpp makes):
// a child is killed before every file-changing system call (and, in the power model, loses every byte it had not fsync'ed), the parent restarts the NAND layer,
// opens the save for reading as the game does at start-up, and the live file must be exactly the old save or exactly the new one (first save: absent or
// the new one). The same with the old in-place write for contrast is in test_safe_file.cpp. Built by run_nand_save_cut_test.py (nand_native.cpp + stubs for the
// SDK's thread and clock layer; wrappers: -Wl,--wrap=open,write,fsync,rename,unlink,close).
#include <revolution/nand.h>

#include <stdint.h>
#include <sys/wait.h>

#include <string>

#include "platform/sdk/save/np_safe_file.h"
#include "platform/sdk/save/np_save_import.h"
#include "cut_wrap.h"

// ---- stubs of what nand_native.cpp takes from the SDK layer ------------------------------------------------------------------------------------------------
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
// (the worker thread and the virtual clock are only reached by the asynchronous calls, which this test does not make)
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

namespace {

int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL %s:%d: %s ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

const char kSave[] = "/title/00010000/52345145/data/Strikers2";

std::string MakeSave(unsigned char fill, size_t total = 35616) {
    std::string s(total, char(fill));
    for (size_t i = 8; i < s.size(); ++i) s[i] = char(fill + i * 7);
    auto be = [&](size_t at, uint32_t v) { s[at] = char(v >> 24); s[at + 1] = char(v >> 16); s[at + 2] = char(v >> 8); s[at + 3] = char(v); };
    be(0, uint32_t(total));
    be(4, NpSafeFile::Crc32(reinterpret_cast<const unsigned char*>(s.data()) + 8, total - 8));
    return s;
}
const std::string kOldSave = MakeSave(0x11), kNewSave = MakeSave(0x22), kOlderSave = MakeSave(0x33);

std::string HostPath() { return std::string(g_dataDir) + "/NAND" + kSave; }
void PutFile(const std::string& p, const std::string& d) {
    int fd = __real_open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { if (__real_write(fd, d.data(), d.size()) < 0) {} __real_close(fd); }
}
bool GetFile(const std::string& p, std::string* d) {
    int fd = __real_open(p.c_str(), O_RDONLY);
    if (fd < 0) return false;
    d->clear();
    char buf[4096];
    for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;) d->append(buf, size_t(n));
    __real_close(fd);
    return true;
}
bool Has(const std::string& p) { return access(p.c_str(), F_OK) == 0; }
void Wipe() { for (const char* s : {"", ".tmp", ".bak", ".bad", ".bad2", ".bad3", ".bad4"}) unlink((HostPath() + s).c_str()); }

// what SaveLoad.cpp does to write: create when the file is not there (-6 = exists is fine), open for writing, write the whole file, close
void GameWrite(const std::string& data) {
    s32 r = NANDCreate(kSave, NAND_PERM_RWALL, 0);
    (void)r;
    NANDFileInfo fi;
    if (NANDOpen(kSave, &fi, NAND_ACCESS_WRITE) != NAND_RESULT_OK) return;
    NANDWrite(&fi, data.data(), (u32)data.size());
    NANDClose(&fi);
}
// what a start-up does to read it: open for reading, read, check
bool GameRead(std::string* out) {
    NANDFileInfo fi;
    if (NANDOpen(kSave, &fi, NAND_ACCESS_READ) != NAND_RESULT_OK) return false;
    u32 len = 0;
    NANDGetLength(&fi, &len);
    out->assign(len, '\0');
    s32 n = len ? NANDRead(&fi, &(*out)[0], len) : 0;
    NANDClose(&fi);
    return n == (s32)len;
}

struct Case { const char* name; std::string before; bool hasBefore; std::string bak; bool hasBak; void (*op)(); std::string oldBytes, newBytes; bool absentOk; };
void OpWrite() { GameWrite(kNewSave); }
void OpDelete() { NANDDelete(kSave); }

void Run(const Case& c, bool power) {
    int cases = 0, bad = 0;
    std::string firstBad;
    for (int k = 0; k < 200; ++k) {
        Wipe();
        if (c.hasBefore) PutFile(HostPath(), c.before);
        if (c.hasBak) PutFile(HostPath() + ".bak", c.bak);
        const pid_t pid = fork();
        if (pid == 0) {
            g_cutAt = k; g_calls = 0; g_power = power;
            c.op();
            Die(78);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        // the restart: the game reads its save
        std::string viaGame, live;
        const bool opened = GameRead(&viaGame);
        const bool present = GetFile(HostPath(), &live);
        const bool isOld = c.oldBytes.empty() ? !present : (present && live == c.oldBytes);
        const bool isNew = present && live == c.newBytes;
        const bool absentOk = c.absentOk && !present;
        ++cases;
        // what the game reads must be what is live, and sound (or "no file")
        const bool gameOk = present ? (opened && viaGame == live && NpSafeFile::GameSaveSound(viaGame)) : !opened;
        if (!(isOld || isNew || absentOk) || !gameOk) {
            ++bad;
            if (firstBad.empty()) { char b[160]; snprintf(b, sizeof b, "cut before call %d: live %s %zu bytes, game read %s", k, present ? "present" : "missing", live.size(), opened ? "a file" : "nothing"); firstBad = b; }
        }
        if (code == 78) break;
        if (code != 77) { CHECK(false, "child ended oddly %d", code); break; }
    }
    printf("  %-46s %-5s %3d cuts, %d bad%s%s\n", c.name, power ? "power" : "kill", cases, bad, bad ? ": " : "", firstBad.c_str());
    CHECK(bad == 0, "%s", c.name);
    CHECK(cases >= 3, "matrix too small");
    Wipe();
}

}  // namespace

int main() {
    snprintf(g_dataDir, sizeof g_dataDir, "/tmp/np_nand_cut_XXXXXX");
    if (!mkdtemp(g_dataDir)) { printf("mkdtemp failed\n"); return 2; }
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.data_dir = g_dataDir;
    CHECK(NANDInit() == NAND_RESULT_OK, "NANDInit");

    const Case cases[] = {
        {"first save (no file, as the game creates it)", "", false, "", false, OpWrite, "", kNewSave, false},
        {"second save over the first",                    kOldSave, true, "", false, OpWrite, kOldSave, kNewSave, false},
        {"save with a previous backup present",           kOldSave, true, kOlderSave, true, OpWrite, kOldSave, kNewSave, false},
        {"delete of a damaged save",                      std::string(kNewSave.size(), '\0'), true, "", false, OpDelete, "", "", true},
    };
    for (const Case& c : cases) for (bool power : {false, true}) Run(c, power);

    // the faults of the task, one by one, through the NAND calls
    {   // 1. torn file, no old file, no backup: not loaded, not overwritten, kept as .bad
        Wipe();
        PutFile(HostPath(), std::string(kNewSave.size(), '\0'));
        std::string got;
        CHECK(!GameRead(&got), "a zero-tailed save with nothing to restore must not be offered to the game");
        CHECK(!Has(HostPath()) && Has(HostPath() + ".bad"), "set aside as .bad");
        GameWrite(kNewSave);
        std::string live, bad;
        GetFile(HostPath(), &live); GetFile(HostPath() + ".bad", &bad);
        CHECK(live == kNewSave && bad == std::string(kNewSave.size(), '\0'), "the next save does not touch the set-aside file");
    }
    {   // 1b. torn file and a good backup: the backup is loaded
        Wipe();
        PutFile(HostPath(), std::string(kNewSave.size(), '\0'));
        PutFile(HostPath() + ".bak", kOldSave);
        std::string got;
        CHECK(GameRead(&got) && got == kOldSave, "the good backup is restored");
        CHECK(Has(HostPath() + ".bad"), "and the torn file kept");
    }
    {   // 2. a deleted save does not come back
        Wipe();
        GameWrite(kOldSave);
        GameWrite(kNewSave);   // now P and P.bak
        CHECK(Has(HostPath() + ".bak"), "backup kept after the second save");
        CHECK(NANDDelete(kSave) == NAND_RESULT_OK, "delete");
        std::string got;
        CHECK(!GameRead(&got) && !Has(HostPath()) && !Has(HostPath() + ".bak"), "deleted save stays deleted at the next start");
        CHECK(Has(HostPath() + ".bad"), "the deleted save is set aside, not destroyed");
    }
    {   // 2b. delete flow: the old backup does not come back, .bad copies stay (at most three), and the once-only import marker keeps the old recompiled save out
        Wipe();
        const std::string src = std::string(g_dataDir) + "-recompiled";   // the recompiled build's data directory, with its older save
        const std::string srcSave = src + "/NAND/title/00010000/52345145/data/Strikers2";
        np_mkdir_p((src + "/NAND/title/00010000/52345145/data").c_str());
        PutFile(srcSave, kOlderSave);
        unlink((std::string(g_dataDir) + "/np-save-import.done").c_str());
        char msg[400];
        // first start: no native save: the older recompiled save is imported, and the marker is written
        CHECK(NpSaveImportRecompiled(src.c_str(), g_dataDir, msg, sizeof msg) == NP_SAVE_IMPORT_DONE, "import %s", msg);
        std::string got;
        CHECK(GameRead(&got) && got == kOlderSave, "the imported save is what the game loads");
        // the player plays: two saves (P and P.bak)
        GameWrite(kOldSave);
        GameWrite(kNewSave);
        CHECK(Has(HostPath() + ".bak"), "backup after the second save");
        // the game finds the save damaged and the player deletes it (the popup's delete): the same NANDDelete call
        CHECK(NANDDelete(kSave) == NAND_RESULT_OK, "delete");
        CHECK(!Has(HostPath()) && !Has(HostPath() + ".bak") && !Has(HostPath() + ".tmp"), "delete removed the live file's .bak and .tmp");
        CHECK(Has(HostPath() + ".bad"), "the set-aside copy stays");
        // restart: the NAND layer is started again, the import runs again, the game reads
        // (the NAND layer keeps no save state in memory: every open recovers from the files, so a restart is the next call)
        CHECK(NpSaveImportRecompiled(src.c_str(), g_dataDir, msg, sizeof msg) == NP_SAVE_IMPORT_NOT_NEEDED, "import is refused after the delete: %s", msg);
        CHECK(!GameRead(&got) && !Has(HostPath()) && !Has(HostPath() + ".bak"), "state after the restart: no save");
        CHECK(Has(std::string(g_dataDir) + "/np-save-import.done"), "marker stays");
        // a second restart, and the old recompiled save is still not taken
        NpSaveImportRecompiled(src.c_str(), g_dataDir, msg, sizeof msg);
        CHECK(!GameRead(&got), "still no save at the next restart");
        // delete when only the backup is left (a damaged live file was set aside when it was opened): the backup goes too, no save at the next start
        PutFile(HostPath() + ".bak", kOldSave);
        PutFile(HostPath(), std::string(kNewSave.size(), '\0'));
        CHECK(!GameRead(&got) || got == kOldSave, "reading a damaged save with a good backup gives the backup or nothing");
        Wipe();
        PutFile(HostPath() + ".bak", std::string(kNewSave.size(), '\0'));   // a damaged backup nobody can use
        CHECK(NANDDelete(kSave) == NAND_RESULT_OK && !Has(HostPath() + ".bak"), "delete with only a leftover backup clears it");
        CHECK(NANDDelete(kSave) == NAND_RESULT_NOEXISTS, "nothing left to delete");
        // the cap: deletes and set-asides never keep more than three .bad copies
        Wipe();
        for (int i = 0; i < 6; ++i) { GameWrite(MakeSave(unsigned(0x40 + i))); CHECK(NANDDelete(kSave) == NAND_RESULT_OK, "delete %d", i); }
        int n = 0;
        for (const char* x : {".bad", ".bad2", ".bad3", ".bad4", ".bad5", ".bad6"}) n += Has(HostPath() + x);
        CHECK(n == 3, "three set-aside copies after six deletes, found %d", n);
        std::string newest;
        GetFile(HostPath() + ".bad", &newest);
        CHECK(newest == MakeSave(0x45), "newest first");
        Wipe();
        unlink((std::string(g_dataDir) + "/np-save-import.done").c_str());
        unlink(srcSave.c_str());
    }
    {   // galcheck 13 Nov (Galaxy's two save faults, checked against this layer)
        // A. first-ever save on a completely empty data folder (no NAND tree yet): NANDInit makes it, the game's create/open/write/close sequence must end with
        //    the live file exactly the new save, and reading it back as the game does gives it.
        Wipe();
        std::string cmd = std::string("rm -rf '") + g_dataDir + "/NAND'";
        if (system(cmd.c_str()) != 0) {}
        // (g_inited stays set, so recreate the tree the way NANDInit does)
        np_mkdir_p((std::string(g_dataDir) + "/NAND/title/00010000/52345145/data").c_str());
        GameWrite(kNewSave);
        std::string live, got;
        CHECK(GetFile(HostPath(), &live) && live == kNewSave, "A: first save on an empty folder: live file is the new save");
        CHECK(GameRead(&got) && got == kNewSave, "A: and the game reads it back");
        std::string bak;
        const bool hasBak = GetFile(HostPath() + ".bak", &bak);
        printf("  A: after the first save .bak is %s\n", hasBak ? (bak.empty() ? "an EMPTY file (the empty file the game created)" : "present") : "absent");
        // B. an empty recovery copy next to a good save must never replace it, whatever happens to the live file afterwards
        Wipe();
        PutFile(HostPath(), kNewSave);
        PutFile(HostPath() + ".bak", "");
        PutFile(HostPath() + ".tmp", "");
        CHECK(GameRead(&got) && got == kNewSave, "B1: good save + empty .bak + empty .tmp: the good save is read");
        GameWrite(kOldSave);
        CHECK(GetFile(HostPath(), &live) && live == kOldSave, "B2: a save over it: live is the new content");
        CHECK(GetFile(HostPath() + ".bak", &bak) && bak == kNewSave, "B2: the .bak is the previous good save, not the empty one");
        // B3. the live file torn, the .bak empty: nothing sound to restore, so no file (never an empty or short save handed to the game)
        Wipe();
        PutFile(HostPath(), std::string(kNewSave.size(), '\0'));
        PutFile(HostPath() + ".bak", "");
        CHECK(!GameRead(&got) && !Has(HostPath()), "B3: torn live + empty .bak: no file offered");
        // B4. live missing, empty .bak, empty .tmp: no file
        Wipe();
        PutFile(HostPath() + ".bak", "");
        PutFile(HostPath() + ".tmp", "");
        CHECK(!GameRead(&got) && !Has(HostPath()), "B4: only empty copies: no file offered");
        // B5. short files (8 bytes of header only, 15 bytes, and a good header cut in half) are never loaded and never replace a good backup
        for (size_t cut : {size_t(8), size_t(15), kNewSave.size() / 2, kNewSave.size() - 1}) {
            Wipe();
            PutFile(HostPath(), kNewSave.substr(0, cut));
            PutFile(HostPath() + ".bak", kOldSave);
            CHECK(GameRead(&got) && got == kOldSave, "B5: short live file (%zu bytes) with a good backup: the backup is loaded", cut);
            Wipe();
            PutFile(HostPath(), kOldSave);
            PutFile(HostPath() + ".bak", kNewSave.substr(0, cut));
            CHECK(GameRead(&got) && got == kOldSave, "B5: good live file with a short .bak (%zu bytes): the live file is kept", cut);
            Wipe();
            PutFile(HostPath(), kNewSave.substr(0, cut));
            CHECK(!GameRead(&got) && !Has(HostPath()), "B5: short live file, nothing to restore (%zu bytes): no file offered", cut);
        }
        // B6. a short write by the game (the game wrote less than a whole file, then closed) over a good save: the next open restores the good one
        Wipe();
        GameWrite(kOldSave);
        {
            NANDCreate(kSave, NAND_PERM_RWALL, 0);
            NANDFileInfo fi;
            CHECK(NANDOpen(kSave, &fi, NAND_ACCESS_WRITE) == NAND_RESULT_OK, "B6 open");
            NANDWrite(&fi, kNewSave.data(), 100);   // (the copy started as the old save, so the file is the old one with 100 bytes replaced)
            NANDClose(&fi);
        }
        CHECK(GameRead(&got) && NpSafeFile::GameSaveSound(got), "B6: a half-replaced save is never handed to the game unsound");
        Wipe();
    }
    {   // an ordinary file of the directory is still written in place
        Wipe();
        const char* other = "/title/00010000/52345145/data/banner.bin";
        CHECK(NANDCreate(other, NAND_PERM_RWALL, 0) == NAND_RESULT_OK, "create banner");
        NANDFileInfo fi;
        CHECK(NANDOpen(other, &fi, NAND_ACCESS_WRITE) == NAND_RESULT_OK, "open banner");
        CHECK(fi.stage == 0, "no copy for banner.bin");
        CHECK(NANDWrite(&fi, "abc", 3) == 3 && NANDClose(&fi) == NAND_RESULT_OK, "write banner");
        std::string b;
        GetFile(std::string(g_dataDir) + "/NAND" + other, &b);
        CHECK(b == "abc", "banner written");
        CHECK(NANDDelete(other) == NAND_RESULT_OK, "delete banner");
    }

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
