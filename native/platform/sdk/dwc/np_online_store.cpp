// The player's credentials on disk: <data_dir>/online.dat. See np_dwc.h.
//
// Where the token lives, and why: not in the save. The save slot is a Wii-format file that players copy between machines and back up;
// a credential inside it would travel with every copy. The token is a file of its own next to the NAND directory
// (ux0:data/mscharged/online.dat), owner-readable on hosts, written atomically (temporary file, then rename) with a CRC so a torn write
// is detected and treated as "no token" (the service then issues a new account). Nothing in this file is ever logged.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/sdk/dwc/np_dwc.h"
#include "platform/sdk/save/np_safe_file.h"

namespace {

const uint8_t kMagic[4] = {'M', 'S', 'C', 'O'};
const uint32_t kVersion = 2;
// magic[4] version[4] profile[4] token[64] (text, NUL padded) crc[4]
const size_t kFileSize = 4 + 4 + 4 + NpOnline::kTokenChars + 4;

bool Path(char* out, size_t size, const char* suffix) {
    const int n = snprintf(out, size, "%s/online.dat%s", NpOnline::DataDir(), suffix);
    return n > 0 && size_t(n) < size;
}

void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

bool FileSound(const std::string& d) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(d.data());
    return d.size() == kFileSize && !memcmp(b, kMagic, 4) && Get32(b + 4) == kVersion && Get32(b + kFileSize - 4) == NpDwcCrc32(b, kFileSize - 4);
}
// Replaced and recovered by np_safe_file.h: synced; a power cut leaves the old file or the new one (on the console a bare rename over an existing file removes it
// first: a cut in between lost the account); a damaged file is set aside as online.dat.bad, never loaded and never overwritten.
NpSafeFile::Options FileOptions() { NpSafeFile::Options o; o.valid = FileSound; o.mode = 0600; o.maxBytes = 4096; return o; }

}  // namespace

bool NpOnlineTokenValid(const char* token) {
    size_t n = 0;
    for (; token && token[n]; ++n) {
        const char c = token[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        if (n >= NpOnline::kTokenChars - 1) return false;
    }
    return n >= 16;
}

bool NpOnlineLoadCredentials(NpOnlineCredentials* out) {
    char path[512];
    if (!out || !Path(path, sizeof path, "")) return false;
    NpSafeFile::Recover(path, FileOptions());
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[kFileSize + 1];
    const size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    bool ok = n == kFileSize && !memcmp(buf, kMagic, 4) && Get32(buf + 4) == kVersion && Get32(buf + kFileSize - 4) == NpDwcCrc32(buf, kFileSize - 4);
    if (ok) {
        out->profileId = int32_t(Get32(buf + 8));
        memcpy(out->token, buf + 12, NpOnline::kTokenChars);
        out->token[NpOnline::kTokenChars - 1] = 0;
        ok = out->profileId > 0 && NpOnlineTokenValid(out->token);
    }
    memset(buf, 0, sizeof buf);
    return ok;
}

bool NpOnlineSaveCredentials(const NpOnlineCredentials& in) {
    char path[512];
    if (in.profileId <= 0 || !NpOnlineTokenValid(in.token)) return false;
    if (!Path(path, sizeof path, "")) return false;
    uint8_t buf[kFileSize];
    memcpy(buf, kMagic, 4);
    Put32(buf + 4, kVersion);
    Put32(buf + 8, uint32_t(in.profileId));
    memset(buf + 12, 0, NpOnline::kTokenChars);
    strncpy(reinterpret_cast<char*>(buf + 12), in.token, NpOnline::kTokenChars - 1);
    Put32(buf + kFileSize - 4, NpDwcCrc32(buf, kFileSize - 4));
    std::string bytes(reinterpret_cast<const char*>(buf), kFileSize);
    memset(buf, 0, sizeof buf);
    const bool ok = NpSafeFile::Replace(path, bytes, FileOptions());
    bytes.assign(bytes.size(), '\0');
    return ok;
}

void NpOnlineForgetCredentials() {
    char path[512];
    if (Path(path, sizeof path, "")) NpSafeFile::RemoveAll(path);   // (and its .tmp, .bak and every set-aside .bad copy: a forgotten token leaves nothing on the card)
}
