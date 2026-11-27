// Host check of vita_bootart.cpp: builds the loading picture from a folder of game files and writes it as loading.bin.
//   g++ -std=gnu++17 -O2 -Isrc tests/test_bootart.cpp src/vita_bootart.cpp -o /tmp/tba && /tmp/tba <files folder> <out loading.bin> [runs]
// The folder is the disc's files folder (files/Art/fe/mainui.dmn and files/opening.bnr, any letter case). Prints the time and the heap peak.
#include "vita_bootart.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

static std::string FindFile(std::string root, const char* relative) {
    std::string rel = relative;
    size_t at = 0;
    while (true) {
        const size_t next = rel.find('/', at);
        const std::string part = rel.substr(at, next == std::string::npos ? std::string::npos : next - at);
        DIR* d = opendir(root.c_str());
        if (!d) return "";
        std::string match;
        while (dirent* e = readdir(d)) if (strcasecmp(e->d_name, part.c_str()) == 0) { match = e->d_name; break; }
        closedir(d);
        if (match.empty()) return "";
        root += "/" + match;
        if (next == std::string::npos) break;
        at = next + 1;
    }
    return root;
}

struct File {
    FILE* f = nullptr;
    uint64_t size = 0;
};
static bool ReadFn(void* ctx, uint64_t off, void* dst, size_t len) {
    File* f = static_cast<File*>(ctx);
    return fseeko(f->f, static_cast<off_t>(off), SEEK_SET) == 0 && fread(dst, 1, len, f->f) == len;
}
static bool Open(File& f, const std::string& path) {
    f.f = fopen(path.c_str(), "rb");
    if (!f.f) return false;
    struct stat st;
    stat(path.c_str(), &st);
    f.size = static_cast<uint64_t>(st.st_size);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <files folder> <out loading.bin> [runs]\n", argv[0]); return 2; }
    const std::string a = FindFile(argv[1], "Art/fe/mainui.dmn"), b = FindFile(argv[1], "opening.bnr");
    File fa, fb;
    if (a.empty() || b.empty() || !Open(fa, a) || !Open(fb, b)) { std::fprintf(stderr, "no game files in %s\n", argv[1]); return 3; }
    VitaBootArt::Source sa{ReadFn, &fa, fa.size}, sb{ReadFn, &fb, fb.size};
    std::vector<uint8_t> out(VitaBootArt::kCacheBytes);
    char why[160];
    VitaBootArt::Stats stats;
    const int runs = argc > 3 ? std::atoi(argv[3]) : 1;
    bool ok = false;
    for (int i = 0; i < runs; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        ok = VitaBootArt::Build(sa, sb, out.data(), out.size(), why, sizeof why, &stats);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s in %.1f ms, heap peak %zu bytes%s%s\n", ok ? "built" : "FAILED", ms, stats.peakBytes, ok ? "" : ": ", ok ? "" : why);
        if (!ok) return 1;
    }
    FILE* o = fopen(argv[2], "wb");
    if (!o) return 4;
    fwrite(out.data(), 1, VitaBootArt::kPictureBytes, o);
    fclose(o);
    return 0;
}
