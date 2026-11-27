// Replacing and recovering the player's small state files (settings, online account, retry queue, the game's save) so that a power cut or a crash at any
// instant leaves the live file exactly as it was or exactly as it becomes, and a file that is damaged is set aside, never loaded and never overwritten.
// Header only, plain POSIX (open/write/fsync/rename/unlink): the same code runs on the console and in the host tests, where tests/test_safe_file.cpp kills
// a child before every file-changing call and models a power cut that keeps renames but loses data that was never synced.
//
// Facts this is built on (read from the vitasdk, 12 Nov 2026):
//  - newlib's rename() on the Vita is sceIoRemove(new) followed by sceIoRename(old, new): replacing a file by rename is NOT atomic there. A cut between the
//    two calls leaves no file under the live name. So the live name is only ever renamed to when it does not exist, and the previous version is kept
//    aside under <path>.bak for the moment between.
//  - a rename can survive a power cut whose data did not (file present, right length, zero tail), unless the data was fsync'ed first. fsync() exists in the
//    vitasdk's libc; it is called before the rename, and its failure is not fatal (a write that cannot be synced is still better than none).
//
// Files for a path P:  P live;  P.tmp being written (complete and synced once the replace has started);  P.bak the previous version while a replace is in
// progress (kept for good when keepBackup: the game's save);  P.bad, P.bad2, P.bad3 damaged files set aside: P.bad is the newest, and at most Options::keepBad
// (3) are kept, the oldest is deleted when a new one is set aside (so a card cannot fill up with them).
//
// Replace(P, data):   write P.tmp, fsync, close;  P -> P.bak;  P.tmp -> P;  (P.bak removed unless keepBackup)
// Recover(P):         P valid: nothing;  else the newest valid of P.tmp (only next to a P.bak: the replace had got that far), P.bak becomes P (a damaged P is set aside first);  else a damaged P is set aside
//                     (an empty P is just removed: it holds nothing) and the answer is "no file". Idempotent, and safe to cut anywhere.
// Remove(P):          P.tmp and P.bak first, then P: a deleted file cannot come back from a backup. Discard(P) sets P aside instead (nothing is destroyed).
// RemoveAll(P):       Remove, and every P.bad* as well: for the online account's files (token, name, retry queue), which must leave no copy behind when forgotten.
#pragma once

#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace NpSafeFile {

typedef bool (*Valid)(const std::string& data);

struct Options {
    Valid valid = nullptr;     // whether the bytes are a complete file; null: any non-empty content
    bool keepBackup = false;   // keep the previous version as P.bak after a successful replace
    bool sync = true;          // fsync the new file before it is renamed into place
    unsigned mode = 0600;      // permissions of the files made
    size_t maxBytes = 1u << 20;
    int keepBad = 3;           // how many set-aside copies (P.bad ... ) are kept; older ones are deleted when a new one is set aside (1..9)
    bool removeEmpty = true;   // Recover removes an empty live file that has nothing to restore it (false: the game has just created it and is about to write)
};

enum class Found { Nothing, Primary, FromTmp, FromBak, SetAside, EmptyRemoved, Undecided };

inline const char* FoundName(Found f) {
    switch (f) {
    case Found::Nothing: return "no file";
    case Found::Primary: return "file sound";
    case Found::FromTmp: return "interrupted replace completed from the new copy";
    case Found::FromBak: return "damaged or missing file restored from its backup";
    case Found::SetAside: return "damaged file set aside (not loaded, not overwritten)";
    case Found::EmptyRemoved: return "empty file removed";
    case Found::Undecided: return "the card would not say whether the file is there (left exactly as it is, nothing written over it)";
    }
    return "";
}

namespace detail {

// (wave 14, from the Galaxy port's savelife review) A stat answer other than "there" or "not there" says nothing about the file, and a read that fails once
// right after the console woke up says little about it either. Both are asked again (kRetries times, kRetryGapUs apart; MSC_SAFE_FILE_RETRY=0 leaves it at
// one look as before) before a file is called missing or damaged: a good newest save must not be set aside, and an older .bak put in its place, because the
// card hiccuped. A stat that STILL cannot tell leaves the files alone (Found::Undecided; Replace and BeginWrite then write nothing).
// The two function pointers are the test's seam (a stat or a read that fails the way a card does); production never changes them.
struct Hooks {
    int (*stat)(const char*, struct stat*) = &::stat;
    ssize_t (*read)(int, void*, size_t) = &::read;
    int (*gapUs)(unsigned) = nullptr;   // null: usleep
    int retries = -1;                    // -1: from MSC_SAFE_FILE_RETRY (default 2), once
};
inline Hooks& H() { static Hooks h; return h; }
inline int Retries() {
#ifdef NP_SAFEFILE_BREAK_RETRY
    return 0;   // (test mutant: one look, as before wave 14)
#endif
    Hooks& h = H();
    if (h.retries < 0) {
        const char* e = ::getenv("MSC_SAFE_FILE_RETRY");
        const long v = e && *e ? ::strtol(e, nullptr, 10) : 2;
        h.retries = v < 0 ? 0 : v > 5 ? 5 : int(v);
    }
    return h.retries;
}
constexpr unsigned kRetryGapUs = 30 * 1000;
inline void Gap() {
    if (H().gapUs) H().gapUs(kRetryGapUs);
    else ::usleep(kRetryGapUs);
}

enum class Look { Present, Absent, Unknown };
// Present / Absent / Unknown. ENOENT and ENOTDIR (a folder that is not there yet) are "absent"; any other error is asked again, then "unknown".
inline Look LookAt(const std::string& p) {
    struct stat st;
    for (int i = 0;; ++i) {
        if (H().stat(p.c_str(), &st) == 0) return Look::Present;
        if (errno == ENOENT || errno == ENOTDIR) return Look::Absent;
#ifdef NP_SAFEFILE_BREAK_UNKNOWN
        return Look::Absent;   // (test mutant: any stat error is "not there", as before wave 14)
#endif
        if (i >= Retries()) return Look::Unknown;
        Gap();
    }
}

inline std::string Suffix(const std::string& p, const char* s) { return p + s; }

inline bool Exists(const std::string& p) {
    struct stat st;
    return H().stat(p.c_str(), &st) == 0;
}

// 1: read; 0: more than maxBytes (not asked again); -1: the open or a read failed (errno set; ENOENT / ENOTDIR: the file is not there)
inline int ReadOnce(const std::string& p, std::string& out, size_t maxBytes) {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return -1;
    out.clear();
    char buf[4096];
    int rc = 1;
    for (;;) {
        ssize_t n = H().read(fd, buf, sizeof buf);
        if (n < 0) { rc = -1; break; }
        if (n == 0) break;
        out.append(buf, size_t(n));
        if (out.size() > maxBytes) { rc = 0; break; }
    }
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return rc;
}

// A failed read is asked again (see Hooks): a card that answered with an error once right after a wake-up usually answers the second time.
inline bool ReadAll(const std::string& p, std::string& out, size_t maxBytes) {
    for (int i = 0;; ++i) {
        const int rc = ReadOnce(p, out, maxBytes);
        if (rc == 1) return true;
        if (rc == 0 || errno == ENOENT || errno == ENOTDIR || i >= Retries()) return false;
        Gap();
    }
}

inline bool WriteFully(int fd, const char* d, size_t n) {
    while (n) {
        ssize_t w = ::write(fd, d, n);
        if (w <= 0) return false;
        d += w;
        n -= size_t(w);
    }
    return true;
}

inline bool Sound(const Options& o, const std::string& data) {
    if (o.valid) return o.valid(data);
    return !data.empty();
}

inline std::string BadName(const std::string& p, int i) { return i == 1 ? p + ".bad" : p + ".bad" + std::to_string(i); }

// P -> P.bad; the earlier copies move up one name (.bad -> .bad2 -> .bad3), and the one that would pass keepBad is deleted first, so P.bad is always the newest and
// at most keepBad exist. Only names that do not exist are renamed to (newlib's rename on the Vita removes the target first). A cut in the middle loses at most
// an old set-aside copy; the live file is untouched until the last rename, so Recover simply does it again.
inline bool SetAsideFile(const std::string& p, int keepBad = 3) {
    if (keepBad < 1) keepBad = 1;
    if (keepBad > 9) keepBad = 9;
    for (int i = keepBad + 1; i <= 9; ++i) ::unlink(BadName(p, i).c_str());   // (more than the cap: made by an older build or a larger setting)
    ::unlink(BadName(p, keepBad).c_str());
    for (int i = keepBad - 1; i >= 1; --i) {
        const std::string from = BadName(p, i);
        if (Exists(from)) ::rename(from.c_str(), BadName(p, i + 1).c_str());
    }
    return ::rename(p.c_str(), BadName(p, 1).c_str()) == 0;
}

// The new bytes in P.tmp, complete, synced, closed. False: nothing is left behind.
inline bool WriteTmp(const std::string& tmp, const std::string& data, const Options& o) {
    ::unlink(tmp.c_str());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, o.mode);
    if (fd < 0) return false;
    bool ok = WriteFully(fd, data.data(), data.size());
    if (ok && o.sync) ::fsync(fd);
    if (::close(fd) != 0) ok = false;
    if (!ok) ::unlink(tmp.c_str());
    return ok;
}

}  // namespace detail

inline Found Recover(const char* path, const Options& o = Options()) {
    using namespace detail;
    const std::string p = path, t = Suffix(p, ".tmp"), b = Suffix(p, ".bak");
    std::string data;
    const Look look = LookAt(p);
    if (look == Look::Unknown) return Found::Undecided;   // (the card cannot say whether the file is there: change nothing)
    bool primaryExists = look == Look::Present, primaryEmpty = false;
    if (primaryExists) {
        if (ReadAll(p, data, o.maxBytes)) {
            if (Sound(o, data)) return Found::Primary;
            primaryEmpty = data.empty();
        }   // unreadable: treated as damaged
    }
    std::string fromTmp, fromBak;
    // P.tmp is complete only once a replace has got as far as moving the live file to P.bak; before that it is a half-written file (which a loose check such as
    // "no run of zero bytes" would take for a sound one), so it counts only next to a P.bak.
    const bool bakOk = Exists(b) && ReadAll(b, fromBak, o.maxBytes) && Sound(o, fromBak);
    const bool tmpOk = Exists(b) && Exists(t) && ReadAll(t, fromTmp, o.maxBytes) && Sound(o, fromTmp);
    if (!tmpOk && !bakOk) {
        if (!primaryExists) return Found::Nothing;
        if (primaryEmpty) {
            if (!o.removeEmpty) return Found::Nothing;
            ::unlink(p.c_str());
            return Found::EmptyRemoved;
        }
        SetAsideFile(p, o.keepBad);
        return Found::SetAside;
    }
    // the newest sound copy: the new one of an interrupted replace, else the previous version
    if (primaryExists) {
        if (primaryEmpty) ::unlink(p.c_str());
        else if (!SetAsideFile(p, o.keepBad)) return Found::Nothing;
    }
    const std::string& from = tmpOk ? t : b;
    if (::rename(from.c_str(), p.c_str()) != 0) return Found::Nothing;
    return tmpOk ? Found::FromTmp : Found::FromBak;
}

inline bool Replace(const char* path, const std::string& data, const Options& o = Options()) {
    using namespace detail;
    const std::string p = path, t = Suffix(p, ".tmp"), b = Suffix(p, ".bak");
    if (Recover(path, o) == Found::Undecided) return false;   // finish what an earlier cut left; a damaged live file is set aside here, not overwritten below
    if (!WriteTmp(t, data, o)) return false;
    const Look lookP = LookAt(p);
    if (lookP == Look::Unknown) { ::unlink(t.c_str()); return false; }   // (cannot tell whether a live file is there: never rename over it blind)
    const bool had = lookP == Look::Present;
    if (had) {
        if (Exists(b) && ::unlink(b.c_str()) != 0) { ::unlink(t.c_str()); return false; }
        if (::rename(p.c_str(), b.c_str()) != 0) { ::unlink(t.c_str()); return false; }
    }
    if (::rename(t.c_str(), p.c_str()) != 0) {
        if (had) ::rename(b.c_str(), p.c_str());
        ::unlink(t.c_str());
        return false;
    }
    if (had && !o.keepBackup) ::unlink(b.c_str());
    return true;
}

// A deliberate delete: nothing is left that Recover could bring back.
inline void Remove(const char* path) {
    const std::string p = path;
    ::unlink(detail::Suffix(p, ".tmp").c_str());
    ::unlink(detail::Suffix(p, ".bak").c_str());
    ::unlink(p.c_str());
}

// The player (or the game) gave the file up: it is set aside, not destroyed. Nothing is left that Recover could bring back as the live file.
// True when there was anything (the file, its .tmp or its .bak) to give up. Set-aside .bad copies are left alone.
inline bool Discard(const char* path, int keepBad = 3) {
    const std::string p = path;
    bool any = false;
    if (::unlink(detail::Suffix(p, ".tmp").c_str()) == 0) any = true;
    if (::unlink(detail::Suffix(p, ".bak").c_str()) == 0) any = true;
    if (detail::Exists(p)) { detail::SetAsideFile(p, keepBad); any = true; }
    return any;
}

// Forgetting a file whose copies must not stay on the card (the online account): the file, .tmp, .bak and every set-aside .bad copy (also the ones above the cap).
inline void RemoveAll(const char* path) {
    const std::string p = path;
    Remove(path);
    for (int i = 1; i <= 9; ++i) ::unlink(detail::BadName(p, i).c_str());
}

// ---- a write that arrives in pieces (the game's NANDOpen / NANDWrite / NANDClose) ----------------------------------------------------------------------
// BeginWrite hands out a descriptor of P.tmp that starts as a copy of the live file (so a partial write keeps the rest as it was); Commit makes it the live
// file the way Replace does. The live file is untouched until Commit. Abort drops the copy.
struct Writer {
    int fd = -1;
    std::string path;
    bool keepBackup = false;
};

inline bool BeginWrite(const char* path, Writer* w, const Options& o = Options()) {
    using namespace detail;
    const std::string p = path, t = Suffix(p, ".tmp");
    if (Recover(path, o) == Found::Undecided) return false;
    std::string old;
    const bool had = Exists(p) && ReadAll(p, old, o.maxBytes);
    ::unlink(t.c_str());
    int fd = ::open(t.c_str(), O_RDWR | O_CREAT | O_TRUNC, o.mode);
    if (fd < 0) return false;
    if (had && !WriteFully(fd, old.data(), old.size())) { ::close(fd); ::unlink(t.c_str()); return false; }
    ::lseek(fd, 0, SEEK_SET);
    w->fd = fd;
    w->path = p;
    w->keepBackup = o.keepBackup;
    return true;
}

inline bool Commit(Writer* w, const Options& o = Options()) {
    using namespace detail;
    if (w->fd < 0) return false;
    const std::string p = w->path, t = Suffix(p, ".tmp"), b = Suffix(p, ".bak");
    if (o.sync) ::fsync(w->fd);
    bool ok = ::close(w->fd) == 0;
    w->fd = -1;
    if (!ok) { ::unlink(t.c_str()); return false; }
    const Look lookP = LookAt(p);
    if (lookP == Look::Unknown) { ::unlink(t.c_str()); return false; }
    const bool had = lookP == Look::Present;
    if (had) {
        if (Exists(b) && ::unlink(b.c_str()) != 0) { ::unlink(t.c_str()); return false; }
        if (::rename(p.c_str(), b.c_str()) != 0) { ::unlink(t.c_str()); return false; }
    }
    if (::rename(t.c_str(), p.c_str()) != 0) {
        if (had) ::rename(b.c_str(), p.c_str());
        ::unlink(t.c_str());
        return false;
    }
    if (had && !w->keepBackup) ::unlink(b.c_str());
    return true;
}

inline void Abort(Writer* w) {
    if (w->fd >= 0) ::close(w->fd);
    w->fd = -1;
    ::unlink((w->path + ".tmp").c_str());
}

// ---- the game's save file ---------------------------------------------------------------------------------------------------------------------------
// The format SaveLoad.cpp writes and checks: an 8 byte big-endian header (Size, CRC-32 of everything after the header), then the payload; the file ends at the
// aligned size. Sound = the header's Size fits the file and the CRC matches. (An empty file, a file whose data was lost, a half written file are not sound.)
inline uint32_t Crc32(const unsigned char* p, size_t n) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            table[i] = c;
        }
        made = true;
    }
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline bool GameSaveSound(const std::string& d) {
    if (d.size() < 16) return false;
    const unsigned char* u = reinterpret_cast<const unsigned char*>(d.data());
    auto be = [u](size_t at) { return uint32_t(u[at]) << 24 | uint32_t(u[at + 1]) << 16 | uint32_t(u[at + 2]) << 8 | uint32_t(u[at + 3]); };
    return be(0) >= 8 && be(0) <= d.size() && be(4) == Crc32(u + 8, d.size() - 8);
}

inline Options GameSaveOptions() {
    Options o;
    o.valid = GameSaveSound;
    o.keepBackup = true;   // the previous good save stays as <name>.bak
    o.mode = 0666;
    return o;
}

}  // namespace NpSafeFile
