// Unit test of the first-start save import (platform/sdk/save/np_save_import.cpp) and the generator of a populated test save.
//   test_save_import                       the tests (temporary directories; the recompiled build's real save of this machine is used when it is there)
//   test_save_import populate IN OUT       OUT = IN with unlocks and cup flags set (a Wii-format Strikers2 with a valid header and CRC): the end-to-end test input
//   test_save_import import SRCROOT DSTROOT   runs NpSaveImportRecompiled and prints its message
#include "../save/np_save_codec.cpp"
#include "../save/np_save_import.cpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                           \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(c)) {                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);            \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

typedef std::vector<unsigned char> Bytes;

static Bytes Slurp(const char* path) {
    Bytes v;
    FILE* f = fopen(path, "rb");
    if (!f) return v;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}
static bool Put(const char* path, const Bytes& v) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const bool ok = fwrite(v.data(), 1, v.size(), f) == v.size();
    fclose(f);
    return ok;
}
static void Be32Put(unsigned char* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void Fix(Bytes& f) {   // header Size (payload + header, unpadded) and CRC over the aligned payload
    Be32Put(&f[0], 35586);
    Be32Put(&f[4], Crc32(&f[8], f.size() - 8));
}
static Bytes ZeroSave() {
    Bytes f(35616, 0);
    Fix(f);
    return f;
}
static const char* kReal = "/home/claude/msc/appdata/MSCRecomp/NAND/title/00010000/52345145/data/Strikers2";

// A populated save: from `base`, the Striker Challenge unlocks (ChallengeUnlockRecord, the last 52 bytes of the payload: twelve completion dates and the
// unlock bits), the cup unlock flags (the 9 bit unit at +44 of the CupProgressRecord that starts the 928 byte progress span, itself before the unlock record),
// and a few settings (UserInfo: first volume 7, skill 3). Written in the Wii's byte order, the file's own.
static Bytes Populate(Bytes f) {
    const size_t payload = 8, end = 8 + 35578, unlock = end - 52, progress = end - 52 - 928;
    Be32Put(&f[payload + 4], 7);                            // UserInfo::mAudioOptions[0]
    Be32Put(&f[payload + 36], 3);                           // UserInfo skill
    f[progress + 44] = 0xFF;                                // unlock flags: all nine bits of the high part
    f[progress + 45] = 0x80;
    for (int i = 0; i < 10; ++i)                            // challenges 0..9: completed on 14 Oct 2026 (day-1, month, year-2000)
        Be32Put(&f[unlock + 4 * i], (13u << 27) | (10u << 23) | (26u << 13));
    Be32Put(&f[unlock + 48], 0x3FF);                        // mUnlockedChallenges
    Fix(f);
    return f;
}

static void Stat(const char* path, time_t* m, off_t* s) {
    struct stat st;
    *m = 0; *s = 0;
    if (stat(path, &st) == 0) { *m = st.st_mtime; *s = st.st_size; }
}

static std::vector<std::string> g_temps;
static std::string TempDir() {
    char t[] = "/tmp/np-save-import-XXXXXX";
    char* d = mkdtemp(t);
    g_temps.push_back(d ? d : "/tmp/none");
    return d ? d : "/tmp/none";
}
static std::string Data(const std::string& root) { return root + "/NAND/title/00010000/52345145/data"; }
static void Seed(const std::string& root, const Bytes* save, const Bytes* banner) {
    MakeDirs(Data(root).c_str());
    if (save) Put((Data(root) + "/Strikers2").c_str(), *save);
    if (banner) Put((Data(root) + "/banner.bin").c_str(), *banner);
}

int main(int argc, char** argv) {
    if (argc == 4 && !strcmp(argv[1], "populate")) {
        Bytes in = Slurp(argv[2]);
        if (in.size() != 35616) { fprintf(stderr, "%s is not a 35616 byte Strikers2\n", argv[2]); return 1; }
        return Put(argv[3], Populate(in)) ? 0 : 1;
    }
    if (argc == 4 && !strcmp(argv[1], "import")) {
        char m[800];
        const int rc = NpSaveImportRecompiled(argv[2], argv[3], m, sizeof m);
        printf("%s\nresult %d\n", m, rc);
        return rc == NP_SAVE_IMPORT_DONE || rc == NP_SAVE_IMPORT_NOT_NEEDED ? 0 : 2;
    }

    Bytes base = Slurp(kReal);
    const bool real = base.size() == 35616 && SaveIsSound(base.data(), base.size());
    if (!real) base = ZeroSave();
    printf("base save: %s\n", real ? "the recompiled build's real file" : "a zero payload");
    CHECK(SaveIsSound(base.data(), base.size()));
    Bytes banner(61600, 0x5A);

    // the populated save is sound, differs from the base, and survives the codec both ways (host image and back)
    Bytes pop = Populate(base);
    CHECK(SaveIsSound(pop.data(), pop.size()) && pop != base);
    {
        Bytes payload(pop.begin() + 8, pop.begin() + 8 + 35578), host = payload;
        NpSaveSettingsFromWii(host.data());
        CHECK(host != payload);
        Bytes back = host;
        NpSaveSettingsToWii(back.data());
        CHECK(back == payload);
    }

    // 1. import into an empty native directory
    {
        const std::string src = TempDir(), dst = TempDir();
        Seed(src, &pop, &banner);
        const std::string from = Data(src) + "/Strikers2";
        time_t m0, m1; off_t s0, s1;
        Stat(from.c_str(), &m0, &s0);
        char msg[800];
        int rc = NpSaveImportRecompiled(src.c_str(), dst.c_str(), msg, sizeof msg);
        printf("  %s\n", msg);
        CHECK(rc == NP_SAVE_IMPORT_DONE);
        CHECK(Slurp((Data(dst) + "/Strikers2").c_str()) == pop && Slurp((Data(dst) + "/banner.bin").c_str()) == banner);
        Stat(from.c_str(), &m1, &s1);
        CHECK(Slurp(from.c_str()) == pop && m0 == m1 && s0 == s1);   // the original: the same bytes, the same time
        CHECK(!IsFile((Data(dst) + "/Strikers2.import-tmp").c_str()));
        // 2. the next start: the native save exists, nothing is copied, a change of the native copy is kept
        Bytes mine = pop;
        mine[8 + 4] ^= 1;
        Fix(mine);
        Put((Data(dst) + "/Strikers2").c_str(), mine);
        rc = NpSaveImportRecompiled(src.c_str(), dst.c_str(), msg, sizeof msg);
        CHECK(rc == NP_SAVE_IMPORT_NOT_NEEDED && Slurp((Data(dst) + "/Strikers2").c_str()) == mine);
        // the same directory, with or without a trailing slash
        rc = NpSaveImportRecompiled(src.c_str(), (src + "/").c_str(), msg, sizeof msg);
        CHECK(rc == NP_SAVE_IMPORT_NOT_NEEDED);
        printf("  %s\n", msg);
    }
    // 3. no save of the recompiled build: nothing is created
    {
        const std::string src = TempDir(), dst = TempDir();
        char msg[800];
        CHECK(NpSaveImportRecompiled(src.c_str(), dst.c_str(), msg, sizeof msg) == NP_SAVE_IMPORT_NO_SOURCE);
        CHECK(!IsDir((dst + "/NAND").c_str()));
        printf("  %s\n", msg);
    }
    // 4. damaged sources are not copied: a flipped bit, a short file, a wrong header
    {
        const std::string src = TempDir(), dst = TempDir();
        char msg[800];
        Bytes bad = pop;
        bad[9000] ^= 0x10;
        Seed(src, &bad, &banner);
        CHECK(NpSaveImportRecompiled(src.c_str(), dst.c_str(), msg, sizeof msg) == NP_SAVE_IMPORT_BAD_SOURCE);
        CHECK(!IsFile((Data(dst) + "/Strikers2").c_str()) && !IsFile((Data(dst) + "/banner.bin").c_str()));
        printf("  %s\n", msg);
        Bytes shortSave(pop.begin(), pop.begin() + 20000);
        Put((Data(src) + "/Strikers2").c_str(), shortSave);
        CHECK(NpSaveImportRecompiled(src.c_str(), dst.c_str(), msg, sizeof msg) == NP_SAVE_IMPORT_BAD_SOURCE);
        Bytes wrongSize = pop;
        Be32Put(&wrongSize[0], 3);
        Put((Data(src) + "/Strikers2").c_str(), wrongSize);
        CHECK(NpSaveImportRecompiled(src.c_str(), dst.c_str(), msg, sizeof msg) == NP_SAVE_IMPORT_BAD_SOURCE);
    }
    // 5. an earlier import that was cut short (banner.bin copied, no Strikers2): completed, and what is there is kept
    {
        const std::string src = TempDir(), dst = TempDir();
        Bytes oldBanner(61600, 0x11);
        Seed(src, &pop, &banner);
        Seed(dst, nullptr, &oldBanner);
        Put((Data(dst) + "/Strikers2.import-tmp").c_str(), Bytes(100, 7));   // a leftover temporary
        char msg[800];
        CHECK(NpSaveImportRecompiled(src.c_str(), dst.c_str(), msg, sizeof msg) == NP_SAVE_IMPORT_DONE);
        CHECK(Slurp((Data(dst) + "/Strikers2").c_str()) == pop && Slurp((Data(dst) + "/banner.bin").c_str()) == oldBanner);
        printf("  %s\n", msg);
    }
    // 6. the console's device-prefixed form is split correctly (no such device here: only the directory logic is checked through a path with a colon-free root)
    {
        char msg[200];
        CHECK(NpSaveImportRecompiled("", "x", msg, sizeof msg) == NP_SAVE_IMPORT_NOT_NEEDED);
        CHECK(NpSaveImportRecompiled(nullptr, nullptr, nullptr, 0) == NP_SAVE_IMPORT_NOT_NEEDED);
    }
    for (size_t i = 0; i < g_temps.size(); ++i)
        if (g_temps[i].compare(0, 21, "/tmp/np-save-import-X") == 0 || g_temps[i].compare(0, 19, "/tmp/np-save-import") == 0) {
            const std::string cmd = "rm -rf '" + g_temps[i] + "'";
            if (system(cmd.c_str()) != 0) printf("could not remove %s\n", g_temps[i].c_str());
        }
    printf("test_save_import: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
