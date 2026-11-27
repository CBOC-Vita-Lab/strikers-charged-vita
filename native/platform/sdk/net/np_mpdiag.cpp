// Always-on multiplayer diagnostics: see np_mpdiag.h. The scrubber follows Galaxy 3.0's mp_scrub (platform/src/mp/mp_report.cpp): it works on text it is given, it
// does not know what is secret, it knows what secrets look like; and the report is built from typed events that carry no such text by design.
#include "np_mpdiag.h"

#include <atomic>
#include <new>
#include <string>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "online/np_app_version.h"
#include "../save/np_safe_file.h"

#ifdef __vita__
#include "../../vita/src/np_build_identity.h"
#endif

extern "C" unsigned NpLanFingerprint() __attribute__((weak));   // np_lan.cpp (absent in the small host tests)
// (mpbeta27) the transport's figures for the report's [network] and [lockstep] lines: weak, the small host tests link np_mpdiag.cpp alone
extern "C" int NpNetRttMs(void) __attribute__((weak));
extern "C" int NpNetJitterMs(void) __attribute__((weak));
extern "C" int NpNetLossPermille(void) __attribute__((weak));
extern "C" int NpNetUsingRelay(void) __attribute__((weak));
extern "C" int NpNetNat(void) __attribute__((weak));
extern "C" int NpNetActive(void) __attribute__((weak));
extern "C" int NpLanKeepAliveLimitMs(void) __attribute__((weak));   // np_lan.cpp: the link's peer-lost limit in force (LAN 20 s, online MSC_NET_PEER_LOST_MS)
extern "C" int NpLanAgreedDelayTicks(void) __attribute__((weak));
extern "C" int NpNetPeerLostMs(void) __attribute__((weak));   // vlink_online.cpp: the online transport's partner-lost limit

namespace NpMpDiag {

const char* const kReportFile = "mp-report.txt";

// ---- the ring -------------------------------------------------------------------------------------------------------------------------------
void Ring::Clear() {
    memset(ev_, 0, sizeof ev_);
    head_ = 0;
    count_ = 0;
    total_ = suppressed_ = 0;
    memset(kindCount_, 0, sizeof kindCount_);
}

void Ring::SessionBegin() { memset(kindCount_, 0, sizeof kindCount_); }

bool Ring::Push(uint32_t ms, Kind kind, int a, int b, int c, const char* text) {
    ++total_;
    if (kind >= kKindCount) kind = kNote;
    if (kindCount_[kind] >= kPerKindCap) {
        ++suppressed_;
        return false;
    }
    ++kindCount_[kind];
    Event& e = ev_[head_];
    e.ms = ms;
    e.kind = uint8_t(kind);
    e.reserved[0] = e.reserved[1] = e.reserved[2] = 0;
    e.a = a;
    e.b = b;
    e.c = c;
    size_t i = 0;
    if (text)
        for (; text[i] && i < size_t(kTextBytes) - 1; ++i) {
            const unsigned char ch = static_cast<unsigned char>(text[i]);
            e.text[i] = (ch < 0x20 || ch >= 0x7F) ? '?' : char(ch);
        }
    for (; i < size_t(kTextBytes); ++i) e.text[i] = 0;
    head_ = (head_ + 1) % kRingSize;
    if (count_ < kRingSize) ++count_;
    return true;
}

namespace {

Ring g_ring;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
char g_connTest[256];
int g_offer = 0;          // 0 nothing to offer, 1 an error came up in this session (offer a report), 2 offered for this session
int g_loss = 0;           // the worst link's loss at the last match end, per mille
char g_logTail[kLogTailBytes];
size_t g_logLen = 0, g_logHead = 0;   // bytes kept (at most the buffer), next write position

// (mpbeta27) a mismatch is always followed by its kDesync; a crash in the last run's session arms the offer too (the result upload arms it itself, only when it is dropped)
bool IsErrorKind(Kind k) { return k == kDesync || k == kConnectionLost || k == kLinkLost || k == kOnlineFail || k == kLastRunCrashed; }

struct Lock {
    Lock() { while (g_lock.test_and_set(std::memory_order_acquire)) { /* a few stores: spin */ } }
    ~Lock() { g_lock.clear(std::memory_order_release); }
};

uint32_t NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static const int64_t base = int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    const int64_t now = int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    return uint32_t(now - base);
}

}  // namespace

void Note(Kind kind, int a, int b, int c, const char* text) {
    const uint32_t ms = NowMs();
    Lock lock;
    if (kind == kSessionStart) { g_ring.SessionBegin(); g_offer = 0; }   // the next session arms the offer again
    if (IsErrorKind(kind) && g_offer == 0) g_offer = 1;
    g_ring.Push(ms, kind, a, b, c, text);
}

void NoteGameLog(const char* format, va_list args) {
    if (!format) return;
    if (!strncmp(format, "DESYNC", 6)) {
        const int what = strstr(format, "frame") ? 1 : strstr(format, "checksum") ? 2 : strstr(format, "random seed") ? 3 : 0;
        const int tick = va_arg(args, int);
        Note(kDesync, tick, what, 0, nullptr);
    } else if (!strncmp(format, "connection lost", 15)) {
        const unsigned connection = va_arg(args, unsigned);
        const int reason = va_arg(args, int);
        Note(kConnectionLost, int(connection), reason, 0, nullptr);
    } else if (!strncmp(format, "sync error reported", 19)) {
        Note(kNote, 0, 0, 0, "sync error shown to the player");
    } else if (!strncmp(format, "input queue overflow", 20)) {
        Note(kNote, 0, 0, 0, "input queue overflow");
    }
}

void ClearForTest() {
    Lock lock;
    g_ring.Clear();
    g_connTest[0] = 0;
    g_offer = 0;
    g_loss = 0;
}
int Events() {
    Lock lock;
    return g_ring.Count();
}
uint32_t TotalEvents() {
    Lock lock;
    return g_ring.Total();
}

void SetLastConnTest(const char* text) {
    Lock lock;
    snprintf(g_connTest, sizeof g_connTest, "%s", text ? text : "");
}

// ---- the scrubber ---------------------------------------------------------------------------------------------------------------------------
namespace {

struct Out {
    char* p;
    size_t cap;   // bytes available including the terminator
    size_t n;
    void put(char c) { if (n + 1 < cap) p[n++] = c; }
    void puts(const char* s) { while (*s) put(*s++); }
    void putn(const char* s, size_t k) { for (size_t i = 0; i < k; i++) put(s[i]); }
};

bool IsWordChar(unsigned char c) { return isalnum(c) || c == '_'; }
bool IsTokenChar(unsigned char c) { return isalnum(c) || c == ':' || c == '.' || c == '_' || c == '-' || c == '[' || c == ']'; }
bool IsHex(unsigned char c) { return isxdigit(c) != 0; }

// The output so far ends with `word` and a space ("... joining "): the next token is what that word introduces.
bool EndsWithWord(const Out& o, const char* word) {
    const size_t l = strlen(word);
    if (o.n < l + 1 || o.p[o.n - 1] != ' ' || strncasecmp(o.p + o.n - 1 - l, word, l) != 0) return false;
    return o.n == l + 1 || !IsWordChar(static_cast<unsigned char>(o.p[o.n - 2 - l]));
}

bool Starts(const char* s, size_t left, const char* lit) {
    const size_t k = strlen(lit);
    return left >= k && memcmp(s, lit, k) == 0;
}

// An invite code is 6 characters of the menu's alphabet (np_menu_flow.cpp kAlphabet: no I L O 0 1); a friend code XXXX-XXXX. A plain run is too common to hide anywhere,
// so it is hidden after a key or the words "joining" / "room" / "friend" / "code" / "invite"; the hyphenated form is recognised anywhere.
bool CodeChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '9'); }
bool LooksLikeFriendCode(const char* s, size_t n) {
    if (n != 9 || s[4] != '-') return false;
    for (size_t i = 0; i < n; i++)
        if (i != 4 && !CodeChar(s[i])) return false;
    return true;
}
bool LooksLikeCode(const char* s, size_t n) {
    if (LooksLikeFriendCode(s, n)) return true;
    if (n < 6 || n > 8) return false;
    for (size_t i = 0; i < n; i++)
        if (!CodeChar(s[i])) return false;
    return true;
}
// What `code=` may show: a lower-case error word ("room_full"). Everything else is hidden: a code format that changes must fail closed.
bool ErrorCodeWord(const char* s, size_t n) {
    if (n == 0 || n > 40 || !(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return false;
    return true;
}

// a.b.c.d with optional :port, also with wildcard parts (x, X, *) when at least three parts are written: the log's own shortened public addresses ("81.12.x.x:41100")
// are addresses too, the first two octets already narrow a person down.
bool IsIpv4Token(const char* s, size_t n) {
    size_t i = 0;
    int parts = 0, wild = 0, digitsParts = 0;
    for (;;) {
        if (i < n && (s[i] == 'x' || s[i] == 'X' || s[i] == '*')) { ++i; ++wild; }
        else {
            size_t d = 0;
            while (i < n && isdigit(static_cast<unsigned char>(s[i]))) { ++i; ++d; }
            if (d < 1 || d > 3) return false;
            ++digitsParts;
        }
        ++parts;
        if (parts == 4) break;
        if (i >= n || s[i] != '.') {
            if (wild && parts >= 3 && digitsParts >= 1) break;
            return false;
        }
        ++i;
    }
    if (parts < 4 && !(wild && parts >= 3)) return false;
    if (wild == 0 && parts != 4) return false;
    if (i == n) return true;
    if (s[i] != ':') return false;
    ++i;
    size_t d = 0;
    while (i < n && isdigit(static_cast<unsigned char>(s[i]))) { ++i; ++d; }
    return d >= 1 && d <= 5 && i == n;
}

// IPv6 and MAC-like: hex digits and colons (dots for a v4 tail), "::" present or at least 5 colons. A clock time (12:34:56) has 2 colons.
bool IsIpv6Token(const char* s, size_t n) {
    int colons = 0;
    bool dbl = false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ':') { ++colons; if (i + 1 < n && s[i + 1] == ':') dbl = true; }
        else if (!IsHex(static_cast<unsigned char>(s[i])) && s[i] != '.') return false;
    }
    return colons >= 2 && (dbl || colons >= 5);
}

bool AllHex(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!IsHex(static_cast<unsigned char>(s[i]))) return false;
    return n > 0;
}

// A long random-looking token (a bearer token is 43 base64url characters, a console key 32 hex digits): letters and digits, 32 or more, only [A-Za-z0-9_-].
bool IsLongToken(const char* s, size_t n) {
    if (n < 32) return false;
    bool letter = false, digit = false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (isalpha(c)) letter = true;
        else if (isdigit(c)) digit = true;
        else if (c != '_' && c != '-') return false;
    }
    return letter && digit;
}

bool IsBlobChar(unsigned char c) { return isalnum(c) || c == '+' || c == '/' || c == '=' || c == '_' || c == '-'; }

// The length of a base64 / base64url / hex blob that starts at s[0] (0 when it is not one): 32 or more of [A-Za-z0-9+/=_-] with a letter and a digit; at most two
// slashes (a path of four folders is not a secret).
size_t BlobLen(const char* s, size_t n) {
    size_t k = 0;
    int slashes = 0;
    bool letter = false, digit = false;
    while (k < n && IsBlobChar(static_cast<unsigned char>(s[k]))) {
        if (s[k] == '/') ++slashes;
        if (isalpha(static_cast<unsigned char>(s[k]))) letter = true;
        if (isdigit(static_cast<unsigned char>(s[k]))) digit = true;
        ++k;
    }
    if (k < 32 || slashes > 2 || !letter || !digit) return 0;
    return k;
}

// A host name without a scheme ("nas.lan:8443"): dotted labels whose last label is a known top-level domain. The list is short and common: file names (np-player.txt),
// versions (1.3.0) and dotted words are never matched.
bool IsHostname(const char* s, size_t n) {
    static const char* const kTld[] = {"com", "net", "org", "io", "dev", "app", "gg", "tv", "me", "xyz", "info", "biz", "co", "uk", "de", "fr", "jp", "us",
                                       "ca", "au", "nl", "eu", "ru", "cn", "kr", "br", "it", "es", "se", "no", "fi", "pl", "ch", "at", "be", "cz", "nz",
                                       "local", "lan", "home", "internal", "localdomain", "cloud", "online", "site", "tech", "link", "page", "workers",
                                       "pages", "run", "sh", "ws", nullptr};
    size_t e = n;
    size_t pe = e;
    while (pe > 0 && isdigit(static_cast<unsigned char>(s[pe - 1]))) --pe;   // ":port"
    if (pe < e && pe > 0 && s[pe - 1] == ':' && e - pe <= 5) e = pe - 1;
    if (e < 4) return false;
    int labels = 0;
    size_t ls = 0;
    for (size_t i = 0; i <= e; i++) {
        if (i == e || s[i] == '.') {
            const size_t l = i - ls;
            if (l < 1 || l > 63 || s[ls] == '-' || s[i - 1] == '-') return false;
            ++labels;
            if (i < e) { ls = i + 1; continue; }
            if (labels < 2 || l < 2) return false;
            for (size_t q = ls; q < e; q++)
                if (!isalpha(static_cast<unsigned char>(s[q]))) return false;
            for (int t = 0; kTld[t]; t++)
                if (strlen(kTld[t]) == l && strncasecmp(s + ls, kTld[t], l) == 0) return true;
            return false;
        }
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!(isalnum(c) || c == '-')) return false;
    }
    return false;
}

// Words that hide their value, whatever the spelling (key=value, key = value, key: value, "key":"value"; any case). Class A: the value goes unless it is empty or "-".
// Class B: a person's or a machine's name may stand there, but a small number (a slot, a count) is kept: "host=1" stays, "host=nas.lan" goes. `code` is special: only a
// lower-case error word survives. For "name" the value runs to the next " key=" of the known set (or the end of the line): a player's name may contain spaces.
const char* const kHideKeys[] = {"name", "nick", "nickname", "onlinename", "console", "friend", "room", "invite", "token", "key", "console_key", "secret", "pass", "password",
                                 "account", "profile", "pid", "uid", "id", "email", "mac", "ip", "addr", "address", nullptr};
const char* const kColonKeys[] = {"name", "nick", "console", "friend", "room", "invite", "token", "key", "console_key", "secret", "password", "account", "profile", "email", "code", nullptr};
const char* const kNumKeys[] = {"host", "peer", "player", "user", "ssid", "owner", nullptr};
const char* const kNameStops[] = {"mode", "link", "slot", "tick", "step", "reason", "conn", "connection", "build", "fingerprint", "peers", "machines", "delay", nullptr};

bool KeyIn(const char* const* list, const char* k, size_t kl) {
    for (int h = 0; list[h]; h++)
        if (strlen(list[h]) == kl && strncasecmp(k, list[h], kl) == 0) return true;
    return false;
}

size_t NameValueLen(const char* s, size_t left) {
    for (size_t i = 0; i < left; i++) {
        if (s[i] != ' ') continue;
        for (int k = 0; kNameStops[k]; k++) {
            const size_t l = strlen(kNameStops[k]);
            if (Starts(s + i + 1, left - i - 1, kNameStops[k]) && i + 1 + l < left && s[i + 1 + l] == '=') return i;
        }
    }
    return left;
}

// A hidden-key pair at s[i]: key, optional closing quote, separator '=' or ':', value. Returns the new position (0: not one).
size_t ScrubKeyValue(const char* s, size_t n, size_t i, Out& o) {
    const bool quoted = s[i] == '"';   // JSON: "key": "value"
    const size_t ks = quoted ? i + 1 : i;
    if (ks >= n || !isalpha(static_cast<unsigned char>(s[ks]))) return 0;
    size_t k = ks;
    while (k < n && IsWordChar(static_cast<unsigned char>(s[k]))) ++k;
    size_t q = k;
    if (quoted) {
        if (q >= n || s[q] != '"') return 0;
        ++q;
    }
    size_t eq = q;
    while (eq < n && s[eq] == ' ') ++eq;
    if (eq >= n || (s[eq] != '=' && s[eq] != ':')) return 0;
    const bool colon = s[eq] == ':';
    if (colon && eq + 1 < n && s[eq + 1] == ':') return 0;   // "::"
    const size_t kl = k - ks;
    const bool isCode = kl == 4 && strncasecmp(s + ks, "code", 4) == 0;
    const bool classA = KeyIn(kHideKeys, s + ks, kl) || isCode;
    const bool classB = KeyIn(kNumKeys, s + ks, kl);
    if (!classA && !classB) return 0;
    if (colon && !quoted && !KeyIn(kColonKeys, s + ks, kl)) return 0;   // (a plain "host: x" is ordinary text)
    const bool isName = kl == 4 && strncasecmp(s + ks, "name", 4) == 0;
    size_t vs = eq + 1;
    if (colon || eq != q) while (vs < n && s[vs] == ' ') ++vs;   // "key: v" and "key = v"
    o.putn(s + i, vs - i);
    size_t vb = vs, ve = vs;
    const bool jq = vs < n && s[vs] == '"';
    if (jq) {
        vb = vs + 1;
        ve = vb;
        while (ve < n && s[ve] != '"') ve += (s[ve] == '\\' && ve + 1 < n) ? 2 : 1;
        if (ve > n) ve = n;
    } else {
        if (isName) ve = vs + NameValueLen(s + vs, n - vs);
        else while (ve < n && s[ve] != ' ' && s[ve] != ',' && s[ve] != ')' && s[ve] != '\t' && s[ve] != '}' && s[ve] != ']' && s[ve] != '"') ++ve;
        if (classB && !classA) {   // a name that may hold spaces, unless it is a small number
            bool num = ve - vs <= 3;
            for (size_t t = vs; t < ve && num; t++) if (!isdigit(static_cast<unsigned char>(s[t]))) num = false;
            if (!num && ve - vs > 0) ve = vs + NameValueLen(s + vs, n - vs);
        }
    }
    const size_t vl = ve - vb;
    bool keep = vl == 0 || (vl == 1 && s[vb] == '-');
    if (isCode && ErrorCodeWord(s + vb, vl)) keep = true;
    if (classB && !classA && vl <= 3) {
        bool num = true;
        for (size_t t = 0; t < vl; t++) if (!isdigit(static_cast<unsigned char>(s[vb + t]))) num = false;
        if (num) keep = true;
    }
    if (jq) o.put('"');
    if (keep) o.putn(s + vb, vl);
    else o.puts("<hidden>");
    if (jq) {
        if (ve < n) o.put('"');
        return ve < n ? ve + 1 : n;
    }
    return ve;
}

// A token's class: the replacement text or null.
const char* ClassifyToken(const char* core, size_t cl, const Out& o) {
    if (cl == 0) return nullptr;
    if (LooksLikeFriendCode(core, cl)) return "<code>";
    if (LooksLikeCode(core, cl) && (EndsWithWord(o, "joining") || EndsWithWord(o, "room") || EndsWithWord(o, "friend") || EndsWithWord(o, "code") || EndsWithWord(o, "invite")))
        return "<code>";
    if (IsIpv4Token(core, cl) || IsIpv6Token(core, cl)) return "<address>";
    if (cl >= 32 && AllHex(core, cl)) return "<secret>";
    if (cl >= 10 && cl <= 14 && AllHex(core, 8) && core[8] == ':') {   // "0a000105:41100": the core's hex form of host:port
        bool digits = true;
        for (size_t q = 9; q < cl; q++) if (!isdigit(static_cast<unsigned char>(core[q]))) digits = false;
        if (digits) return "<address>";
    }
    if (cl == 8 && AllHex(core, cl) && o.n >= 8 && memcmp(o.p + o.n - 8, "address ", 8) == 0) return "<address>";
    if (cl >= 1 && (EndsWithWord(o, "Bearer") || EndsWithWord(o, "bearer"))) return "<secret>";
    // (mpbeta27) an account number after its word, without '=' ("login ok: profile 100003", "pid 4242"): the service's ids have 4 digits or more
    if (cl >= 4 && cl <= 12 && (EndsWithWord(o, "profile") || EndsWithWord(o, "pid") || EndsWithWord(o, "account") || EndsWithWord(o, "uid") || EndsWithWord(o, "player"))) {
        bool digits = true;
        for (size_t q = 0; q < cl && digits; q++) digits = isdigit(static_cast<unsigned char>(core[q])) != 0;
        if (digits) return "<id>";
    }
    if (IsLongToken(core, cl)) return "<secret>";
    if (IsHostname(core, cl)) return "<host>";
    return nullptr;
}

void ScrubLine(const char* s, size_t n, Out& o) {
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const bool wordStart = i == 0 || !IsWordChar(static_cast<unsigned char>(s[i - 1]));
        // 1. key=value, key: value, "key":"value"
        if (wordStart && (isalpha(c) || c == '"')) {
            const size_t r = ScrubKeyValue(s, n, i, o);
            if (r) { i = r; continue; }
        }
        // 2. URLs: the host is hidden, the rest of the token too (paths may carry room codes)
        if (c == 'h' && (Starts(s + i, n - i, "https://") || Starts(s + i, n - i, "http://"))) {
            const size_t sl = s[i + 4] == 's' ? 8 : 7;
            o.putn(s + i, sl);
            o.puts("<host>");
            i += sl;
            while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != ')' && s[i] != ',' && s[i] != '"') ++i;
            continue;
        }
        // 3. the user's folder name
        if (c == '/' || c == 'C') {
            static const char* const kHomes[] = {"/home/", "/Users/", "C:\\Users\\", "C:/Users/", nullptr};
            bool done = false;
            for (int h = 0; kHomes[h] && !done; h++) {
                if (Starts(s + i, n - i, kHomes[h])) {
                    const size_t hl = strlen(kHomes[h]);
                    o.putn(s + i, hl);
                    o.puts("<user>");
                    i += hl;
                    while (i < n && s[i] != '/' && s[i] != '\\' && s[i] != ' ' && s[i] != '\t' && s[i] != '"') ++i;
                    done = true;
                }
            }
            if (done) continue;
        }
        // 4a. "[addr]" and "[addr]:port"
        if (c == '[') {
            size_t e = i + 1;
            while (e < n && s[e] != ']' && s[e] != ' ' && e - i < 48) ++e;
            if (e < n && s[e] == ']' && (IsIpv6Token(s + i + 1, e - i - 1) || IsIpv4Token(s + i + 1, e - i - 1) || IsHostname(s + i + 1, e - i - 1) || LooksLikeFriendCode(s + i + 1, e - i - 1))) {
                ++e;
                if (e < n && s[e] == ':') {
                    size_t d = e + 1;
                    while (d < n && isdigit(static_cast<unsigned char>(s[d]))) ++d;
                    if (d > e + 1) e = d;
                }
                o.puts("<address>");
                i = e;
                continue;
            }
        }
        if (c == '[') { o.put('['); ++i; continue; }   // (not an address: the text inside is looked at on its own)
        // 3b. base64 / hex blobs of token length (they use '+' '/' '=' that tokens do not)
        if (IsBlobChar(c) && (i == 0 || !IsBlobChar(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '/')) {   // (also after each '/': a blob inside a path)
            const size_t bl = BlobLen(s + i, n - i);
            if (bl) { o.puts("<secret>"); i += bl; continue; }
        }
        // 4. tokens: addresses, ids, secrets
        if (IsTokenChar(c)) {
            size_t k = i;
            while (k < n && IsTokenChar(static_cast<unsigned char>(s[k]))) ++k;
            const char* t = s + i;
            size_t tl = k - i;
            const char* core = t;
            size_t cl = tl;
            if (cl && core[0] == '[') { ++core; --cl; }
            while (cl && (core[cl - 1] == '.' || core[cl - 1] == ':' || core[cl - 1] == '-' || core[cl - 1] == ']')) --cl;   // a sentence's full stop or colon after the token
            const size_t trail = tl - (static_cast<size_t>(core - t) + cl);
            const char* repl = ClassifyToken(core, cl, o);
            size_t lead = 0;   // text kept in front of the hidden part: "peer:1.2.3.4:41100", "ip-10.0.0.5"
            if (!repl && cl <= 90) {
                for (size_t j = 1; j < cl && !repl; j++) {
                    if (core[j - 1] != ':' && core[j - 1] != '=' && core[j - 1] != '-' && core[j - 1] != '_') continue;
                    repl = ClassifyToken(core + j, cl - j, o);
                    if (repl) lead = j;
                }
            }
            // a blob glued to a key: "peer:dGhpcy+pcyBh..." (its '+' and '/' end the token above)
            bool found = false;
            if (!repl) {
                for (size_t j = 1; j < tl && !found; j++) {
                    if (t[j - 1] != ':' && t[j - 1] != '=') continue;
                    const size_t bl = BlobLen(t + j, n - (i + j));
                    if (bl) {
                        o.putn(t, j);
                        o.puts("<secret>");
                        i += j + bl;
                        found = true;
                        break;
                    }
                }
            }
            if (found) continue;
            // a code that is a path segment: ".../friends/KQ7M2X"
            if (!repl && i > 0 && s[i - 1] == '/' && LooksLikeCode(core, cl)) repl = "<code>";
            if (repl) {
                o.putn(t, static_cast<size_t>(core - t) + lead);
                o.puts(repl);
                o.putn(t + (tl - trail), trail);
            } else {
                o.putn(t, tl);
            }
            i = k;
            continue;
        }
        o.put(static_cast<char>(c));
        ++i;
    }
}

}  // namespace

size_t Scrub(const char* in, size_t n, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    Out o{out, cap, 0};
#ifdef NP_MPDIAG_SCRUB_OFF   // the mutant of tests/test_mpdiag.py: the scrubber does nothing, and the tests must notice
    if (in) o.putn(in, n);
#else
    if (in) {
        size_t i = 0;
        while (i < n) {
            size_t e = i;
            while (e < n && in[e] != '\n') ++e;
            ScrubLine(in + i, e - i, o);
            if (e < n) o.put('\n');
            i = e + 1;
        }
    }
#endif
    out[o.n] = 0;
    return o.n;
}

// ---- the report -----------------------------------------------------------------------------------------------------------------------------
namespace {

// One line of the report: scrubbed, then appended (a line is at most 300 characters).
void AddLine(Out& o, const char* raw) {
    char tmp[400];
    const size_t l = Scrub(raw, strlen(raw), tmp, sizeof tmp);
    o.putn(tmp, l);
    o.put('\n');
}

const char* SessionLinkName(int a) { return a == 1 ? "online" : "local (Nearby or Same Wi-Fi)"; }

void Describe(const Event& e, char* out, size_t cap) {
    switch (e.kind) {
    case kSessionStart:
        if (e.b > 0) snprintf(out, cap, "session start: %s, %d machines", SessionLinkName(e.a), e.b);
        else snprintf(out, cap, "session start: %s", SessionLinkName(e.a));
        break;
    case kPeers: snprintf(out, cap, "draft: %d consoles (%d partner%s)", e.a, e.a - 1, e.a - 1 == 1 ? "" : "s"); break;
    case kInputDelay:
        if (e.c == 1) snprintf(out, cap, "input delay: %d ticks, chosen by AUTO from a %d ms round trip", e.a, e.b);
        else snprintf(out, cap, "input delay: %d ticks (fixed, or the host's choice)", e.a);
        break;
    case kDesync: snprintf(out, cap, "DESYNC at step %d: %s differs", e.a, e.b == 1 ? "the frame number" : e.b == 2 ? "the checksum" : e.b == 3 ? "the random seed" : "a value"); break;
    case kConnectionLost: snprintf(out, cap, "connection lost (connection %d, reason %d)", e.a, e.b); break;
    case kLinkLost: snprintf(out, cap, "link lost: %s", e.text); break;
    case kMatchEnd:
        snprintf(out, cap, "match end: %d steps, %d starved (%d.%d%%), round trip %d ms%s%s", e.a, e.b, e.a + e.b > 0 ? int((1000LL * e.b / (e.a + e.b)) / 10) : 0,
                 e.a + e.b > 0 ? int((1000LL * e.b / (e.a + e.b)) % 10) : 0, e.c, e.text[0] ? "; last digests: " : "", e.text);
        break;
    case kSessionEnd: snprintf(out, cap, "session end: this console left the multiplayer session"); break;
    case kSuspend: snprintf(out, cap, "the console went to sleep inside a session"); break;
    case kResume: snprintf(out, cap, "the console woke up inside a session%s", e.a ? " (the link was closed)" : ""); break;
    case kOnlineFail: snprintf(out, cap, "online menu: %s", e.text); break;
    case kConnTest: snprintf(out, cap, "connection test: %s", e.text); break;
    case kStage: snprintf(out, cap, "stage: %s (%s, %s)", StageName(Stage(e.a)), NpMpDiag::LinkName(Link(e.b)), PlayName(Play(e.c))); break;
    case kError:
        if (e.b) snprintf(out, cap, "error in %s: %s, status %d", StageName(Stage(e.a)), e.text[0] ? e.text : "-", e.b);
        else snprintf(out, cap, "error in %s: %s", StageName(Stage(e.a)), e.text[0] ? e.text : "-");
        break;
    case kMismatch: snprintf(out, cap, "mismatch at step %d: %s, %s", e.a, e.b == 1 ? "frame" : e.b == 2 ? "checksum" : e.b == 3 ? "seed" : "value", e.text); break;
    case kResultUpload:
        snprintf(out, cap, "ranked result upload: %s after %d attempt%s%s%s", e.b == 1 ? "accepted" : e.b == 0 ? "failed, kept for a retry" : "dropped", e.a, e.a == 1 ? "" : "s",
                 e.text[0] ? ": " : "", e.text);
        break;
    case kPeerBuild: snprintf(out, cap, "partner build: %s (%s)", e.a ? "the same as this one" : "ANOTHER build or rules, refused", e.text); break;
    case kLastRunCrashed: snprintf(out, cap, "the last run of the game ended inside a multiplayer session (stage %s) without leaving it", e.text); break;
    default: snprintf(out, cap, "%s", e.text); break;
    }
}

}  // namespace

size_t BuildReport(char* out, size_t cap, const ReportParts& p) {
    if (!out || cap == 0) return 0;
    Out o{out, cap, 0};
    char line[400];
    AddLine(o, "Mario Strikers Charged for PS Vita: multiplayer report");
    AddLine(o, "Nothing here names you or your network: addresses, names, ids, codes, tokens and keys are removed.");
    AddLine(o, "");
    AddLine(o, "[build]");
    snprintf(line, sizeof line, "version = %s", p.version && *p.version ? p.version : "-");
    AddLine(o, line);
    snprintf(line, sizeof line, "build = %s", p.buildId && *p.buildId ? p.buildId : "-");
    AddLine(o, line);
    snprintf(line, sizeof line, "fingerprint = %08x", p.fingerprint);
    AddLine(o, line);
    AddLine(o, "");
    AddLine(o, "[settings]");
    if (p.settings && *p.settings) {
        const char* s = p.settings;
        while (*s) {
            const char* e = strchr(s, '\n');
            const size_t l = e ? size_t(e - s) : strlen(s);
            snprintf(line, sizeof line, "%.*s", int(l < 300 ? l : 300), s);
            AddLine(o, line);
            s += l + (e ? 1 : 0);
        }
    } else {
        AddLine(o, "(none set: every multiplayer setting is at its default)");
    }
    AddLine(o, "");
    AddLine(o, "[connection test]");
    if (p.connTest && *p.connTest) {
        const char* s = p.connTest;
        while (*s) {
            const char* e = strchr(s, '\n');
            const size_t l = e ? size_t(e - s) : strlen(s);
            snprintf(line, sizeof line, "%.*s", int(l < 300 ? l : 300), s);
            AddLine(o, line);
            s += l + (e ? 1 : 0);
        }
    } else {
        AddLine(o, "none run since the game started");
    }
    if (p.trail && *p.trail) {   // (mpbeta27) [session] [network] [lockstep] [peers]: built from typed numbers, scrubbed here like every other line
        AddLine(o, "");
        const char* s = p.trail;
        while (*s) {
            const char* e = strchr(s, '\n');
            const size_t l = e ? size_t(e - s) : strlen(s);
            snprintf(line, sizeof line, "%.*s", int(l < 300 ? l : 300), s);
#ifdef NP_MPDIAG_MUT_TRAIL_NOSCRUB   // mutant of tests/test_mpbeta.py: the trail's lines are not scrubbed
            o.puts(line);
            o.put('\n');
#else
            AddLine(o, line);
#endif
            s += l + (e ? 1 : 0);
        }
    }
    AddLine(o, "");
    AddLine(o, "[events] (seconds since the game started, newest last)");
    const Ring* r = p.ring;
    int first = 0;
    const int count = r ? r->Count() : 0;
    // when the report would not fit the oldest events go first; the room kept for the tail is what the footer needs
    char text[160];
    size_t total = 0;
    int lens[kRingSize];
    for (int i = 0; i < count; i++) {
        const Event& e = r->At(i);
        Describe(e, text, sizeof text);
        lens[i] = snprintf(line, sizeof line, "T+%4u.%01us  %s", e.ms / 1000u, (e.ms % 1000u) / 100u, text);
        total += size_t(lens[i] < 0 ? 0 : (lens[i] > 399 ? 399 : lens[i])) + 1;
    }
    const size_t room = o.cap > o.n + 400 ? o.cap - o.n - 400 : 0;
    while (first < count && total > room) {
        total -= size_t(lens[first] < 0 ? 0 : (lens[first] > 399 ? 399 : lens[first])) + 1;
        ++first;
    }
    if (count == 0) AddLine(o, "no multiplayer events since the game started");
    if (first > 0) {
        snprintf(line, sizeof line, "(%d older events did not fit)", first);
        AddLine(o, line);
    }
    for (int i = first; i < count; i++) {
        const Event& e = r->At(i);
        Describe(e, text, sizeof text);
        snprintf(line, sizeof line, "T+%4u.%01us  %s", e.ms / 1000u, (e.ms % 1000u) / 100u, text);
        AddLine(o, line);
    }
    if (r && (r->Suppressed() || r->Overwritten())) {
        snprintf(line, sizeof line, "(%u repeated events were only counted, %u older ones were overwritten)", unsigned(r->Suppressed()), unsigned(r->Overwritten()));
        AddLine(o, line);
    }
    snprintf(line, sizeof line, "report written at T+%4u.%01us", p.nowMs / 1000u, (p.nowMs % 1000u) / 100u);
    AddLine(o, line);
    out[o.n] = 0;
    return o.n;
}

size_t BuildCurrentReport(char* out, size_t outCap) {
    if (!out || outCap == 0) return 0;
    // the settings: a fixed allow-list of keys (no name, no code, no address ever)
    static const char* const kKeys[] = {"MSC_LAN_MODE", "MSC_LAN_DIALOG", "MSC_LAN_INPUT_DELAY", "MSC_NET_RELAY_ONLY", "MSC_LAN_KEEPALIVE_MS", "MSC_LAN_DIGEST_EXT", nullptr};
    char settings[400];
    size_t sn = 0;
    settings[0] = 0;
    for (int i = 0; kKeys[i]; ++i) {
        const char* v = getenv(kKeys[i]);
        if (!v || !*v) continue;
        const int w = snprintf(settings + sn, sizeof settings - sn, "%s = %.40s\n", kKeys[i], v);
        if (w > 0 && sn + size_t(w) < sizeof settings) sn += size_t(w);
    }
    // the ring is copied under the lock (20 KiB, a memcpy) so that the report is built without holding it
    Ring* snapshot = new (std::nothrow) Ring;
    if (!snapshot) {
        out[0] = 0;
        return 0;
    }
    char connTest[256];
    uint32_t nowMs;
    {
        Lock lock;
        *snapshot = g_ring;
        snprintf(connTest, sizeof connTest, "%s", g_connTest);
        nowMs = NowMs();
    }
    ReportParts parts;
    memset(&parts, 0, sizeof parts);
    parts.version = NpNet::BuiltAppVersion();
#ifdef __vita__
    parts.buildId = NpBuildIdentity::BuildId();
#else
    parts.buildId = "host";
#endif
    parts.fingerprint = NpLanFingerprint ? NpLanFingerprint() : 0;
    parts.settings = settings;
    parts.ring = snapshot;
    parts.nowMs = nowMs;
    parts.connTest = connTest;
    char* trail = ReportPlusOn() ? static_cast<char*>(malloc(4096)) : nullptr;   // (mpbeta27) the session trail's sections
    if (trail) {
        trail[0] = 0;
        LinkFigures f;
        f.rttMs = NpNetRttMs ? NpNetRttMs() : -1;
        f.jitterMs = NpNetJitterMs ? NpNetJitterMs() : -1;
        f.lossPermille = NpNetLossPermille ? NpNetLossPermille() : -1;
        f.relay = NpNetUsingRelay ? NpNetUsingRelay() : -1;
        f.nat = NpNetNat ? NpNetNat() : 0;
        f.keepAliveMs = NpLanKeepAliveLimitMs ? NpLanKeepAliveLimitMs() : -1;
        f.peerLostMs = NpNetPeerLostMs ? NpNetPeerLostMs() : -1;
        f.delayTicks = NpLanAgreedDelayTicks ? NpLanAgreedDelayTicks() : 0;
        const SessionNumbers sn = SessionOf(*snapshot, nowMs, 0);   // the steps and starved steps of the newest session's matches
        f.steps = sn.steps;
        f.starved = sn.starved;
        if (!f.delayTicks) f.delayTicks = sn.delayTicks;
        TrailReport(CurrentTrail(), f, trail, 4096);
        parts.trail = trail;
    }
    const size_t n = BuildReport(out, outCap, parts);
    delete snapshot;
    free(trail);
    return n;
}

bool SaveReport(const char* dir, char* path, size_t pathCap) {
    char dd[200];
    if (!dir || !*dir) {
#ifdef __vita__
        snprintf(dd, sizeof dd, "ux0:data/mscharged");
#else
        const char* d = getenv("NP_DATA_DIR");
        snprintf(dd, sizeof dd, "%s", d && *d ? d : ".");
#endif
        dir = dd;
    }
    char file[300];
    snprintf(file, sizeof file, "%s/%s", dir, kReportFile);
    const size_t outCap = 16384;
    char* out = static_cast<char*>(malloc(outCap));
    if (!out) return false;
    const size_t n = BuildCurrentReport(out, outCap);
    NpSafeFile::Options opt;
    opt.mode = 0644;
    opt.maxBytes = outCap;
    const bool ok = n > 0 && NpSafeFile::Replace(file, std::string(out, n), opt);
    free(out);
    if (!ok) return false;
    if (path && pathCap) snprintf(path, pathCap, "%s", file);
    return true;
}


// ---- the log tail ---------------------------------------------------------------------------------------------------------------------------
void NoteLogLine(const char* format, va_list args) {
    if (!format) return;
    char line[200];
    int n = vsnprintf(line, sizeof line - 1, format, args);
    if (n < 0) return;
    if (size_t(n) > sizeof line - 2) n = int(sizeof line - 2);
    for (int i = 0; i < n; ++i) {
        const unsigned char ch = static_cast<unsigned char>(line[i]);
        if (ch != '\n' && (ch < 0x20 || ch >= 0x7F)) line[i] = '?';
    }
    if (n == 0 || line[n - 1] != '\n') line[n++] = '\n';
    Lock lock;
    for (int i = 0; i < n; ++i) {
        g_logTail[g_logHead] = line[i];
        g_logHead = (g_logHead + 1) % size_t(kLogTailBytes);
    }
    g_logLen = g_logLen + size_t(n) > size_t(kLogTailBytes) ? size_t(kLogTailBytes) : g_logLen + size_t(n);
}

size_t LogTail(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    char* raw = static_cast<char*>(malloc(size_t(kLogTailBytes) + 1));
    if (!raw) { out[0] = 0; return 0; }
    size_t len;
    bool wrapped;
    {
        Lock lock;
        len = g_logLen;
        wrapped = len >= size_t(kLogTailBytes);
        const size_t start = (g_logHead + size_t(kLogTailBytes) - len) % size_t(kLogTailBytes);
        for (size_t i = 0; i < len; ++i) raw[i] = g_logTail[(start + i) % size_t(kLogTailBytes)];
    }
    size_t from = 0;
    if (wrapped) {   // the oldest line was cut in the middle: start at the next one
        while (from < len && raw[from] != '\n') ++from;
        if (from < len) ++from;
    }
    // room for the newest lines: if the scrubbed text would not fit, drop the oldest lines first
    for (;;) {
        const size_t w = Scrub(raw + from, len - from, out, cap);
        if (w + 1 < cap || from >= len) break;
        const char* nl = static_cast<const char*>(memchr(raw + from, '\n', len - from));
        if (!nl) { out[0] = 0; break; }
        from = size_t(nl - raw) + 1;
    }
    free(raw);
    return strlen(out);
}

void LogTailClearForTest() {
    Lock lock;
    g_logLen = g_logHead = 0;
}

// ---- the last session's numbers ------------------------------------------------------------------------------------------------------------
SessionNumbers SessionOf(const Ring& ring, uint32_t nowMs, int lossPermille) {
    SessionNumbers n;
    memset(&n, 0, sizeof n);
    n.rttMs = -1;
    int start = -1;
    for (int i = ring.Count() - 1; i >= 0; --i)
        if (ring.At(i).kind == kSessionStart) { start = i; break; }
    if (start < 0) return n;
    n.valid = true;
    const uint32_t t0 = ring.At(start).ms;
    uint32_t t1 = nowMs;
    for (int i = start; i < ring.Count(); ++i) {
        const Event& e = ring.At(i);
        switch (e.kind) {
        case kSessionStart: n.online = e.a == 1; break;
        case kInputDelay: n.delayTicks = e.a; if (e.b >= 0) n.rttMs = e.b; break;
        case kMatchEnd: n.steps += e.a; n.starved += e.b; n.rttMs = e.c; break;
        case kSessionEnd: n.ended = true; t1 = e.ms; break;
        case kResume: if (e.a) ++n.rebuilds; break;
        case kLinkLost: ++n.rebuilds; n.errored = true; break;
        case kConnectionLost: case kDesync: case kOnlineFail: n.errored = true; break;
        default: break;
        }
    }
    n.durationSec = t1 > t0 ? int((t1 - t0) / 1000u) : 0;
    n.lossPermille = lossPermille > 0 ? lossPermille : 0;
    return n;
}

SessionNumbers LastSession() {
    Ring* snapshot = new (std::nothrow) Ring;
    if (!snapshot) { SessionNumbers none; memset(&none, 0, sizeof none); none.rttMs = -1; return none; }
    uint32_t now;
    int loss;
    {
        Lock lock;
        *snapshot = g_ring;
        now = NowMs();
        loss = g_loss;
    }
    const SessionNumbers n = SessionOf(*snapshot, now, loss);
    delete snapshot;
    return n;
}

namespace {
int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
}

size_t SessionHeader(const SessionNumbers& n, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!n.valid) return 0;
    const int w = snprintf(out, cap, "d=%d;dl=%d;rs=%d;dr=%d;sv=%d;e=%d", Clamp(n.durationSec, 0, 86400), Clamp(n.delayTicks, 0, 30), Clamp(n.rebuilds, 0, 1000000),
                           Clamp(n.lossPermille, 0, 1000), Clamp(n.starved, 0, 10000000), n.errored ? 1 : 0);
    if (w < 0 || size_t(w) >= cap) { out[0] = 0; return 0; }
    return size_t(w);
}

void SetLoss(int permille) {
    Lock lock;
    g_loss = permille > 0 ? permille : 0;
}

bool OfferWanted() {
    Lock lock;
#ifdef NP_MPDIAG_MUT_OFFER_ALWAYS   // mutant of tests/test_mpreport.py: the offer comes again and again
    return g_offer >= 1;
#else
    return g_offer == 1;
#endif
}

void MarkOffered() {
    Lock lock;
    if (g_offer == 1) g_offer = 2;
}

const char* LastConnTest() { return g_connTest; }

// ---- the session trail (mpbeta27) ------------------------------------------------------------------------------------------------------------
const char* const kSessionMarkFile = "mp-session.txt";

const char* LinkName(Link l) {
    switch (l) {
    case kLinkNearby: return "Nearby";
    case kLinkSameWifi: return "Same Wi-Fi";
    case kLinkOnline: return "online";
    default: return "unknown link";
    }
}
const char* PlayName(Play p) {
    switch (p) {
    case kPlayRoomHost: return "friend room, hosting";
    case kPlayRoomJoin: return "friend room, joining";
    case kPlayQuick: return "quick match (unranked)";
    case kPlayRanked: return "ranked match";
    case kPlayFriends: return "friend list";
    case kPlayLocal: return "local match";
    default: return "unknown play";
    }
}
const char* StageName(Stage s) {
    static const char* const kNames[kStageCount] = {"none", "menu", "login", "queue", "match found", "signaling", "connect", "lobby", "in-match", "result", "result upload"};
    return s < kStageCount ? kNames[s] : "?";
}

namespace {

Trail g_trail;
bool g_trailInit = false;

void TrailReset(Trail& t) {
    const Link link = g_trailInit ? t.link : kLinkUnknown;
    const Play play = g_trailInit ? t.play : kPlayUnknown;
    const bool crashed = g_trailInit && t.lastRunCrashed;
    char crashedStage[sizeof t.crashedStage];
    snprintf(crashedStage, sizeof crashedStage, "%s", g_trailInit ? t.crashedStage : "");
    memset(&t, 0, sizeof t);
    t.link = link;
    t.play = play;
    t.peerSame = -1;
    t.uploadOutcome = -2;
    for (int i = 0; i < 4; ++i) t.mismatchTick[i] = -1;
    t.lastRunCrashed = crashed;
    snprintf(t.crashedStage, sizeof t.crashedStage, "%s", crashedStage);
    g_trailInit = true;
}
Trail& T() {
    if (!g_trailInit) TrailReset(g_trail);
    return g_trail;
}

int FailIndex(const char* name) {
    if (!name) return 5;
    if (!strcmp(name, "TIMEOUT")) return 0;
    if (!strncmp(name, "DNS", 3) || !strcmp(name, "RESOLVE")) return 1;
    if (!strncmp(name, "CONNECT", 7)) return 2;
    if (!strncmp(name, "TLS", 3) || !strncmp(name, "CERT", 4) || !strcmp(name, "CLOCK")) return 3;
    if (!strncmp(name, "CANCEL", 6)) return 4;
    return 5;
}

// A word from another layer (an error's name, the service's error code: "TIMEOUT", "over_budget", "room_not_found"): only its first word is taken, and only when it is
// all lower case or all upper case letters with '_' / '-' (the shape of every such code); anything else is "other". So no piece of a token, an address, an id or a name
// can reach the trail's text through it, whatever the other layer put there.
void SafeWord(const char* in, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!in || !*in) return;
    size_t len = 0;
    bool lower = false, upper = false, bad = false;
    while (in[len] && in[len] != ' ' && in[len] != ':' && in[len] != '(' && in[len] != ',') {
        const unsigned char c = static_cast<unsigned char>(in[len]);
        if (c >= 'a' && c <= 'z') lower = true;
        else if (c >= 'A' && c <= 'Z') upper = true;
        else if (c != '_' && c != '-') bad = true;
        ++len;
    }
    if (len == 0 || bad || (lower && upper) || len + 1 > cap) { snprintf(out, cap, "%s", len == 0 ? "" : "other"); return; }
    memcpy(out, in, len);
    out[len] = 0;
}

}  // namespace

bool ReportPlusOn() {
    const char* v = getenv("MSC_MP_REPORT_PLUS");
    return !(v && v[0] == '0');
}

void SetMode(Link link, Play play) {
    Lock lock;
    Trail& t = T();
    t.link = link;
    t.play = play;
}

void BeginTrail() {
    Lock lock;
    TrailReset(T());
}

void Reach(Stage s) {
    if (s >= kStageCount) return;
    Link link;
    Play play;
    {
        Lock lock;
        Trail& t = T();
        if (t.now == s) return;
        t.now = s;
        if (s > t.furthest) t.furthest = s;
        if (!t.stageMs[s]) t.stageMs[s] = NowMs() | 1u;
        link = t.link;
        play = t.play;
    }
    Note(kStage, int(s), int(link), int(play), nullptr);
}

void Error(Stage where, int code, const char* word) {
    char w[28];
    SafeWord(word, w, sizeof w);
    Stage at;
    {
        Lock lock;
        Trail& t = T();
        at = where == kStageNone ? t.now : where;
        ++t.errors;
        t.lastErrorStage = at;
        t.lastErrorCode = code;
        snprintf(t.lastErrorWord, sizeof t.lastErrorWord, "%s", w);
    }
    Note(kError, int(at), code, 0, w);
}

static void CountRequest(const char* method, const char* path, int status, const char* failName, uint32_t ms, int reused, int attempt) {
    char m[8], line[100], fw[24];
    SafeWord(method, m, sizeof m);
    SafeWord(failName, fw, sizeof fw);
    // the path is already ReqLog::SafePath (":id" for anything with a digit or a capital); it is cut to what the line holds and scrubbed again in the report
    char p[40];
    size_t n = 0;
    for (size_t i = 0; path && path[i] && n + 1 < sizeof p; ++i) {
        const unsigned char c = static_cast<unsigned char>(path[i]);
        if (isalnum(c) || c == '/' || c == '_' || c == '-' || c == ':') p[n++] = char(c);
    }
    p[n] = 0;
    if (failName) snprintf(line, sizeof line, "%s %s -> %s %ums", m, p, fw, unsigned(ms));
    else snprintf(line, sizeof line, "%s %s -> %d %ums%s", m, p, status, unsigned(ms), reused ? " reused" : "");
    Lock lock;
    Trail& t = T();
    ++t.requests;
    // a retry: the attempt number says so, or the same method and path again right after one that got no answer or a 5xx
    static char lastFailedKey[48] = "";
    char key[48];
    snprintf(key, sizeof key, "%s %s", m, p);
    if (attempt > 1 || (lastFailedKey[0] && !strcmp(key, lastFailedKey))) ++t.retries;
    if (failName || status >= 500) snprintf(lastFailedKey, sizeof lastFailedKey, "%s", key);
    else lastFailedKey[0] = 0;
    if (ms > t.slowestMs) t.slowestMs = ms;
    if (failName) {
        ++t.failed;
        ++t.failByName[FailIndex(failName)];
        snprintf(t.lastFail, sizeof t.lastFail, "%s", fw);
    } else if (status >= 200 && status < 300) ++t.answered2xx;
    else if (status >= 500) ++t.answered5xx;
    else if (status >= 400) ++t.answered4xx;
    snprintf(t.lines[t.lineHead], sizeof t.lines[0], "%.63s", line);
    t.lineHead = (t.lineHead + 1) % kRequestLines;
    if (t.lineCount < kRequestLines) ++t.lineCount;
}

namespace {
// after the request was counted (outside the lock): an answer of 400 or more, or no answer that was not a cancel, is an error of the stage
void RequestError(int status, const char* failName, const char* codeWord) {
    if (failName) {
        if (!strncmp(failName, "CANCEL", 6)) return;
        Error(kStageNone, 0, failName);
    } else if (status >= 400) {
        Error(kStageNone, status, codeWord && *codeWord ? codeWord : "http");
    }
}
}  // namespace

void NoteSockets(int live, int opened, int closed, int most, int refused) {
    Lock lock;
    Trail& t = T();
    t.sockLive = live;
    t.sockOpened = opened;
    t.sockClosed = closed;
    t.sockMost = most;
    t.sockRefused = refused;
}

void NoteMismatch(int what, int tick, uint32_t ours, uint32_t theirs) {
    if (what < 1 || what > 3) what = 2;
    {
        Lock lock;
        Trail& t = T();
        if (t.mismatchTick[what] >= 0) return;   // the first of each kind in a trail
        t.mismatchTick[what] = tick;
        t.mismatchOurs[what] = ours;
        t.mismatchTheirs[what] = theirs;
    }
    char text[48];
    snprintf(text, sizeof text, "ours %08x theirs %08x", unsigned(ours), unsigned(theirs));
    Note(kMismatch, tick, what, 0, text);
}

void NoteWait(uint32_t waitedMs) {
    Lock lock;
    Trail& t = T();
    if (waitedMs > t.longestWaitMs) t.longestWaitMs = waitedMs;
    if (waitedMs >= 1500) ++t.waits;
}

void NotePeerBuild(bool same, uint32_t fingerprint, unsigned rules) {
    {
        Lock lock;
        Trail& t = T();
        if (t.peerSame == (same ? 1 : 0) && t.peerFingerprint == fingerprint) return;   // once per partner build
        t.peerSame = same ? 1 : 0;
        t.peerFingerprint = fingerprint;
        t.peerRules = rules;
    }
    char text[40];
    snprintf(text, sizeof text, "fingerprint %08x rules %02x", unsigned(fingerprint), rules & 0xffu);
    Note(kPeerBuild, same ? 1 : 0, 0, 0, text);
}

void NoteResultUpload(int attempts, int outcome, const char* word) {
    char w[24];
    SafeWord(word, w, sizeof w);
    {
        Lock lock;
        Trail& t = T();
        t.uploadAttempts = attempts;
        t.uploadOutcome = outcome;
        snprintf(t.uploadWord, sizeof t.uploadWord, "%s", w);
        if (outcome < 0 && g_offer == 0) g_offer = 1;   // a ranked result that is lost for good: offer a report
#ifdef NP_MPDIAG_MUT_UPLOAD_NO_OFFER   // mutant of tests/test_mpbeta.py: a dropped result does not arm the offer
        if (outcome < 0 && g_offer == 1) g_offer = 0;
#endif
    }
    Note(kResultUpload, attempts, outcome, 0, w);
}

Trail CurrentTrail() {
    Lock lock;
    return T();
}

void TrailClearForTest() {
    Lock lock;
    g_trailInit = false;
    TrailReset(g_trail);
    g_trail.lastRunCrashed = false;
    g_trail.crashedStage[0] = 0;
}

namespace {
void Addf(std::string& s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Addf(std::string& s, const char* fmt, ...) {
    char b[320];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    s += b;
    s += '\n';
}
const char* MismatchName(int k) { return k == 1 ? "frame number" : k == 2 ? "checksum" : "random seed"; }
}  // namespace

size_t TrailReport(const Trail& t, const LinkFigures& f, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    std::string s;
    s.reserve(2048);
    Addf(s, "[session]");
    Addf(s, "mode = %s, %s", LinkName(t.link), PlayName(t.play));
    Addf(s, "stage reached = %s (now %s)", StageName(t.furthest), StageName(t.now));
    {
        std::string order;
        for (int i = 1; i < kStageCount; ++i)
            if (t.stageMs[i]) {
                char b[48];
                snprintf(b, sizeof b, "%s%s T+%u.%us", order.empty() ? "" : ", ", StageName(Stage(i)), t.stageMs[i] / 1000u, (t.stageMs[i] % 1000u) / 100u);
                order += b;
            }
        Addf(s, "stages = %s", order.empty() ? "none" : order.c_str());
    }
    if (t.errors && t.lastErrorCode) Addf(s, "errors = %d, the last in %s: %s, status %d", t.errors, StageName(t.lastErrorStage), t.lastErrorWord[0] ? t.lastErrorWord : "-", t.lastErrorCode);
    else if (t.errors) Addf(s, "errors = %d, the last in %s: %s", t.errors, StageName(t.lastErrorStage), t.lastErrorWord[0] ? t.lastErrorWord : "-");
    else Addf(s, "errors = none");
    if (t.uploadOutcome != -2)
        Addf(s, "ranked result upload = %s after %d attempt%s%s%s", t.uploadOutcome == 1 ? "accepted" : t.uploadOutcome == 0 ? "failed, kept for a retry" : "dropped", t.uploadAttempts,
             t.uploadAttempts == 1 ? "" : "s", t.uploadWord[0] ? ", " : "", t.uploadWord);
    if (t.lastRunCrashed) Addf(s, "last run = ended inside a multiplayer session (stage %s) without leaving it", t.crashedStage[0] ? t.crashedStage : "?");
    Addf(s, "%s", "");
    Addf(s, "[network]");
    Addf(s, "requests = %d (2xx %d, 4xx %d, 5xx %d, no answer %d), retries %d, slowest %u ms", t.requests, t.answered2xx, t.answered4xx, t.answered5xx, t.failed, t.retries, unsigned(t.slowestMs));
    if (t.failed)
        Addf(s, "no answer by kind = timeout %d, dns %d, connect %d, tls %d, cancelled %d, other %d; the last %s", t.failByName[0], t.failByName[1], t.failByName[2], t.failByName[3],
             t.failByName[4], t.failByName[5], t.lastFail[0] ? t.lastFail : "-");
    Addf(s, "sockets = %d open, %d opened, %d closed, most at once %d, refused %d", t.sockLive, t.sockOpened, t.sockClosed, t.sockMost, t.sockRefused);
    Addf(s, "link = round trip %d ms, jitter %d ms, loss %d permille, relay %s, router %s", f.rttMs, f.jitterMs, f.lossPermille, f.relay < 0 ? "unknown" : f.relay ? "yes" : "no",
         f.nat == 1 ? "no NAT" : f.nat == 2 ? "cone NAT" : f.nat == 3 ? "symmetric NAT" : "not measured");
    if (t.lineCount) {
        Addf(s, "last requests (newest last):");
        for (int i = 0; i < t.lineCount; ++i) Addf(s, "  %s", t.lines[(t.lineHead + kRequestLines - t.lineCount + i) % kRequestLines]);
    }
    Addf(s, "%s", "");
    Addf(s, "[lockstep]");
    Addf(s, "input delay = %d ticks%s; partner-lost limit %d ms (link), %d ms (online transport)", f.delayTicks, f.delayTicks ? "" : " (the game's own)", f.keepAliveMs, f.peerLostMs);
    if (f.steps + f.starved > 0)
        Addf(s, "steps = %d, starved %d (%d.%d%%)", f.steps, f.starved, int((1000LL * f.starved / (f.steps + f.starved)) / 10), int((1000LL * f.starved / (f.steps + f.starved)) % 10));
    else Addf(s, "steps = none recorded (no match ended in this session)");
    Addf(s, "waits for the partner = %d over 1.5 s, the longest %u ms", t.waits, unsigned(t.longestWaitMs));
    bool any = false;
    for (int k = 1; k <= 3; ++k)
        if (t.mismatchTick[k] >= 0) {
            any = true;
            Addf(s, "desync = %s at step %d: ours %08x theirs %08x", MismatchName(k), t.mismatchTick[k], unsigned(t.mismatchOurs[k]), unsigned(t.mismatchTheirs[k]));
        }
    if (!any) Addf(s, "desync = none detected");
    Addf(s, "%s", "");
    Addf(s, "[peers]");
    Addf(s, "this Vita = version %s, fingerprint %08x", NpNet::BuiltAppVersion(), NpLanFingerprint ? NpLanFingerprint() : 0u);
    if (t.peerSame < 0) Addf(s, "partner = not seen");
    else if (t.peerSame) Addf(s, "partner = the same build (fingerprint %08x, rules %02x)", unsigned(t.peerFingerprint), t.peerRules & 0xffu);
    else Addf(s, "partner = ANOTHER build or rules was refused (fingerprint %08x, rules %02x)", unsigned(t.peerFingerprint), t.peerRules & 0xffu);
    const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    memcpy(out, s.data(), n);
    out[n] = 0;
    return n;
}

void NoteRequest(const char* method, const char* path, int status, const char* failName, uint32_t ms, int reused, int attempt, const char* codeWord) {
    CountRequest(method, path, status, failName, ms, reused, attempt);
    RequestError(status, failName, codeWord);
}

// ---- crash on the next start -----------------------------------------------------------------------------------------------------------------
namespace {
bool MarkOn() {
    const char* v = getenv("MSC_MP_CRASH_MARK");
    return !(v && v[0] == '0');
}
void MarkPath(const char* dir, char* out, size_t cap) {
    char dd[200];
    if (!dir || !*dir) {
#ifdef __vita__
        snprintf(dd, sizeof dd, "ux0:data/mscharged");
#else
        const char* d = getenv("NP_DATA_DIR");
        snprintf(dd, sizeof dd, "%s", d && *d ? d : ".");
#endif
        dir = dd;
    }
    snprintf(out, cap, "%s/%s", dir, kSessionMarkFile);
}
}  // namespace

void SessionMarkWrite(const char* dir) {
    if (!MarkOn()) return;
    char path[300];
    MarkPath(dir, path, sizeof path);
    Stage s;
    {
        Lock lock;
        s = T().now;
    }
    NpSafeFile::Options opt;
    opt.mode = 0644;
    opt.maxBytes = 64;
    NpSafeFile::Replace(path, std::string(StageName(s)) + "\n", opt);
}

void SessionMarkClear(const char* dir) {
    if (!MarkOn()) return;
    char path[300];
    MarkPath(dir, path, sizeof path);
    remove(path);
}

bool SessionMarkCheckAtStart(const char* dir) {
    if (!MarkOn()) return false;
    char path[300];
    MarkPath(dir, path, sizeof path);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char raw[40] = "";
    const size_t n = fread(raw, 1, sizeof raw - 1, f);
    fclose(f);
    raw[n] = 0;
#ifndef NP_MPDIAG_MUT_MARK_KEEP   // mutant of tests/test_mpbeta.py: the marker stays, so every start "finds a crash"
    remove(path);
#endif
    if (char* end = strpbrk(raw, "\r\n")) *end = 0;
    char stage[24] = "other";   // only a stage name this code writes is taken back (the file is the player's to edit)
    for (int i = 0; i < kStageCount; ++i)
        if (!strcmp(raw, StageName(Stage(i)))) snprintf(stage, sizeof stage, "%s", StageName(Stage(i)));
    {
        Lock lock;
        Trail& t = T();
        t.lastRunCrashed = true;
        snprintf(t.crashedStage, sizeof t.crashedStage, "%s", stage);
    }
    Note(kLastRunCrashed, 0, 0, 0, stage);
    return true;
}

}  // namespace NpMpDiag

extern "C" size_t NpMpDiagSessionHeader(char* out, size_t cap) {
    return NpMpDiag::SessionHeader(NpMpDiag::LastSession(), out, cap);
}

extern "C" void NpMpDiagNoteRequest(const char* method, const char* path, int status, const char* failName, unsigned ms, int reused, int attempt, const char* codeWord) {
    NpMpDiag::NoteRequest(method, path, status, failName, ms, reused, attempt, codeWord);
}
extern "C" void NpMpDiagNoteSockets(int live, int opened, int closed, int most, int refused) { NpMpDiag::NoteSockets(live, opened, closed, most, refused); }
extern "C" void NpMpDiagReach(int stage) { if (stage > 0 && stage < NpMpDiag::kStageCount) NpMpDiag::Reach(NpMpDiag::Stage(stage)); }
extern "C" void NpMpDiagError(int stage, int code, const char* word) {
    NpMpDiag::Error(stage >= 0 && stage < NpMpDiag::kStageCount ? NpMpDiag::Stage(stage) : NpMpDiag::kStageNone, code, word);
}
extern "C" void NpMpDiagResultUpload(int attempts, int outcome, const char* word) { NpMpDiag::NoteResultUpload(attempts, outcome, word); }
