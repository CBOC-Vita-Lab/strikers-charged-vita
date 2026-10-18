// Unit test of the save codec (platform/sdk/save): real save files written by the retail game's code (the recompiled build
// runs the translated game on big-endian memory) are decoded to host structures and encoded back; random data checks the
// bit-field conversion. No game needed.
#include "../save/np_save_codec.cpp"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static std::vector<unsigned char> slurp(const char* path) {
    std::vector<unsigned char> v;
    FILE* f = fopen(path, "rb");
    if (!f) return v;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}

static uint32_t crc32_ref(const unsigned char* p, size_t n) {   // the game's nlChecksum32 = the standard CRC-32
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static uint32_t be32(const unsigned char* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// host structure mirrors (the layout of UserInfo)
struct HostUser {
    uint32_t saveId;
    int audio[6];
    bool autoZoom;
    float zoom;
    int skill, limitType, time, goalLimit, numGames;
};

static void check_file(const char* path) {
    std::vector<unsigned char> f = slurp(path);
    CHECK(f.size() == 35616);
    if (f.size() != 35616) return;
    CHECK(be32(&f[0]) == 35586);
    CHECK(be32(&f[4]) == crc32_ref(&f[8], f.size() - 8));   // the header's CRC covers the aligned payload
    CHECK(NpSaveSettingsPayloadSize() == 35578);
    std::vector<unsigned char> payload(f.begin() + 8, f.begin() + 8 + 35578);
    // to the host and back: byte for byte the file
    std::vector<unsigned char> host = payload;
    NpSaveSettingsFromWii(host.data());
    std::vector<unsigned char> back = host;
    NpSaveSettingsToWii(back.data());
    CHECK(back == payload);
    // the user info decodes to sane values (volumes 10, zoom 0.5, skill ROOKIE ...)
    HostUser u;
    memcpy(&u, host.data(), sizeof(u) - 0);
    CHECK(u.audio[0] == 10 && u.audio[1] == 10 && u.audio[2] == 10);
    CHECK(u.autoZoom == true && u.zoom == 0.5f);
    CHECK(u.skill >= 0 && u.skill <= 4);
    CHECK(u.limitType >= 0 && u.limitType < 4);
}

static void check_random() {
    srand(1234);
    for (int round = 0; round < 50; ++round) {
        std::vector<unsigned char> a(NpSaveSettingsPayloadSize());
        for (auto& b : a) b = (unsigned char)rand();
        std::vector<unsigned char> orig = a;
        NpSaveSettingsFromWii(a.data());
        CHECK(a != orig || round == 0);
        NpSaveSettingsToWii(a.data());
        CHECK(a == orig);
        std::vector<unsigned char> o(NpSaveOnlinePayloadSize());
        for (auto& b : o) b = (unsigned char)rand();
        orig = o;
        NpSaveOnlineFromWii(o.data());
        NpSaveOnlineToWii(o.data());
        CHECK(o == orig);
    }
}

// one bit-field unit by hand: CupHistoryRecord's first word. Wii: captain 4 bits at the top ... year offset in the low 10 bits.
static void check_bitfields() {
    // a record as the Wii wrote it: captain 5, sidekick1 3, sidekick2 7, sidekick3 1, day 29, month 11, year 1023
    uint32_t w = (5u << 28) | (3u << 25) | (7u << 22) | (1u << 19) | (29u << 14) | (11u << 10) | 1023u;
    unsigned char b[4] = {(unsigned char)(w >> 24), (unsigned char)(w >> 16), (unsigned char)(w >> 8), (unsigned char)w};
    const uint8_t* widths = kBitUnits[1];   // {4,3,3,3,5,4,10}
    CHECK(widths[0] == 4 && widths[6] == 10);
    bf_to_host(b, 4, widths);
    uint32_t v = ld32(b);   // host order, packed from the least significant bit
    CHECK((v & 15) == 5 && ((v >> 4) & 7) == 3 && ((v >> 7) & 7) == 7 && ((v >> 10) & 7) == 1 && ((v >> 13) & 31) == 29 &&
          ((v >> 18) & 15) == 11 && ((v >> 22) & 1023) == 1023);
    bf_to_wii(b, 4, widths);
    CHECK(be32(b) == w);
    // the u16 unit of CupProgressRecord: unlock flags 9 bits (top), the rest 7
    unsigned char h[2] = {(unsigned char)((0x155u << 7 | 0x2A) >> 8), (unsigned char)((0x155u << 7 | 0x2A) & 0xFF)};
    bf_to_host(h, 2, kBitUnits[0]);
    uint16_t hv = ld16(h);
    CHECK((hv & 0x1FF) == 0x155 && (hv >> 9) == 0x2A);
}

static void check_banner(const char* path) {
    std::vector<unsigned char> f = slurp(path);
    CHECK(f.size() == NP_SAVE_BANNER_SIZE);
    if (f.size() != NP_SAVE_BANNER_SIZE) return;
    // build the host image the game would have: header words and the strings in host order
    std::vector<unsigned char> host = f;
    swap_n(host.data(), 4, 2);
    swap_n(host.data() + 8, 2, 1);
    swap_n(host.data() + 0x20, 2, 64);
    CHECK(ld32(host.data()) == 0x5749424Eu);   // 'WIBN'
    CHECK(ld16(host.data() + 0x20) == 'M' && ld16(host.data() + 0x22) == 'a');
    std::vector<unsigned char> out(NP_SAVE_BANNER_SIZE);
    NpSaveBannerToWii(host.data(), out.data());
    CHECK(out == f);
}

int main(int argc, char** argv) {
    const char* dirs[] = {"/home/claude/msc/appdata/MSCRecomp/NAND/title/00010000/52345145/data",
                          "/home/claude/msc/build/host-gl/appdata/MSCRecomp/NAND/title/00010000/52345145/data",
                          "/home/claude/msc/build/split/v3k/pref/ux0/data/mscharged/NAND/title/00010000/52345145/data",
                          "/home/claude/msc/build/rel/v3k/pref/ux0/data/mscharged/NAND/title/00010000/52345145/data",
                          "/home/claude/msc/build/npm/runs/pair-03/peer0/xdg/MSCRecomp/NAND/title/00010000/52345145/data"};
    int files = 0;
    for (int i = 1; i < argc; ++i) { check_file(argv[i]); ++files; }
    if (argc == 1) {
        for (const char* d : dirs) {
            char p[600];
            snprintf(p, sizeof p, "%s/Strikers2", d);
            struct stat st;
            if (stat(p, &st) == 0) { check_file(p); ++files; }
            snprintf(p, sizeof p, "%s/banner.bin", d);
            if (stat(p, &st) == 0) check_banner(p);
        }
    }
    check_random();
    check_bitfields();
    printf("test_save_format: %d real files, %d checks, %d failed\n", files, g_checks, g_fail);
    return g_fail ? 1 : 0;
}
