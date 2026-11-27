#include "np_startup_check.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace NpStartupCheck {

namespace {

void Add(Message& m, const char* line) {
    const size_t n = std::strlen(m.details);
    if (n + std::strlen(line) + 2 >= sizeof(m.details)) return;
    std::snprintf(m.details + n, sizeof(m.details) - n, "%s%s", n ? "\n" : "", line);
}

void AddPath(Message& m, const char* prefix, const char* path) {
    char line[300];
    std::snprintf(line, sizeof(line), "%s%s", prefix, path);
    Add(m, line);
}

// The six-character id at the start of sys/boot.bin, or false when the file cannot be read (then nothing is claimed).
bool ReadDiscId(const char* root, char id[7]) {
    char path[300];
    std::snprintf(path, sizeof(path), "%s/sys/boot.bin", root);
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    const size_t got = std::fread(id, 1, 6, f);
    std::fclose(f);
    if (got != 6) return false;
    id[6] = '\0';
    for (int i = 0; i < 6; ++i)
        if (!std::isprint(static_cast<unsigned char>(id[i]))) return false;
    return true;
}

}  // namespace

bool CheckGameData(const char* root, ExistsFn exists, Message& out) {
    out = Message{};
    char fst[300], files[300], sysDir[300], filesDir[300];
    std::snprintf(fst, sizeof(fst), "%s/sys/fst.bin", root);
    std::snprintf(files, sizeof(files), "%s/files", root);
    std::snprintf(sysDir, sizeof(sysDir), "%s/sys", root);
    std::snprintf(filesDir, sizeof(filesDir), "%s/files", root);
    if (!exists(fst) || !exists(files)) {
        out.category = "the game data is missing";
        Add(out, "Copy your own extracted Mario Strikers Charged disc (Wii, USA, R4QE01) into these two folders:");
        AddPath(out, "    ", sysDir);
        AddPath(out, "    ", filesDir);
        Add(out, "The CBOC Vita Installer does this for you.");
        Add(out, "Another place can be set with MSC_GAME_ROOT in ux0:data/mscharged/np-player.txt.");
        return false;
    }
    char id[7];
    if (ReadDiscId(root, id) && std::strcmp(id, kDiscId) != 0) {
        out.category = "this is not the right game disc";
        Add(out, "The game files found are for a different game or region. Mario Strikers Charged, Wii, USA (R4QE01) is needed.");
        AddPath(out, "Disc id found: ", id);
        AddPath(out, "Folder: ", root);
        return false;
    }
    return true;
}

bool CompilerPresent(ExistsFn exists) {
    for (const char* p : kCompilerPaths)
        if (exists(p)) return true;
    return false;
}

void RendererFailed(bool compilerPresent, Message& out, int cause) {
    out = Message{};
    if (!compilerPresent || cause == kCauseNoCompiler) {
        out.category = "the shader compiler is missing";
        Add(out, "The game needs the Vita's shader compiler module, which was not found. Copy libshacccg.suprx to:");
        AddPath(out, "    ", kCompilerPaths[0]);
        Add(out, "VitaShell can copy it there: put it in ux0:VPK, press Triangle, Copy, then open ur0:data and Paste.");
        Add(out, "The CBOC Vita Installer does this for you.");
    } else {
        out.category = "the graphics could not start";
        Add(out, "The Vita's graphics could not be set up. Restart the Vita and try again.");
        switch (cause) {   // (the numbers are the renderer's; one fixed line each, no number in it)
        case kCauseNoMemoryPool: Add(out, "Cause: the graphics memory pool could not be created."); break;
        case kCauseVideoMemory: Add(out, "Cause: not enough video memory was free at start-up."); break;
        case kCauseShaderTest: Add(out, "Cause: the shader compiler test failed."); break;
        case kCauseBackend: Add(out, "Cause: the graphics back end could not start."); break;
        case kCauseThread: Add(out, "Cause: the render thread could not be created."); break;
        default: break;
        }
        Add(out, "If it happens again, ux0:data/mscharged/log.txt says why.");
    }
}

int FormatLines(const char* details, const char* (*tr)(const char*), int width, char (*out)[kLineBytes], int maxLines) {
    int rows = 0;
    const char* p = details ? details : "";
    while (*p && rows < maxLines) {
        char src[300];
        size_t n = 0;
        while (*p && *p != '\n' && n < sizeof(src) - 1) src[n++] = *p++;
        src[n] = '\0';
        if (*p == '\n') ++p;
        int indent = 0;
        while (src[indent] == ' ') ++indent;
        char joined[400];
        const char* body = src + indent;
        const char* t = tr(body);
        if (t == body) {
            if (const char* colon = std::strstr(body, ": ")) {
                char label[120];
                const size_t len = static_cast<size_t>(colon - body) + 2;
                if (len < sizeof(label)) {
                    std::memcpy(label, body, len);
                    label[len] = '\0';
                    const char* tl = tr(label);
                    if (tl != label) { std::snprintf(joined, sizeof(joined), "%s%s", tl, colon + 2); t = joined; }
                }
            }
        }
        if (indent >= width - 8) indent = 0;
        do {
            char* line = out[rows];
            int bytes = 0, chars = indent, lastSpace = -1;
            for (int i = 0; i < indent; ++i) line[bytes++] = ' ';
            const char* q = t;
            while (*q && chars < width && bytes < kLineBytes - 5) {
                const unsigned char c = static_cast<unsigned char>(*q);
                int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
                for (int k = 1; k < len; ++k) if (!q[k]) { len = k; break; }
                if (c == ' ') lastSpace = bytes;
                std::memcpy(line + bytes, q, static_cast<size_t>(len));
                bytes += len; q += len; ++chars;
            }
            if (*q && lastSpace > indent) { bytes = lastSpace; q = t + (lastSpace - indent); }
            line[bytes] = '\0';
            while (*q == ' ') ++q;
            t = q;
            ++rows;
        } while (*t && rows < maxLines);
    }
    return rows;
}

const char* SeedFailedNotice() { return "FIRST-RUN FILES NOT SAVED. IS THE CARD FULL?"; }

}  // namespace NpStartupCheck
