// Cut test of platform/sdk/save/np_safe_file.h (and of the two writers it replaces).
//
// For every scenario a child process runs the operation and is killed BEFORE the k-th file-changing call, for k = 0, 1, 2 ... until the operation has run to
// the end (then once more: a cut right after it returned). The parent restarts (runs Recover, as a start-up does) and checks the live file is EXACTLY the old
// bytes or EXACTLY the new bytes (for a first write: absent or exactly the new bytes), never torn, never missing, never an older copy that was deleted.
//
// Two models of the cut:
//   kill:   the process dies; everything it wrote is in the file system.
//   power:  as kill, and every byte written but not fsync'ed is replaced by zeros (the length stays: a rename that survived without its data).
// The calls counted are open (for writing), write, fsync, rename, unlink. rename follows newlib's on the Vita: if the target exists it is removed first, as a
// second counted step (vitasdk libc.a _rename_r: sceIoRemove(new) then sceIoRename(old, new)): replacing a file by rename is not atomic there.
//
// The old writers (a bare temporary + rename, and the settings screen's backup dance without fsync) are run through the same matrix and MUST fail it: that
// is what shows the matrix can see the faults.
//
// Built by run_safe_file_tests.py with -Wl,--wrap=open,write,fsync,rename,unlink,close.
#include "platform/sdk/save/np_safe_file.h"

#include <stdarg.h>
#include <stdint.h>
#include <sys/wait.h>

#include <algorithm>
#include <string>
#include <vector>

#include "cut_wrap.h"

namespace {

int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL %s:%d: %s ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the files --------------------------------------------------------------------------------------------------------------------------------------
std::string Text(char c, size_t n) { return std::string(n, c) + "\n"; }
const std::string kOld = Text('o', 300), kNew = Text('n', 420);

// the settings file's rule (np_vita_settings.cpp SettingsTextSound): a run of 8 zero bytes is a torn file; an empty file is not told apart from defaults
bool TextValid(const std::string& d) {
    size_t run = 0;
    for (char c : d) { run = c == 0 ? run + 1 : 0; if (run >= 8) return false; }
    return true;
}

uint32_t Crc32(const unsigned char* p, size_t n) {
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; ++i) { crc ^= p[i]; for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); }
    return ~crc;
}
// a save in the game's format: 8 byte header (Size, CRC-32 of the rest, big endian), payload
std::string MakeSave(unsigned char fill, size_t total = 35616) {
    std::string s(total, char(fill));
    for (size_t i = 8; i < s.size(); ++i) s[i] = char(fill + i * 7);
    auto be = [&](size_t at, uint32_t v) { s[at] = char(v >> 24); s[at + 1] = char(v >> 16); s[at + 2] = char(v >> 8); s[at + 3] = char(v); };
    be(0, uint32_t(total));
    be(4, Crc32(reinterpret_cast<const unsigned char*>(s.data()) + 8, total - 8));
    return s;
}
bool SaveValid(const std::string& d) {
    if (d.size() < 16) return false;
    const unsigned char* u = reinterpret_cast<const unsigned char*>(d.data());
    auto be = [&](size_t at) { return uint32_t(u[at]) << 24 | uint32_t(u[at + 1]) << 16 | uint32_t(u[at + 2]) << 8 | u[at + 3]; };
    return be(0) >= 8 && be(0) <= d.size() && be(4) == Crc32(u + 8, d.size() - 8);
}
const std::string kOldSave = MakeSave(0x11), kNewSave = MakeSave(0x22), kOlderSave = MakeSave(0x33);

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

// ---- the writers --------------------------------------------------------------------------------------------------------------------------------------
NpSafeFile::Options TextOpt() { NpSafeFile::Options o; o.valid = TextValid; o.mode = 0644; return o; }
NpSafeFile::Options SaveOpt() { NpSafeFile::Options o; o.valid = SaveValid; o.keepBackup = true; o.mode = 0644; return o; }

// the old settings writer (np_vita_settings.cpp WriteFile before this change) and its recovery (SavedFilePath), no fsync
bool LegacyWriteFile(const std::string& p, const std::string& text) {
    const std::string t = p + ".tmp", b = p + ".bak";
    if (Has(b)) { if (!Has(p)) { if (rename(b.c_str(), p.c_str()) < 0) return false; } else if (unlink(b.c_str()) < 0) return false; }
    int fd = open(t.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size());
    if (close(fd) < 0) ok = false;
    if (ok) {
        const bool had = Has(p);
        if (had && rename(p.c_str(), b.c_str()) < 0) ok = false;
        if (ok) { ok = rename(t.c_str(), p.c_str()) >= 0; if (!ok && had) rename(b.c_str(), p.c_str()); if (ok && had) unlink(b.c_str()); }
    }
    if (!ok) unlink(t.c_str());
    return ok;
}
void LegacyRecoverText(const std::string& p) { if (!Has(p) && Has(p + ".bak")) rename((p + ".bak").c_str(), p.c_str()); }

// the old writer of the online account files (np_online_store.cpp): temporary file, rename over the live one, no fsync
bool LegacyBare(const std::string& p, const std::string& d) {
    const std::string t = p + ".tmp";
    int fd = open(t.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = write(fd, d.data(), d.size()) == ssize_t(d.size());
    ok = close(fd) == 0 && ok;
    if (ok) ok = rename(t.c_str(), p.c_str()) == 0;
    if (!ok) unlink(t.c_str());
    return ok;
}

// ---- the matrix ---------------------------------------------------------------------------------------------------------------------------------------
struct Scenario {
    const char* name;
    void (*setup)(const std::string& dir);      // the state before (made without cuts)
    void (*op)(const std::string& dir);         // the operation the child runs
    void (*recover)(const std::string& dir);    // what a restart does
    std::string oldBytes;                       // "" = absent
    std::string newBytes;
    std::string file;
    bool allowAbsentAfter = false;              // the operation is a delete: absent is a legal end
};

struct Result { int cases = 0; int bad = 0; std::string firstBad; };

Result RunMatrix(const Scenario& s, bool power) {
    Result r;
    char tmpl[] = "/tmp/np_safe_file_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp");
    if (!dir) return r;
    const std::string d = dir, live = d + "/" + s.file;
    for (int k = 0; k < 200; ++k) {
        for (const char* suffix : {"", ".tmp", ".bak", ".bad", ".bad2", ".bad3"}) unlink((live + suffix).c_str());
        s.setup(d);
        const pid_t pid = fork();
        if (pid == 0) {
            g_cutAt = k; g_calls = 0; g_power = power;
            s.op(d);
            g_cutAt = -1;
            Die(78);   // ran to the end; with the power model the power goes right after the operation returned

        }
        int st = 0;
        waitpid(pid, &st, 0);
        const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        s.recover(d);
        std::string got;
        const bool present = GetFile(live, &got);
        const bool isOld = s.oldBytes.empty() ? !present : (present && got == s.oldBytes);
        const bool isNew = present && got == s.newBytes;
        const bool okAbsent = s.allowAbsentAfter && !present;
        ++r.cases;
        if (!(isOld || isNew || okAbsent)) {
            ++r.bad;
            if (r.firstBad.empty()) {
                char b[200];
                snprintf(b, sizeof b, "cut before call %d (%s): live file %s, %zu bytes", k, power ? "power" : "kill", present ? "present" : "missing", got.size());
                r.firstBad = b;
            }
        }
        if (code == 78) break;   // ran to the end (and was cut right after): the matrix for this scenario is complete
        if (code != 77) { CHECK(false, "child ended oddly %d", code); break; }
    }
    // clean
    for (const char* suffix : {"", ".tmp", ".bak", ".bad", ".bad2", ".bad3"}) unlink((live + suffix).c_str());
    rmdir(dir);
    return r;
}

std::string F(const std::string& dir) { return dir + "/file"; }

// settings-like text file
void SetupOldText(const std::string& d) { PutFile(F(d), kOld); }
void SetupNone(const std::string&) {}
void OpReplaceText(const std::string& d) { NpSafeFile::Replace(F(d).c_str(), kNew, TextOpt()); }
void RecoverText(const std::string& d) { NpSafeFile::Recover(F(d).c_str(), TextOpt()); }
void OpLegacyText(const std::string& d) { LegacyWriteFile(F(d), kNew); }
void RecoverLegacyText(const std::string& d) { LegacyRecoverText(F(d)); }
// the account-file style, with its CRC-like validity (a zero tail is invalid): TextValid stands in
void OpLegacyBare(const std::string& d) { LegacyBare(F(d), kNew); }
void RecoverLegacyBare(const std::string& d) {   // the old loader: the file if it is sound, else "no file" (the service issues a new account)
    std::string got;
    if (GetFile(F(d), &got) && !TextValid(got)) { /* ignored by the loader: nothing is restored */ }
}

// the game's save through the piecewise writer, NAND style
void SetupOldSave(const std::string& d) { PutFile(F(d), kOldSave); }
void SetupEmptyCreated(const std::string& d) { PutFile(F(d), ""); }   // nlFlashCreate made the empty file, the write follows
void OpWriteSave(const std::string& d) {
    NpSafeFile::Writer w;
    if (!NpSafeFile::BeginWrite(F(d).c_str(), &w, SaveOpt())) return;
    if (write(w.fd, kNewSave.data(), kNewSave.size()) != ssize_t(kNewSave.size())) { NpSafeFile::Abort(&w); return; }
    NpSafeFile::Commit(&w, SaveOpt());
}
void RecoverSave(const std::string& d) { NpSafeFile::Recover(F(d).c_str(), SaveOpt()); }
void OpLegacySaveInPlace(const std::string& d) {   // nand_native.cpp op_open(O_WRONLY) + op_write + op_close before this change
    int fd = open(F(d).c_str(), O_WRONLY);
    if (fd < 0) return;
    if (write(fd, kNewSave.data(), kNewSave.size()) < 0) {}
    close(fd);
}
void RecoverNone(const std::string&) {}

// recovery itself cut at every call: the crashed states
void SetupMidDanceTmpAndBak(const std::string& d) { PutFile(F(d) + ".bak", kOldSave); PutFile(F(d) + ".tmp", kNewSave); }   // cut after P -> P.bak
void SetupMidDanceBakOnly(const std::string& d) { PutFile(F(d) + ".bak", kOldSave); }
void SetupTornAndBak(const std::string& d) { PutFile(F(d), std::string(kNewSave.size(), '\0')); PutFile(F(d) + ".bak", kOldSave); }
void SetupTornOnly(const std::string& d) { PutFile(F(d), std::string(kNewSave.size(), '\0')); }
void OpRecoverSave(const std::string& d) { NpSafeFile::Recover(F(d).c_str(), SaveOpt()); }
// a deliberate delete
void SetupOldWithBak(const std::string& d) { PutFile(F(d), kOldSave); PutFile(F(d) + ".bak", kOlderSave); }   // the live save and the version before it
void OpRemove(const std::string& d) { NpSafeFile::Remove(F(d).c_str()); }
void OpDiscard(const std::string& d) { NpSafeFile::Discard(F(d).c_str()); }

}  // namespace

int main() {
    const Scenario newOnes[] = {
        {"Replace over an old text file",           SetupOldText, OpReplaceText, RecoverText, kOld, kNew, "file"},
        {"Replace, no old file",                    SetupNone, OpReplaceText, RecoverText, "", kNew, "file"},
        {"save: piecewise write over an old save",  SetupOldSave, OpWriteSave, RecoverSave, kOldSave, kNewSave, "file"},
        {"save: write after nlFlashCreate (empty)", SetupEmptyCreated, OpWriteSave, RecoverSave, "", kNewSave, "file"},
        {"save: first write, no file",              SetupNone, OpWriteSave, RecoverSave, "", kNewSave, "file"},
        {"Recover: P.tmp and P.bak, no P",          SetupMidDanceTmpAndBak, OpRecoverSave, RecoverSave, kOldSave, kNewSave, "file"},
        {"Recover: P.bak only",                     SetupMidDanceBakOnly, OpRecoverSave, RecoverSave, kOldSave, kOldSave, "file"},
        {"Recover: torn P with a good P.bak",       SetupTornAndBak, OpRecoverSave, RecoverSave, kOldSave, kOldSave, "file"},
    };
    for (const Scenario& s : newOnes)
        for (bool power : {false, true}) {
            Result r = RunMatrix(s, power);
            printf("  %-44s %-5s %3d cuts, %d bad%s%s\n", s.name, power ? "power" : "kill", r.cases, r.bad, r.bad ? ": " : "", r.firstBad.c_str());
            CHECK(r.bad == 0, "%s", s.name);
            CHECK(r.cases >= 2, "matrix too small");
        }
    // the library's own save check agrees with the independent one above
    {
        CHECK(NpSafeFile::GameSaveSound(kOldSave) && SaveValid(kOldSave), "sound save");
        std::string flipped = kOldSave; flipped[9000] ^= 0x10;
        CHECK(!NpSafeFile::GameSaveSound(flipped) && !SaveValid(flipped), "flipped bit");
        CHECK(!NpSafeFile::GameSaveSound(std::string(kOldSave.size(), '\0')) && !NpSafeFile::GameSaveSound("") && !NpSafeFile::GameSaveSound(kOldSave.substr(0, 20000)), "zero tail, empty, short");
        CHECK(NpSafeFile::Crc32(reinterpret_cast<const unsigned char*>("123456789"), 9) == 0xCBF43926u, "CRC-32 check value");
    }
    // a torn P with nothing to restore: the live name must not hold the torn bytes (absent or the set-aside file), and the torn bytes must be kept
    {
        char tmpl[] = "/tmp/np_safe_file_XXXXXX";
        const std::string d = mkdtemp(tmpl);
        SetupTornOnly(d);
        NpSafeFile::Found f = NpSafeFile::Recover(F(d).c_str(), SaveOpt());
        CHECK(f == NpSafeFile::Found::SetAside, "found %s", NpSafeFile::FoundName(f));
        CHECK(!Has(F(d)) && Has(F(d) + ".bad"), "torn file set aside as .bad, live name free");
        // and the next write does not touch the set-aside file
        std::string before, after;
        GetFile(F(d) + ".bad", &before);
        CHECK(NpSafeFile::Replace(F(d).c_str(), kNewSave, SaveOpt()), "replace after set-aside");
        GetFile(F(d) + ".bad", &after);
        CHECK(before == after, ".bad untouched by the next write");
        std::string live; GetFile(F(d), &live);
        CHECK(live == kNewSave, "new save live");
        // a second damaged file goes to .bad2, the first is kept
        PutFile(F(d), std::string(100, '\0'));
        PutFile(F(d), std::string(kNewSave.size(), '\1'));
        CHECK(NpSafeFile::Recover(F(d).c_str(), SaveOpt()) == NpSafeFile::Found::SetAside && Has(F(d) + ".bad2") && Has(F(d) + ".bad"), "second set-aside keeps the first");
        for (const char* sfx : {"", ".tmp", ".bak", ".bad", ".bad2"}) unlink((F(d) + sfx).c_str());
        rmdir(d.c_str());
    }
    // an empty live file (a cut right after create) is removed, not set aside
    {
        char tmpl[] = "/tmp/np_safe_file_XXXXXX";
        const std::string d = mkdtemp(tmpl);
        PutFile(F(d), "");
        CHECK(NpSafeFile::Recover(F(d).c_str(), SaveOpt()) == NpSafeFile::Found::EmptyRemoved && !Has(F(d)) && !Has(F(d) + ".bad"), "empty removed");
        rmdir(d.c_str());
    }
    // a deleted file does not come back from its backup, at any cut
    {
        Scenario rm = {"Remove: P and P.bak, cut anywhere", SetupOldWithBak, OpRemove, RecoverSave, kOldSave, kNewSave, "file", true};
        for (bool power : {false, true}) {
            // after the delete ran to the end the file must be gone; before it finished, old or gone
            Result r = RunMatrix(rm, power);
            printf("  %-44s %-5s %3d cuts, %d bad%s%s\n", rm.name, power ? "power" : "kill", r.cases, r.bad, r.bad ? ": " : "", r.firstBad.c_str());
            CHECK(r.bad == 0, "remove");
        }
        char tmpl[] = "/tmp/np_safe_file_XXXXXX";
        const std::string d = mkdtemp(tmpl);
        SetupOldWithBak(d);
        NpSafeFile::Remove(F(d).c_str());
        NpSafeFile::Recover(F(d).c_str(), SaveOpt());
        CHECK(!Has(F(d)) && !Has(F(d) + ".bak"), "deleted file stays deleted after a restart's recovery");
        // the live file gone by cut-in-the-middle of Remove: P.tmp, P.bak removed first, so what is left is P
        rmdir(d.c_str());
        // Discard: set aside, not destroyed, and nothing comes back as the live file
        const std::string d2 = [] { char t[] = "/tmp/np_safe_file_XXXXXX"; return std::string(mkdtemp(t)); }();
        SetupOldWithBak(d2);
        NpSafeFile::Discard(F(d2).c_str());
        NpSafeFile::Recover(F(d2).c_str(), SaveOpt());
        std::string bad; GetFile(F(d2) + ".bad", &bad);
        CHECK(!Has(F(d2)) && !Has(F(d2) + ".bak") && bad == kOldSave, "discarded file set aside, live name stays free");
        for (const char* sfx : {"", ".tmp", ".bak", ".bad"}) unlink((F(d2) + sfx).c_str());
        rmdir(d2.c_str());
    }

    // set-aside copies are capped: the 3 newest are kept (.bad newest), older ones are deleted when a new one is set aside (the card cannot fill up)
    {
        char tmpl[] = "/tmp/np_safe_file_XXXXXX";
        const std::string d = mkdtemp(tmpl);
        for (int i = 1; i <= 7; ++i) {
            PutFile(F(d), std::string(kNewSave.size(), char(i)));   // seven different damaged saves, one after the other
            CHECK(NpSafeFile::Recover(F(d).c_str(), SaveOpt()) == NpSafeFile::Found::SetAside, "set aside %d", i);
            CHECK(!Has(F(d)), "live name free");
        }
        std::string b1, b2, b3;
        GetFile(F(d) + ".bad", &b1); GetFile(F(d) + ".bad2", &b2); GetFile(F(d) + ".bad3", &b3);
        CHECK(b1 == std::string(kNewSave.size(), char(7)) && b2 == std::string(kNewSave.size(), char(6)) && b3 == std::string(kNewSave.size(), char(5)), "the three newest, newest first");
        int n = 0;
        for (int i = 1; i <= 12; ++i) n += Has(F(d) + (i == 1 ? std::string(".bad") : ".bad" + std::to_string(i)));
        CHECK(n == 3, "exactly three set-aside copies, found %d", n);
        // a larger set from an older build is cut back too
        PutFile(F(d) + ".bad4", "x"); PutFile(F(d) + ".bad9", "x");
        PutFile(F(d), std::string(kNewSave.size(), char(8)));
        NpSafeFile::Recover(F(d).c_str(), SaveOpt());
        CHECK(!Has(F(d) + ".bad4") && !Has(F(d) + ".bad9"), "copies beyond the cap removed");
        // a game-side delete (Discard) obeys the cap as well, leaves the .bad copies, and takes the .bak and .tmp
        PutFile(F(d), kNewSave); PutFile(F(d) + ".bak", kOldSave); PutFile(F(d) + ".tmp", "partial");
        CHECK(NpSafeFile::Discard(F(d).c_str()), "discard found something");
        CHECK(!Has(F(d)) && !Has(F(d) + ".bak") && !Has(F(d) + ".tmp") && Has(F(d) + ".bad") && Has(F(d) + ".bad2") && Has(F(d) + ".bad3") && !Has(F(d) + ".bad4"), "delete: bak and tmp gone, three .bad kept");
        std::string newest; GetFile(F(d) + ".bad", &newest);
        CHECK(newest == kNewSave, "the deleted save is the newest set-aside copy");
        CHECK(!NpSafeFile::Discard(F(d).c_str()), "second discard finds nothing");
        // only a backup left (the live file was set aside when it was opened): the delete still removes it
        PutFile(F(d) + ".bak", kOldSave);
        CHECK(NpSafeFile::Discard(F(d).c_str()) && !Has(F(d) + ".bak"), "an orphan backup goes with the delete");
        NpSafeFile::RemoveAll(F(d).c_str());
        for (int i = 1; i <= 9; ++i) CHECK(!Has(F(d) + (i == 1 ? std::string(".bad") : ".bad" + std::to_string(i))), "RemoveAll leaves no .bad%d", i);
        rmdir(d.c_str());
    }
    // Forget of a credential file: every copy goes, at the end of the call none is left (the matrix: cut anywhere, the live file is old or gone; and a completed call leaves nothing)
    {
        char tmpl[] = "/tmp/np_safe_file_XXXXXX";
        const std::string d = mkdtemp(tmpl);
        for (const char* sfx : {"", ".tmp", ".bak", ".bad", ".bad2", ".bad3"}) PutFile(F(d) + sfx, "secret-token-copy");
        NpSafeFile::RemoveAll(F(d).c_str());
        for (const char* sfx : {"", ".tmp", ".bak", ".bad", ".bad2", ".bad3"}) CHECK(!Has(F(d) + sfx), "forget leaves no %s", sfx);
        rmdir(d.c_str());
    }


    // ---- wave 14 (Galaxy savelife review): a card that cannot say, or that errs once, does not make a good save "missing" or "damaged" ----
    {
        using namespace NpSafeFile;
        static int sStatFails = 0, sReadFails = 0, sStatCalls = 0;
        static std::string sFailPath;
        auto hookStat = [](const char* path, struct stat* st) -> int {
            if (sFailPath == path) { ++sStatCalls; if (sStatFails != 0) { if (sStatFails > 0) --sStatFails; errno = EIO; return -1; } }
            return ::stat(path, st);
        };
        auto hookRead = [](int fd, void* b, size_t n) -> ssize_t {
            if (sReadFails != 0) { if (sReadFails > 0) --sReadFails; errno = EIO; return -1; }
            return ::read(fd, b, n);
        };
        detail::H().stat = hookStat;
        detail::H().read = hookRead;
        detail::H().gapUs = [](unsigned) { return 0; };
        detail::H().retries = 2;
        auto fresh = [&](std::string* dir) {
            char tmpl[] = "/tmp/np_safe_file_XXXXXX";
            *dir = mkdtemp(tmpl);
            PutFile(F(*dir), kNewSave);
            PutFile(F(*dir) + ".bak", kOldSave);
            sFailPath = F(*dir);
            sStatFails = sReadFails = sStatCalls = 0;
        };
        auto cleanup = [&](const std::string& d) { for (const char* sfx : {"", ".tmp", ".bak", ".bad", ".bad2", ".bad3"}) unlink((F(d) + sfx).c_str()); rmdir(d.c_str()); };
        std::string d, live;
        // 1. the stat of the live file fails twice, then answers: the newest save stays live (before: not there -> the older .bak put over it)
        fresh(&d);
        sStatFails = 2;
        CHECK(Recover(F(d).c_str(), SaveOpt()) == Found::Primary, "a stat that errs twice then answers: the file is sound");
        CHECK(GetFile(F(d), &live) && live == kNewSave && !Has(F(d) + ".bad"), "newest save untouched, nothing set aside");
        cleanup(d);
        // 2. the stat keeps failing: undecided, nothing changed, nothing written
        fresh(&d);
        sStatFails = -1;
        CHECK(Recover(F(d).c_str(), SaveOpt()) == Found::Undecided, "a stat that always errs: undecided");
        sStatFails = 0;
        CHECK(GetFile(F(d), &live) && live == kNewSave && Has(F(d) + ".bak") && !Has(F(d) + ".bad"), "files exactly as they were");
        sStatFails = -1;
        CHECK(!Replace(F(d).c_str(), kOlderSave, SaveOpt()), "Replace refuses while the card cannot say");
        Writer w;
        CHECK(!BeginWrite(F(d).c_str(), &w, SaveOpt()), "BeginWrite refuses while the card cannot say");
        sStatFails = 0;
        CHECK(GetFile(F(d), &live) && live == kNewSave && !Has(F(d) + ".tmp"), "no write happened, no .tmp left");
        cleanup(d);
        // 3. a file that is not there is asked about once (no pause for the normal case)
        fresh(&d);
        unlink(F(d).c_str());
        unlink((F(d) + ".bak").c_str());
        CHECK(Recover(F(d).c_str(), SaveOpt()) == Found::Nothing && sStatCalls == 1, "ENOENT is answered at once (%d stat calls)", sStatCalls);
        cleanup(d);
        // 4. a read of the live file that fails twice, then answers: the file is sound (before: damaged -> set aside, the older .bak restored)
        fresh(&d);
        sReadFails = 2;
        CHECK(Recover(F(d).c_str(), SaveOpt()) == Found::Primary, "a read that errs twice then answers: the file is sound");
        CHECK(GetFile(F(d), &live) && live == kNewSave && !Has(F(d) + ".bad"), "newest save still live");
        cleanup(d);
        // 5. a read that always fails is still a damaged file, as before: set aside, the .bak restored
        fresh(&d);
        sReadFails = -1;
        const Found f = Recover(F(d).c_str(), SaveOpt());
        sReadFails = 0;
        CHECK(f == Found::FromBak || f == Found::SetAside, "a lasting read error is handled as damaged as before (%s)", FoundName(f));
        cleanup(d);
        detail::H() = detail::Hooks();
        detail::H().retries = 0;   // (the old single look) read it again from the environment next time
        detail::H().retries = -1;
    }

    // ---- the old writers must FAIL the same matrix (it can see the faults) ----
    const Scenario oldOnes[] = {
        {"OLD settings writer, no fsync",           SetupOldText, OpLegacyText, RecoverLegacyText, kOld, kNew, "file"},
        {"OLD bare tmp+rename (account files)",     SetupOldText, OpLegacyBare, RecoverLegacyBare, kOld, kNew, "file"},
        {"OLD bare tmp+rename, no old file",        SetupNone, OpLegacyBare, RecoverLegacyBare, "", kNew, "file"},
        {"OLD save written in place",               SetupOldSave, OpLegacySaveInPlace, RecoverNone, kOldSave, kNewSave, "file"},
    };
    for (const Scenario& s : oldOnes) {
        int badTotal = 0;
        for (bool power : {false, true}) {
            Result r = RunMatrix(s, power);
            printf("  %-44s %-5s %3d cuts, %d bad%s%s\n", s.name, power ? "power" : "kill", r.cases, r.bad, r.bad ? ": " : "", r.firstBad.c_str());
            badTotal += r.bad;
        }
        CHECK(badTotal > 0, "the matrix did not catch the old writer: %s", s.name);
    }

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
