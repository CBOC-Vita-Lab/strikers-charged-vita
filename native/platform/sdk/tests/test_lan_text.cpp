// np_lan_text.h (1.3): the disc's online messages replaced by id in every language file, in place, with the port's Spanish and French texts.
//   c++ -std=c++17 -Wall -Wextra -I<native> native/platform/sdk/tests/test_lan_text.cpp -o /tmp/test_lan_text && /tmp/test_lan_text   (tests/test_text_table.py runs it)
// The messages are made up here (letters of the right length at the ids); no disc text is used.
#include "platform/sdk/net/np_lan_text.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Lookup {
    unsigned long hash;
    unsigned long StringOffset;
};

// A language file with one made-up message per patch (of the disc's length in `lang`, or `forced` when not 0) between guard messages.
struct Fake {
    std::vector<Lookup> table;
    std::vector<unsigned short> text;
    Fake(int lang, size_t forced = 0) {
        add(0x11111111, 6, 'g');
        for (size_t p = 0; p < NpLanText::kPatchCount; ++p) {
            const int index = lang == NpText::kSpanish ? 1 : lang == NpText::kFrench ? 2 : 0;
            add(NpLanText::kPatches[p].id, forced ? forced : NpLanText::kPatches[p].discLength[index], 'x');
        }
        add(0x22222222, 6, 'g');
    }
    void add(unsigned long id, size_t len, char fill) {
        table.push_back({id, (unsigned long)text.size()});
        for (size_t i = 0; i < len; ++i) text.push_back((unsigned short)fill);
        text.push_back(0);
    }
    std::string at(size_t patch) const {
        std::string s;
        for (const unsigned short* c = &text[table[patch + 1].StringOffset]; *c; ++c) s += *c < 128 ? char(*c) : '?';
        return s;
    }
    std::u16string wide(size_t patch) const {
        std::u16string s;
        for (const unsigned short* c = &text[table[patch + 1].StringOffset]; *c; ++c) s += char16_t(*c);
        return s;
    }
};

static std::u16string Expect(int language, const char* key) {
    unsigned short w[512];
    const int n = NpText::ToUtf16(NpText::In(language, key), w, 511);
    return std::u16string(w, w + n);
}

int main() {
    using namespace NpLanText;
    CHECK(TextLanguage(0) == NpText::kEnglish && TextLanguage(6) == NpText::kEnglish && TextLanguage(2) == NpText::kEnglish && TextLanguage(9) == NpText::kEnglish);
    CHECK(TextLanguage(3) == NpText::kSpanish && TextLanguage(8) == NpText::kSpanish);
    CHECK(TextLanguage(1) == NpText::kFrench && TextLanguage(7) == NpText::kFrench);

    // every replacement fits the disc's message in its language, and none still names Nintendo, the Wii or an SD card
    for (size_t p = 0; p < kPatchCount; ++p)
        for (int l = 0; l < 3; ++l) {
            unsigned short w[512];
            const char* text = NpText::In(l, kPatches[p].key);
            const int n = NpText::ToUtf16(text, w, 511);
            CHECK(n > 0 && n <= kPatches[p].discLength[l]);
            if (n > kPatches[p].discLength[l]) std::printf("  patch %zu language %d: %d > %d\n", p, l, n, kPatches[p].discLength[l]);
            CHECK(!std::strstr(text, "NINTENDO") && !std::strstr(text, "Nintendo") && !std::strstr(text, "Wii") && !std::strstr(text, " SD "));
            if (l) CHECK(text != kPatches[p].key);   // a Spanish and a French text exist (a row of the table)
        }

    // each disc language file: the messages of the table are replaced by id, the guards and the lengths around them stay
    const int discLanguages[] = {0, 1, 3, 6, 7, 8};
    for (int disc : discLanguages) {
        const int language = TextLanguage(disc);
        Fake f(language);
        const int n = PatchById(f.table.data(), f.table.size(), f.text.data(), disc, true);
        // English files keep the messages np_prompts.cpp rewrites for the player's layout (translateOnly); every other language gets all of them
        int expected = 0;
        for (size_t p = 0; p < kPatchCount; ++p) expected += !(kPatches[p].translateOnly && language == NpText::kEnglish);
        CHECK(n == expected);
        for (size_t p = 0; p < kPatchCount; ++p) {
            if (kPatches[p].translateOnly && language == NpText::kEnglish) CHECK(f.at(p).find_first_not_of('x') == std::string::npos);
            else CHECK(f.wide(p) == Expect(language, kPatches[p].key));
        }
        CHECK(f.text[0] == 'g' && f.text[5] == 'g' && f.text[6] == 0);   // the guard before
        CHECK(f.text.back() == 0 && f.text[f.text.size() - 2] == 'g');   // and after
        // the old text's end mark is where it was: the rest of a longer message is zero, the next message starts at its offset
        for (size_t p = 0; p < kPatchCount; ++p) {
            const unsigned long end = f.table[p + 1].StringOffset + (kPatches[p].discLength[language == NpText::kSpanish ? 1 : language == NpText::kFrench ? 2 : 0]);
            CHECK(f.text[end] == 0);
        }
    }

    // MSC_ONLINE_TEXT=0: only the messages that are not part of the online wording (the two menu texts, the save/memory and controller ones)
    {
        Fake f(NpText::kSpanish);
        int offline = 0;
        for (size_t p = 0; p < kPatchCount; ++p) offline += !kPatches[p].online;
        CHECK(offline > 2);
        CHECK(PatchById(f.table.data(), f.table.size(), f.text.data(), 8, false) == offline);
        CHECK(f.wide(0) == Expect(NpText::kSpanish, "MULTIPLAYER") && f.wide(1) == Expect(NpText::kSpanish, kPatches[1].key));
        CHECK(f.at(2).find_first_not_of('x') == std::string::npos);   // left as it was
        for (size_t p = 0; p < kPatchCount; ++p)
            if (kPatches[p].online) CHECK(f.at(p).find_first_not_of('x') == std::string::npos);
    }

    // a message too short for the new text, an empty one, an unknown id: left alone
    {
        Fake f(NpText::kFrench, 5);
        CHECK(PatchById(f.table.data(), f.table.size(), f.text.data(), 7, true) == 0);
        CHECK(f.at(2) == "xxxxx");
        Fake g(NpText::kFrench, 0);
        g.table.push_back({0x33333333, 0});
        CHECK(PatchById(g.table.data(), g.table.size(), g.text.data(), 7, true) == (int)kPatchCount);
        CHECK(PatchById((const Lookup*)nullptr, 3, g.text.data(), 7, true) == 0);
        std::vector<unsigned short> empty = {0, 0, 0};
        std::vector<Lookup> t = {{kPatches[0].id, 0}};
        CHECK(PatchById(t.data(), t.size(), empty.data(), 7, true) == 0);
    }

    // English: the key itself
    {
        Fake f(NpText::kEnglish);
        PatchById(f.table.data(), f.table.size(), f.text.data(), 0, true);
        CHECK(f.at(0) == "MULTIPLAYER");
    }

    // the new messages keep their control marks as the disc writes them: {0} the code, {p} a break; no button name or Wii word
    for (size_t p = 0; p < kPatchCount; ++p)
        for (int l = 0; l < 3; ++l) {
            const char* text = NpText::In(l, kPatches[p].key);
            CHECK(!std::strstr(text, "{clr") && !std::strstr(text, "{nbs}") && !std::strstr(text, "MII") && !std::strstr(text, "WFC"));
            CHECK((std::strstr(NpText::In(0, kPatches[p].key), "{0}") != nullptr) == (std::strstr(text, "{0}") != nullptr));
            CHECK((std::strstr(NpText::In(0, kPatches[p].key), "{p}") != nullptr) == (std::strstr(text, "{p}") != nullptr));
        }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
