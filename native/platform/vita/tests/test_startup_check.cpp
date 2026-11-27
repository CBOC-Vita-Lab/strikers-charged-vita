// Host test of the start-up messages: missing game folder, wrong disc, missing shader compiler.
#include "np_startup_check.h"
#include "../../sdk/include/np/np_text.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static bool Exists(const char* p) { struct stat st; return stat(p, &st) == 0; }
static bool Nothing(const char*) { return false; }
static bool Everything(const char*) { return true; }

static void Make(const std::string& p, bool dir) { if (dir) mkdir(p.c_str(), 0755); else { FILE* f = std::fopen(p.c_str(), "wb"); if (f) std::fclose(f); } }

int main() {
    char tmpl[] = "/tmp/startup-check-XXXXXX";
    std::string root = mkdtemp(tmpl);
    NpStartupCheck::Message m;

    // Nothing there: the game data is missing, and the message names both folders and the installer.
    CHECK(!NpStartupCheck::CheckGameData(root.c_str(), Exists, m));
    CHECK(m.category && std::strcmp(m.category, "the game data is missing") == 0);
    CHECK(std::strstr(m.details, (root + "/sys").c_str()) && std::strstr(m.details, (root + "/files").c_str()));
    CHECK(std::strstr(m.details, "np-player.txt") && !std::strstr(m.details, "env.txt"));

    // sys only: still missing (files/ is absent).
    Make(root + "/sys", true);
    Make(root + "/sys/fst.bin", false);
    CHECK(!NpStartupCheck::CheckGameData(root.c_str(), Exists, m));
    CHECK(std::strcmp(m.category, "the game data is missing") == 0);

    // Complete, boot.bin absent: nothing is claimed, all is well.
    Make(root + "/files", true);
    CHECK(NpStartupCheck::CheckGameData(root.c_str(), Exists, m));
    CHECK(m.category == nullptr);

    // Wrong id: named.
    { FILE* f = std::fopen((root + "/sys/boot.bin").c_str(), "wb"); std::fwrite("RMGE01\0\0", 1, 8, f); std::fclose(f); }
    CHECK(!NpStartupCheck::CheckGameData(root.c_str(), Exists, m));
    CHECK(std::strcmp(m.category, "this is not the right game disc") == 0);
    CHECK(std::strstr(m.details, "Disc id found: RMGE01"));

    // Right id, and a short or binary boot.bin claim nothing.
    { FILE* f = std::fopen((root + "/sys/boot.bin").c_str(), "wb"); std::fwrite("R4QE01\0\0", 1, 8, f); std::fclose(f); }
    CHECK(NpStartupCheck::CheckGameData(root.c_str(), Exists, m));
    { FILE* f = std::fopen((root + "/sys/boot.bin").c_str(), "wb"); std::fwrite("R4Q", 1, 3, f); std::fclose(f); }
    CHECK(NpStartupCheck::CheckGameData(root.c_str(), Exists, m));
    { FILE* f = std::fopen((root + "/sys/boot.bin").c_str(), "wb"); std::fwrite("\x01\x02\x03\x04\x05\x06", 1, 6, f); std::fclose(f); }
    CHECK(NpStartupCheck::CheckGameData(root.c_str(), Exists, m));

    // The compiler: either place counts.
    CHECK(!NpStartupCheck::CompilerPresent(Nothing));
    CHECK(NpStartupCheck::CompilerPresent(Everything));
    NpStartupCheck::RendererFailed(false, m);
    CHECK(std::strcmp(m.category, "the shader compiler is missing") == 0 && std::strstr(m.details, "ur0:data/libshacccg.suprx"));
    NpStartupCheck::RendererFailed(true, m);
    CHECK(std::strcmp(m.category, "the graphics could not start") == 0 && std::strstr(m.details, "log.txt"));
    // (words3) the renderer's own cause: one more line each, in every language a table row; the compiler cause is the compiler message
    for (int cause = 2; cause <= 6; ++cause) {
        NpStartupCheck::RendererFailed(true, m, cause);
        CHECK(std::strstr(m.details, "Cause: ") != nullptr && std::strstr(m.details, "log.txt"));
        const char* line = std::strstr(m.details, "Cause: ");
        char one[160];
        std::snprintf(one, sizeof one, "%.*s", (int)strcspn(line, "\n"), line);
        CHECK(NpText::In(NpText::kSpanish, one) != one && NpText::In(NpText::kFrench, one) != one);
    }
    NpStartupCheck::RendererFailed(true, m, 1);
    CHECK(std::strcmp(m.category, "the shader compiler is missing") == 0);
    NpStartupCheck::RendererFailed(true, m, 99);
    CHECK(std::strstr(m.details, "Cause: ") == nullptr);

    // Layout: every language wraps within the width, no line is cut inside a UTF-8 character, paths stay whole, nothing is lost.
    for (int lang = 0; lang < 3; ++lang) {
        static char out[20][NpStartupCheck::kLineBytes];
        static int g_lang; g_lang = lang;
        auto tr = [](const char* s) -> const char* { return NpText::In(g_lang, s); };
        const char* messages[2];
        NpStartupCheck::Message a, b, c;
        NpStartupCheck::CheckGameData("ux0:data/mscharged/game", Nothing, a);
        NpStartupCheck::RendererFailed(false, b);
        NpStartupCheck::RendererFailed(true, c);
        const NpStartupCheck::Message* all[3] = {&a, &b, &c};
        (void)messages;
        for (const auto* msg : all) {
            const int n = NpStartupCheck::FormatLines(msg->details, tr, 78, out, 20);
            CHECK(n >= 2 && n < 20);
            bool path = false;
            for (int i = 0; i < n; ++i) {
                int chars = 0;
                for (const unsigned char* q = reinterpret_cast<const unsigned char*>(out[i]); *q; ++q) {
                    if ((*q & 0xC0) != 0x80) ++chars;
                    if (*q >= 0x80 && (*q & 0xC0) != 0x80) { CHECK((q[1] & 0xC0) == 0x80); }
                }
                CHECK(chars <= 78);
                if (std::strstr(out[i], "ux0:data/mscharged/game/sys") || std::strstr(out[i], "ur0:data/libshacccg.suprx")) path = true;
            }
            if (msg != &c) CHECK(path);
        }
        // The translated first line differs from English in es and fr, and the category has a translation.
        if (lang > 0) {
            CHECK(std::strcmp(tr(a.category), a.category) != 0);
            static char one[2][NpStartupCheck::kLineBytes];
            NpStartupCheck::FormatLines(a.details, tr, 78, one, 2);
            CHECK(std::strncmp(one[0], "Copy your", 9) != 0);
        }
    }
    NpStartupCheck::Message w;
    std::strcpy(w.details, "Disc id found: RMGE01");
    { static char out[3][NpStartupCheck::kLineBytes]; auto tr = [](const char* s) -> const char* { return NpText::In(1, s); };
      CHECK(NpStartupCheck::FormatLines(w.details, tr, 78, out, 3) == 1 && std::strcmp(out[0], "Id del disco: RMGE01") == 0); }
    if (std::system(("rm -rf " + root).c_str())) {}
    std::printf(failures ? "startup check: %d FAILED\n" : "startup check: all passed\n", failures);
    return failures ? 1 : 0;
}
