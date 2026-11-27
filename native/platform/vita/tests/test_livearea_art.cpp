// Host test for vita_livearea_art.cpp: builds icon0.png, bg.png, startup.png, and pic0.png from a disc files dir.
// Usage:
//   test_livearea_art <files-dir> <out-dir>
//   test_livearea_art --check-generic <png-path>
#include "vita_livearea_art.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

static bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    long sz = std::ftell(f);
    if (sz <= 0) { std::fclose(f); return false; }
    std::rewind(f);
    out.resize(static_cast<size_t>(sz));
    bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--check-generic") == 0) {
        std::vector<uint8_t> raw;
        if (!ReadFile(argv[2], raw)) {
            std::fprintf(stderr, "FAIL: cannot read %s\n", argv[2]);
            return 2;
        }
        bool isGen = VitaLiveAreaArt::IsGenericIconBytes(raw.data(), raw.size());
        std::printf("%s\n", isGen ? "GENERIC" : "CUSTOM");
        return isGen ? 0 : 1;
    }
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <files-dir> <out-dir>\n", argv[0]);
        return 2;
    }
    VitaLiveAreaArt::BuiltArt art;
    char why[160] = "";
    if (!VitaLiveAreaArt::BuildFromFilesDir(argv[1], art, why, sizeof why)) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        return 1;
    }
    std::string out(argv[2]);
    if (!WriteFile(out + "/icon0.png", art.icon0) ||
        !WriteFile(out + "/bg.png", art.bg) ||
        !WriteFile(out + "/startup.png", art.startup) ||
        !WriteFile(out + "/pic0.png", art.pic0)) {
        std::fprintf(stderr, "FAIL: write error\n");
        return 1;
    }
    std::printf("OK: icon0=%zu bg=%zu startup=%zu pic0=%zu\n",
                art.icon0.size(), art.bg.size(), art.startup.size(), art.pic0.size());
    return 0;
}
