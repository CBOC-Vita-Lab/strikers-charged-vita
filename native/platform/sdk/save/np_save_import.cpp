// See np_save_import.h. Plain POSIX (stdio, stat, rename): the same code runs on host32 (the unit test, test_save_import) and on the console.
#include "np_save_import.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

const char kTitleData[] = "/NAND/title/00010000/52345145/data";   // "R4QE" as hex: the title's data directory (nand_native.cpp kHome)
const char kSave[] = "Strikers2";
const size_t kSaveSize = 35616;          // 8 byte header + 35578 payload, padded to 32 (platform/sdk/save: np_save_game.cpp)

void Say(char* message, size_t size, const char* format, ...) __attribute__((format(printf, 3, 4)));
void Say(char* message, size_t size, const char* format, ...) {
    if (!message || !size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(message, size, format, args);
    va_end(args);
}

bool IsFile(const char* path, size_t* size = nullptr) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (size) *size = static_cast<size_t>(st.st_size);
    return true;
}
bool IsDir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// mkdir -p; "ux0:data/x" and "ux0:/data/x": the device prefix is not a directory.
bool MakeDirs(const char* path) {
    char buf[1100];
    const size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return false;
    memcpy(buf, path, n + 1);
    size_t start = 1;
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == ':') { start = i + 2; break; }
        if (buf[i] == '/') break;
    }
    for (size_t i = start; i < n; ++i)
        if (buf[i] == '/') {
            buf[i] = 0;
            if (!IsDir(buf)) mkdir(buf, 0777);
            buf[i] = '/';
        }
    if (!IsDir(buf)) mkdir(buf, 0777);
    return IsDir(buf);
}

uint32_t Crc32(const unsigned char* p, size_t n) {   // the game's nlChecksum32: the standard CRC-32
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
uint32_t Be32(const unsigned char* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

bool ReadAll(const char* path, unsigned char** data, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (length < 0) { fclose(f); return false; }
    *data = static_cast<unsigned char*>(malloc(length ? static_cast<size_t>(length) : 1));
    const size_t got = *data ? fread(*data, 1, static_cast<size_t>(length), f) : 0;
    fclose(f);
    *size = got;
    if (!*data || got != static_cast<size_t>(length)) { free(*data); *data = nullptr; return false; }
    return true;
}

// The same checks the game's loader makes (SaveLoad.cpp): the size, the header's Size word, the CRC-32 of everything after the header.
bool SaveIsSound(const unsigned char* d, size_t n) {
    if (n != kSaveSize) return false;
    if (Be32(d) < 8 || Be32(d) > n) return false;
    return Be32(d + 4) == Crc32(d + 8, n - 8);
}

bool CopyFile(const char* from, const char* to) {
    unsigned char* data = nullptr;
    size_t size = 0;
    if (!ReadAll(from, &data, &size)) return false;
    char temporary[1200];
    snprintf(temporary, sizeof(temporary), "%s.import-tmp", to);
    FILE* out = fopen(temporary, "wb");
    bool ok = out != nullptr;
    if (out) {
        ok = fwrite(data, 1, size, out) == size;
        if (fclose(out) != 0) ok = false;
    }
    free(data);
    if (ok) {
        remove(to);   // (the destination does not exist; a failed earlier attempt may have left one)
        ok = rename(temporary, to) == 0;
    }
    if (!ok) remove(temporary);
    return ok;
}

} // namespace

extern "C" int NpSaveImportRecompiled(const char* sourceRoot, const char* destRoot, char* message, size_t messageSize) {
    if (!sourceRoot || !destRoot || !*sourceRoot || !*destRoot) { Say(message, messageSize, "save import: no directory given"); return NP_SAVE_IMPORT_NOT_NEEDED; }
    // the same directory (a player who set NP_DATA_DIR to share the recompiled build's save): nothing to copy
    size_t a = strlen(sourceRoot), b = strlen(destRoot);
    while (a > 1 && sourceRoot[a - 1] == '/') --a;
    while (b > 1 && destRoot[b - 1] == '/') --b;
    if (a == b && strncmp(sourceRoot, destRoot, a) == 0) { Say(message, messageSize, "save import: the native game uses the recompiled build's directory, nothing to copy"); return NP_SAVE_IMPORT_NOT_NEEDED; }

    char fromDir[1100], toDir[1100], from[1200], to[1200];
    snprintf(fromDir, sizeof(fromDir), "%.*s%s", int(a), sourceRoot, kTitleData);
    snprintf(toDir, sizeof(toDir), "%.*s%s", int(b), destRoot, kTitleData);
    snprintf(to, sizeof(to), "%s/%s", toDir, kSave);
    if (IsFile(to)) { Say(message, messageSize, "save import: the native game has its own save (%s), nothing to copy", to); return NP_SAVE_IMPORT_NOT_NEEDED; }
    snprintf(from, sizeof(from), "%s/%s", fromDir, kSave);
    size_t size = 0;
    if (!IsFile(from, &size)) { Say(message, messageSize, "save import: no save of the recompiled build at %s", from); return NP_SAVE_IMPORT_NO_SOURCE; }
    unsigned char* data = nullptr;
    size_t got = 0;
    const bool read = ReadAll(from, &data, &got);
    const bool sound = read && SaveIsSound(data, got);
    free(data);
    if (!sound) {
        Say(message, messageSize, "save import: %s is not a sound save (%u bytes, size/CRC check %s): left alone, not copied", from, unsigned(size), read ? "failed" : "unreadable");
        return NP_SAVE_IMPORT_BAD_SOURCE;
    }
    if (!MakeDirs(toDir)) { Say(message, messageSize, "save import: cannot create %s", toDir); return NP_SAVE_IMPORT_FAILED; }
    // the other files of the directory first (banner.bin), the save last
    unsigned copied = 0;
    if (DIR* dir = opendir(fromDir)) {
        while (struct dirent* e = readdir(dir)) {
            if (e->d_name[0] == '.' || strcmp(e->d_name, kSave) == 0) continue;
            if (strstr(e->d_name, ".tmp") || strstr(e->d_name, ".import-tmp")) continue;
            char f[1400], t[1400];
            snprintf(f, sizeof(f), "%s/%s", fromDir, e->d_name);
            snprintf(t, sizeof(t), "%s/%s", toDir, e->d_name);
            if (!IsFile(f) || IsFile(t)) continue;
            if (!CopyFile(f, t)) { closedir(dir); Say(message, messageSize, "save import: could not copy %s to %s", f, t); return NP_SAVE_IMPORT_FAILED; }
            ++copied;
        }
        closedir(dir);
    }
    if (!CopyFile(from, to)) { Say(message, messageSize, "save import: could not copy %s to %s", from, to); return NP_SAVE_IMPORT_FAILED; }
    Say(message, messageSize, "save import: copied the recompiled build's save (%s, %u bytes, and %u other file(s)) to %s; the original is untouched", kSave, unsigned(size), copied, toDir);
    return NP_SAVE_IMPORT_DONE;
}
