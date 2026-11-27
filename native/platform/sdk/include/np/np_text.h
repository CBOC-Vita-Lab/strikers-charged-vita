// np_text.h: the port's own player-facing texts in the game's language (1.3).
//
// The disc carries the game's own texts in English, North American Spanish and North American French, and MSC_LANGUAGE picks one
// (platform/sdk/src/os_core.cpp). Everything the port added itself (the Settings screen, the online and local multiplayer menus, the
// online banner, the loading screen's lines, the waiting notice and the error screen) is written in English in the code. This header
// gives those English texts in Spanish and French, keyed by the English text itself: a text with no translation, or English, is shown
// as written. The table is np_text_table.h, made from np_text_strings.tsv by native/platform/vita/tools/gen_text.py.
//
// Header only (the Vita's Settings screen links without the game, the SDK layer and the game both include it), no allocation, no game
// headers. Texts are UTF-8; the Settings font draws ASCII and Latin-1 (vita_font.h), the game's popups take UTF-16 (ToUtf16 below).
// It changes what is drawn only: nothing in a match, the simulation or the online protocol reads these texts.
#ifndef NP_TEXT_H
#define NP_TEXT_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace NpText {

enum Language { kEnglish = 0, kSpanish = 1, kFrench = 2 };

struct Entry {
    const char* en;
    const char* es;
    const char* fr;
};

inline const Entry* Table(int* count) {
    static const Entry kTable[] = {
#include "np_text_table.h"
    };
    if (count) *count = int(sizeof kTable / sizeof kTable[0]);
    return kTable;
}

// MSC_LANGUAGE the way os_core.cpp reads it: es / sp... Spanish, fr... French, en... English (case does not matter); -1 for anything else.
inline int ParseLanguage(const char* l) {
    if (!l || !l[0]) return -1;
    const int a = l[0] | 0x20, b = l[1] ? (l[1] | 0x20) : 0;
    if ((a == 'e' && b == 's') || (a == 's' && b == 'p')) return kSpanish;
    if (a == 'f' && b == 'r') return kFrench;
    if (a == 'e' && b == 'n') return kEnglish;
    return -1;
}

// The language of the game texts as the game itself reads it at start: MSC_LANGUAGE, else NP_LANG (the SDK's number, 3 French, 4 Spanish).
inline int LanguageFromEnv() {
    const int l = ParseLanguage(getenv("MSC_LANGUAGE"));
    if (l >= 0) return l;
    const char* n = getenv("NP_LANG");
    if (n && *n) {
        const int v = atoi(n);
        if (v == 4) return kSpanish;
        if (v == 3) return kFrench;
    }
    return kEnglish;
}

// Read once, like the game's own choice (a Settings change applies at the next start).
inline int CurrentLanguage() {
    static int language = -1;
    if (language < 0) language = LanguageFromEnv();
    return language;
}

// The text in `language`, or `en` itself when there is none.
inline const char* In(int language, const char* en) {
    if (!en || !*en || language <= kEnglish || language > kFrench) return en;
    int count = 0;
    const Entry* t = Table(&count);
    for (int i = 0; i < count; ++i)
        if (strcmp(t[i].en, en) == 0) {
            const char* s = language == kSpanish ? t[i].es : t[i].fr;
            return s && *s ? s : en;
        }
    return en;
}
inline const char* Tr(const char* en) { return In(CurrentLanguage(), en); }

// ---- texts with a placeholder (1.3) ----------------------------------------------------------------------------------------------------
// A message built from a number or a code ("...refused the request (503).", "Using the relay (no direct path).") is formatted in English by
// its caller and never equals a table row. A table row whose English holds %s, %d, %u or %lu is a TEMPLATE: the formatted English text is
// matched against it (the literal parts must agree; a %s takes at least one character, a %d/%u a run of digits), the pieces that stood for
// the placeholders are cut out, and the Spanish or French template is filled with them. Nothing is ever passed to printf: the pieces are
// copied as text, so a wrong or hostile piece cannot reach a conversion (and %zu, %lld... are not understood by the Vita's C library:
// the table may only hold %s %d %u %lu, which tests/test_text_table.py checks). A %s piece that is itself a table text ("no answer") or a
// template ("status 503") is translated too, one level deep; other pieces (codes, names, versions) stay as they are. The order of the
// placeholders is the same in all three languages (the test checks that too).
namespace TemplateDetail {
struct Arg {
    const char* s;
    size_t len;
};
enum { kMaxArgs = 4 };

// A conversion at `t`: its length (2, or 3 for %lu) and its letter (s, d or u); 0 when `t` is not at one.
inline int Spec(const char* t, char* conv) {
    if (t[0] != '%') return 0;
    if (t[1] == 's' || t[1] == 'd' || t[1] == 'u') { *conv = t[1]; return 2; }
    if (t[1] == 'l' && t[2] == 'u') { *conv = 'u'; return 3; }
    return 0;
}

// Does `s` (whole) match the English template `t`? Fills args with the pieces of the placeholders, in order.
inline bool MatchAt(const char* t, const char* s, Arg* args, int n, int* total) {
    while (*t) {
        char conv = 0;
        const int sl = Spec(t, &conv);
        if (sl) {
            if (n >= kMaxArgs) return false;
            t += sl;
            if (conv == 's') {
                for (size_t len = 1; s[len - 1]; ++len) {   // the shortest piece that lets the rest match
                    args[n].s = s;
                    args[n].len = len;
                    if (MatchAt(t, s + len, args, n + 1, total)) return true;
                }
                return false;
            }
            const char* e = s;
            if (conv == 'd' && *e == '-') ++e;
            const char* digits = e;
            while (*e >= '0' && *e <= '9') ++e;
            if (e == digits) return false;
            args[n].s = s;
            args[n].len = size_t(e - s);
            ++n;
            s = e;
            continue;
        }
        if (t[0] == '%' && t[1] == '%') {
            if (*s != '%') return false;
            ++s;
            t += 2;
            continue;
        }
        if (*s != *t) return false;
        ++s;
        ++t;
    }
    *total = n;
    return *s == 0;
}
}  // namespace TemplateDetail

// `in` (English, already formatted) in `language` through a template row; false when no template matches. Writes at most size-1 bytes and a NUL.
inline bool Template(int language, const char* in, char* out, size_t size, int depth = 0) {
    using namespace TemplateDetail;
    if (language <= kEnglish || language > kFrench || !in || !*in || !out || !size) return false;
    int count = 0;
    const Entry* t = Table(&count);
    for (int i = 0; i < count; ++i) {
        const char* en = t[i].en;
        bool has = false;
        for (const char* p = en; *p && !has; ++p) {
            char c = 0;
            has = Spec(p, &c) != 0;
        }
        if (!has) continue;
        Arg args[kMaxArgs];
        int n = 0;
        if (!MatchAt(en, in, args, 0, &n)) continue;
        const char* tr = language == kSpanish ? t[i].es : t[i].fr;
        if (!tr || !*tr) continue;
        size_t o = 0;
        auto put = [&](const char* s, size_t len) {
            for (size_t k = 0; k < len && o + 1 < size; ++k) out[o++] = s[k];
        };
        int a = 0;
        for (const char* p = tr; *p;) {
            char conv = 0;
            const int sl = Spec(p, &conv);
            if (sl && a < n) {
                char piece[160];
                size_t len = args[a].len < sizeof piece - 1 ? args[a].len : sizeof piece - 1;
                memcpy(piece, args[a].s, len);
                piece[len] = 0;
                ++a;
                p += sl;
                if (conv == 's') {
                    const char* x = In(language, piece);
                    char inner[160];
                    if (x != piece) put(x, strlen(x));
                    else if (depth < 1 && Template(language, piece, inner, sizeof inner, depth + 1)) put(inner, strlen(inner));
                    else put(piece, len);
                } else {
                    put(piece, len);
                }
            } else if (p[0] == '%' && p[1] == '%') {
                put("%", 1);
                p += 2;
            } else {
                put(p, 1);
                ++p;
            }
        }
        out[o] = 0;
        return true;
    }
    return false;
}

// A message made of several sentences (a reason followed by "The match goes on.", a status followed by "Press Circle to cancel."): the
// whole text when the table has it, else each sentence on its own; a sentence the table does not have stays English. A sentence ends at
// '.', '!' or '?' (runs such as "..." included) followed by a space or the end. Writes at most size-1 bytes and a NUL.
inline void Sentences(int language, const char* in, char* out, size_t size) {
    if (!out || !size) return;
    out[0] = 0;
    if (!in) return;
    size_t n = 0;
    auto put = [&](const char* s, size_t len) {
        if (n + 1 >= size) return;
        if (len > size - 1 - n) len = size - 1 - n;
        memcpy(out + n, s, len);
        n += len;
        out[n] = 0;
    };
    const char* whole = In(language, in);
    if (whole != in || language <= kEnglish) {
        put(whole, strlen(whole));
        return;
    }
    char formatted[512];
    if (Template(language, in, formatted, sizeof formatted)) {   // the whole text is one message with a number or a code in it
        put(formatted, strlen(formatted));
        return;
    }
    const char* p = in;
    while (*p) {
        const char* space = p;
        while (*p == ' ') ++p;
        put(space, size_t(p - space));
        if (!*p) break;
        const char* q = p;
        while (*q) {
            if (*q == '.' || *q == '!' || *q == '?') {
                const char* r = q;
                while (*r == '.' || *r == '!' || *r == '?') ++r;
                q = r;
                if (*r == ' ' || *r == 0) break;
                continue;
            }
            ++q;
        }
        char sentence[512];
        const size_t len = size_t(q - p);
        if (len < sizeof sentence) {
            memcpy(sentence, p, len);
            sentence[len] = 0;
            const char* t = In(language, sentence);
            char tb[512];
            if (t == sentence && Template(language, sentence, tb, sizeof tb)) t = tb;
            put(t, t == sentence ? len : strlen(t));
        } else {
            put(p, len);
        }
        p = q;
    }
}

// The next code point of UTF-8 text (advances s); a byte that is not valid UTF-8 reads as Latin-1.
inline unsigned NextCodePoint(const char*& s) {
    const unsigned char c = (unsigned char)*s;
    if (c < 0x80) { ++s; return c; }
    const unsigned char d = (unsigned char)s[1];
    if ((c & 0xE0) == 0xC0 && (d & 0xC0) == 0x80) { s += 2; return (unsigned(c & 0x1F) << 6) | (d & 0x3F); }
    const unsigned char e = d ? (unsigned char)s[2] : 0;
    if ((c & 0xF0) == 0xE0 && (d & 0xC0) == 0x80 && (e & 0xC0) == 0x80) { s += 3; return (unsigned(c & 0x0F) << 12) | (unsigned(d & 0x3F) << 6) | (e & 0x3F); }
    const unsigned char f = e ? (unsigned char)s[3] : 0;
    if ((c & 0xF8) == 0xF0 && (d & 0xC0) == 0x80 && (e & 0xC0) == 0x80 && (f & 0xC0) == 0x80) {
        s += 4;
        return (unsigned(c & 0x07) << 18) | (unsigned(d & 0x3F) << 12) | (unsigned(e & 0x3F) << 6) | (f & 0x3F);
    }
    ++s;
    return c;
}

// UTF-8 to the game's UTF-16 text (at most `capacity` units and a 0). Line breaks stay; other control characters and anything outside the
// Basic Multilingual Plane become '?'.
inline int ToUtf16(const char* in, unsigned short* out, int capacity) {
    int n = 0;
    while (in && *in && n < capacity) {
        const unsigned cp = NextCodePoint(in);
        out[n++] = (cp == '\n' || (cp >= 0x20 && cp != 0x7F && cp < 0x10000 && !(cp >= 0x80 && cp < 0xA0))) ? (unsigned short)cp : (unsigned short)'?';
    }
    out[n] = 0;
    return n;
}

}  // namespace NpText

#endif  // NP_TEXT_H
