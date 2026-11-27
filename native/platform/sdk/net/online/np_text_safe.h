// np_text_safe.h (wave 13, netclient13, MSC_NET_TEXT_ASCII): the online service only ever sends printable ASCII (names are A-Z a-z 0-9 space . _ -,
// its sentences are English), and a peer's name travels through the signaling answer and the LAN / room paths into the screen and the log. A
// string with a byte outside 0x20..0x7E (a line feed that forges a log line, UTF-8 the fonts cannot draw, a control byte) is therefore never
// taken: a JSON answer holding one is refused whole (fail closed: ONLINE "protocol" error, the same as a syntax error), and a name copied into
// a fixed field has every such byte turned into '?'. MSC_NET_TEXT_ASCII=0 puts the earlier behaviour back (bytes >= 0x20 pass).
// Header only, no platform types (tests/fuzz_net_test.cpp, json_cap_test.cpp). Counters are plain integers: diagnostics, read by the log lines.
#ifndef NP_NET_TEXT_SAFE_H
#define NP_NET_TEXT_SAFE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace NpNet {

struct TextStats {
    unsigned jsonRefused;    // answers refused because a string held a byte outside 0x20..0x7E
    unsigned namesFiltered;  // names copied with at least one byte replaced
};
inline TextStats& TextCounters() { static TextStats s = {0, 0}; return s; }
inline int& TextAsciiOverride() { static int v = -1; return v; }   // tests: 0 / 1 forces the switch, -1 reads the environment
inline bool TextAsciiOn() {
    if (TextAsciiOverride() >= 0) return TextAsciiOverride() != 0;
    static const bool on = [] { const char* v = getenv("MSC_NET_TEXT_ASCII"); return !(v && v[0] == '0'); }();
    return on;
}
inline bool IsPrintableAscii(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}
// A decoded JSON string (or key): every byte outside 0x20..0x7E becomes '?' (a UTF-8 sequence, lead byte plus its continuation bytes, becomes one '?'). The document is NOT refused:
// the service's admin / maintenance message (GET /v1/info, 503 service_disabled) is free text that may hold an en dash or a curly apostrophe. Switch off: unchanged.
inline void ScrubAscii(std::string* s) {
    if (!TextAsciiOn() || IsPrintableAscii(s->data(), s->size())) return;
    std::string r;
    r.reserve(s->size());
    for (size_t i = 0; i < s->size(); ++i) {
        const unsigned char c = static_cast<unsigned char>((*s)[i]);
        if (c >= 0x20 && c <= 0x7E) { r.push_back(char(c)); continue; }
        r.push_back('?');
        if (c >= 0xC0) while (i + 1 < s->size() && (static_cast<unsigned char>((*s)[i + 1]) & 0xC0) == 0x80) ++i;
    }
    s->swap(r);
    ++TextCounters().jsonRefused;   // (counts strings scrubbed)
}

// Copies at most cap-1 bytes of `src` (up to n bytes or its end) into `dst` and terminates it. With the switch on, a byte outside 0x20..0x7E becomes '?'.
// Returns the number of bytes written (not counting the terminator).
inline size_t SafeCopy(char* dst, size_t cap, const char* src, size_t n = static_cast<size_t>(-1)) {
    if (!dst || !cap) return 0;
    const bool on = TextAsciiOn();
    size_t i = 0;
    bool changed = false;
    for (; src && i < n && src[i] && i + 1 < cap; ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        if (on && (c < 0x20 || c > 0x7E)) { dst[i] = '?'; changed = true; }
        else dst[i] = static_cast<char>(c);
    }
    dst[i] = 0;
    if (changed) ++TextCounters().namesFiltered;
    return i;
}

}  // namespace NpNet

#endif
