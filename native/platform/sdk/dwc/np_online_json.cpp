#include "platform/sdk/dwc/np_online_json.h"
#include "platform/sdk/net/online/np_text_safe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace NpOnline {

const JValue* JValue::Get(const char* key) const {
    if (type != Obj) return nullptr;
    for (size_t i = 0; i < o.size(); ++i)
        if (o[i].first == key) return &o[i].second;
    return nullptr;
}
std::string JValue::Str_(const char* key, const char* def) const { const JValue* v = Get(key); return v && v->type == Str ? v->s : std::string(def); }
double JValue::Num_(const char* key, double def) const { const JValue* v = Get(key); return v && v->type == Num ? v->n : def; }
bool JValue::Bool_(const char* key, bool def) const { const JValue* v = Get(key); return v && v->type == Bool ? v->b : def; }

namespace {

struct Parser {
    const char* p;
    const char* end;
    void Ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool Lit(const char* word) {
        const size_t n = strlen(word);
        if (size_t(end - p) < n || memcmp(p, word, n)) return false;
        p += n;
        return true;
    }
    static void Utf8(std::string* s, unsigned c) {
        if (c < 0x80) *s += char(c);
        else if (c < 0x800) { *s += char(0xC0 | (c >> 6)); *s += char(0x80 | (c & 0x3F)); }
        else { *s += char(0xE0 | (c >> 12)); *s += char(0x80 | ((c >> 6) & 0x3F)); *s += char(0x80 | (c & 0x3F)); }
    }
    bool String(std::string* out) {
        const bool ok = StringRaw(out);
        if (ok) NpNet::ScrubAscii(out);   // MSC_NET_TEXT_ASCII: bytes outside 0x20..0x7E become '?', the answer is kept
        return ok;
    }
    bool StringRaw(std::string* out) {
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end && *p != '"') {
            unsigned char c = static_cast<unsigned char>(*p++);
            if (c < 0x20) return false;
            if (c != '\\') { *out += char(c); continue; }
            if (p >= end) return false;
            c = static_cast<unsigned char>(*p++);
            switch (c) {
            case '"': case '\\': case '/': *out += char(c); break;
            case 'b': *out += '\b'; break;
            case 'f': *out += '\f'; break;
            case 'n': *out += '\n'; break;
            case 'r': *out += '\r'; break;
            case 't': *out += '\t'; break;
            case 'u': {
                if (end - p < 4) return false;
                unsigned v = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = *p++;
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= unsigned(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10);
                    else return false;
                }
                if (v >= 0xD800 && v <= 0xDFFF) v = '?';   // surrogate halves: not needed by the service
                Utf8(out, v);
                break;
            }
            default: return false;
            }
        }
        if (p >= end) return false;
        ++p;
        return true;
    }
    bool Value(JValue* v, int depth) {
        if (depth > 12) return false;
        Ws();
        if (p >= end) return false;
        const char c = *p;
        if (c == '{') {
            ++p;
            v->type = JValue::Obj;
            Ws();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                Ws();
                std::string key;
                if (!String(&key)) return false;
                Ws();
                if (p >= end || *p++ != ':') return false;
                JValue member;
                if (!Value(&member, depth + 1)) return false;
                v->o.push_back(std::make_pair(key, member));
                Ws();
                if (p >= end) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == '}') { ++p; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p;
            v->type = JValue::Arr;
            Ws();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                JValue item;
                if (!Value(&item, depth + 1)) return false;
                v->a.push_back(item);
                Ws();
                if (p >= end) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == ']') { ++p; return true; }
                return false;
            }
        }
        if (c == '"') { v->type = JValue::Str; return String(&v->s); }
        if (Lit("true")) { v->type = JValue::Bool; v->b = true; return true; }
        if (Lit("false")) { v->type = JValue::Bool; v->b = false; return true; }
        if (Lit("null")) { v->type = JValue::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            char buf[40];
            size_t n = 0;
            while (p < end && n < sizeof buf - 1 && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) buf[n++] = *p++;
            buf[n] = 0;
            char* e = nullptr;
            v->n = strtod(buf, &e);
            if (!n || *e) return false;
            v->type = JValue::Num;
            return true;
        }
        return false;
    }
};

}  // namespace

bool JsonParse(const char* text, size_t size, JValue* out) {
    Parser parser = {text, text + size};
    *out = JValue();
    if (!parser.Value(out, 0)) return false;
    parser.Ws();
    return parser.p == parser.end;
}

std::string JsonQuote(const std::string& s) {
    std::string r = "\"";
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); r += b; }
            else r += char(c);
        }
    }
    return r + "\"";
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const uint8_t* d, size_t n) {
    std::string r;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = uint32_t(d[i]) << 16 | (i + 1 < n ? uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        r += kB64[(v >> 18) & 63];
        r += kB64[(v >> 12) & 63];
        r += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        r += i + 2 < n ? kB64[v & 63] : '=';
    }
    return r;
}

bool Base64Decode(const std::string& t, std::vector<uint8_t>* out) {
    out->clear();
    if (t.size() % 4) return false;
    for (size_t i = 0; i < t.size(); i += 4) {
        uint32_t v = 0;
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t[i + size_t(k)];
            int x;
            if (c >= 'A' && c <= 'Z') x = c - 'A';
            else if (c >= 'a' && c <= 'z') x = c - 'a' + 26;
            else if (c >= '0' && c <= '9') x = c - '0' + 52;
            else if (c == '+') x = 62;
            else if (c == '/') x = 63;
            else if (c == '=' && k >= 2 && i + 4 == t.size()) { x = 0; ++pad; }
            else return false;
            if (pad && c != '=') return false;
            v = v << 6 | uint32_t(x);
        }
        out->push_back(uint8_t(v >> 16));
        if (pad < 2) out->push_back(uint8_t(v >> 8));
        if (pad < 1) out->push_back(uint8_t(v));
    }
    return true;
}

}  // namespace NpOnline
