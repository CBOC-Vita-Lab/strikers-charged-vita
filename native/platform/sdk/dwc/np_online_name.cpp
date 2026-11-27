// The public name: validation, storage and the request state machine. See np_online_name.h.
#include "platform/sdk/dwc/np_online_name.h"
#include "platform/sdk/save/np_safe_file.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "platform/sdk/dwc/np_online_api.h"
#include "platform/sdk/include/np/np_text.h"

namespace NpOnline {
namespace {

enum Phase { kIdle, kTyping, kDone, kCancelled };
Phase g_phase = kIdle;
char g_name[kMaxNameChars + 1] = "";
int g_attempts = 0;

bool NamePath(char* out, size_t size) {
    const int n = snprintf(out, size, "%s/online_name.txt", DataDir());
    return n > 0 && size_t(n) < size;
}

bool OpenKeyboard(const char* initial, const char* title) {
    if (!NpOnlineImeOpen || !NpOnlineImeAvailable || !NpOnlineImeAvailable()) return false;
    return NpOnlineImeOpen(initial, title) == 0;
}

}  // namespace

bool SanitizeName(const char* in, char* out, bool* changed) {
    // The service's rules (PROTOCOL.md section 3): 2..10 characters of A-Z a-z 0-9 space . _ -, starting and ending with a letter or digit,
    // no double spaces, at least two letters or digits. Everything else is dropped, spaces collapse, the ends are trimmed.
    size_t n = 0;
    bool lastSpace = false;
    for (size_t i = 0; in && in[i]; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (c == ' ' || c == '\t') {
            if (n && !lastSpace) { out[n++] = ' '; lastSpace = true; }   // never first; one at a time
        } else if (alnum || c == '.' || c == '_' || c == '-') {
            if (!n && !alnum) continue;                                  // must start with a letter or digit
            out[n++] = char(c);
            lastSpace = false;
        }
        if (n >= kMaxNameChars) break;
    }
    for (;;) {   // must end with a letter or digit
        while (n && !isalnum(static_cast<unsigned char>(out[n - 1]))) --n;
        break;
    }
    out[n] = 0;
    size_t letters = 0;
    for (size_t i = 0; i < n; ++i) letters += isalnum(static_cast<unsigned char>(out[i])) ? 1 : 0;
    if (letters < 2) { out[0] = 0; n = 0; }
    if (changed) *changed = !in || strcmp(in, out) != 0;
    return n > 0;
}

bool IsValidName(const char* name) {
    char clean[kMaxNameChars + 1];
    bool changed = false;
    return name && SanitizeName(name, clean, &changed) && !changed;
}

// np_safe_file.h: synced replace, a power cut leaves the old file or the new one; a damaged file is set aside as online_name.txt.bad
bool NameFileSound(const std::string& d) {
    char line[64];
    snprintf(line, sizeof line, "%.*s", int(d.size() < sizeof line - 1 ? d.size() : sizeof line - 1), d.c_str());
    char* end = strpbrk(line, "\r\n");
    if (end) *end = 0;
    return IsValidName(line);
}
NpSafeFile::Options NameFileOptions() { NpSafeFile::Options o; o.valid = NameFileSound; o.mode = 0644; o.maxBytes = 4096; return o; }

bool LoadStoredName(char* out) {
    char path[512], line[64];
    out[0] = 0;
    if (!NamePath(path, sizeof path)) return false;
    NpSafeFile::Recover(path, NameFileOptions());
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    const size_t n = fread(line, 1, sizeof line - 1, f);
    fclose(f);
    line[n] = 0;
    char* end = strpbrk(line, "\r\n");
    if (end) *end = 0;
    return IsValidName(line) && (strcpy(out, line), true);
}

bool StoreName(const char* name) {
    char path[512];
    if (!IsValidName(name) || !NamePath(path, sizeof path)) return false;
    return NpSafeFile::Replace(path, std::string(name) + "\n", NameFileOptions());
}

void ForgetName() {
    char path[512];
    if (NamePath(path, sizeof path)) NpSafeFile::RemoveAll(path);   // (with its .tmp, .bak and .bad copies)
    g_name[0] = 0;
    g_phase = kIdle;
}

void NameResetForTest() { g_phase = kIdle; g_name[0] = 0; g_attempts = 0; }

const char* Name() { return g_name; }

NameState NameRequest() {
    if (g_phase == kDone) return NameState::Ready;
    if (g_phase == kCancelled) { g_phase = kIdle; }
    if (g_phase == kIdle) {
        // an environment override wins on every build (scripted runs, console env.txt)
        const char* env = getenv("MSC_ONLINE_NAME");
        char clean[kMaxNameChars + 1];
        if (env && *env && SanitizeName(env, clean)) {
            strcpy(g_name, clean);
            g_phase = kDone;
            return NameState::Ready;
        }
        if (LoadStoredName(clean)) {
            strcpy(g_name, clean);
            g_phase = kDone;
            return NameState::Ready;
        }
        char suggestion[kMaxNameChars + 1] = "";
        if (NpOnlineImeSuggestion) SanitizeName(NpOnlineImeSuggestion(), suggestion);
        if (!suggestion[0]) strcpy(suggestion, "Player");
        if (OpenKeyboard(suggestion, NpText::Tr("Name for the online rankings (10 characters)"))) {
            g_phase = kTyping;
            g_attempts = 0;
            return NameState::Pending;
        }
        strcpy(g_name, suggestion);   // no keyboard (host without MSC_ONLINE_NAME): the suggestion, not stored
        g_phase = kDone;
        return NameState::Ready;
    }
    // typing
    char text[96];
    const int r = NpOnlineImePoll ? NpOnlineImePoll(text, sizeof text) : -1;
    if (r == 0) return NameState::Pending;
    if (r == 1) {
        char clean[kMaxNameChars + 1];
        if (SanitizeName(text, clean)) {
            strcpy(g_name, clean);
            if (!StoreName(g_name)) fprintf(stderr, "[online] the name could not be stored; it is asked again next time\n");
            g_phase = kDone;
            return NameState::Ready;
        }
        if (++g_attempts < 3 && OpenKeyboard("", NpText::Tr("Use letters, digits or symbols (10 characters)"))) return NameState::Pending;
    }
    g_phase = kCancelled;
    return NameState::Cancelled;
}

}  // namespace NpOnline
