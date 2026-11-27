// Start-up checks that need no Vita API (host-tested): what to tell the player when the disc files, the disc itself or the shader compiler
// module are missing. Every message is a category plus lines; the category and each line are English keys of np_text_strings.tsv (the fatal
// screen translates them one by one), a line that is a path or an id is not a key and stays as written.
#pragma once

#include <cstddef>

namespace NpStartupCheck {

using ExistsFn = bool (*)(const char* path);

struct Message {
    const char* category = nullptr;   // an English key; nullptr = nothing is wrong
    char details[900] = {};           // lines separated by '\n'
};

constexpr const char* kDiscId = "R4QE01";   // Mario Strikers Charged, Wii, USA (the disc this port is made from)
constexpr const char* kCompilerPaths[] = {"ur0:data/libshacccg.suprx", "ur0:data/external/libshacccg.suprx"};

// The game folder: sys/fst.bin and files/ must exist, and sys/boot.bin must name the right disc. Returns true when all is well.
bool CheckGameData(const char* root, ExistsFn exists, Message& out);
// True when the shader compiler module is in either place.
bool CompilerPresent(ExistsFn exists);
// The renderer failed to start: the compiler message when the module is absent, else the general one with one line naming the cause when the renderer
// said which (`cause` is vgxn_vita_start_failure(): VGXN_START_* of renderer/native/vgx_native_vita.h, the same numbers; 0 = not known).
constexpr int kCauseNoCompiler = 1, kCauseNoMemoryPool = 2, kCauseVideoMemory = 3, kCauseShaderTest = 4, kCauseBackend = 5, kCauseThread = 6;
void RendererFailed(bool compilerPresent, Message& out, int cause = 0);
// The details as screen lines: each '\n' line is looked up with `tr` (whole line first, then the part up to the first ": "; a path or an id is not
// a key and stays), then wrapped at spaces to `width` characters (UTF-8 aware), continuation lines keeping the indent. Returns the line count.
constexpr int kLineBytes = 400;
int FormatLines(const char* details, const char* (*tr)(const char*), int width, char (*out)[kLineBytes], int maxLines);
// The text a player sees for the first-run files that could not be copied (a loading-screen line, English key).
const char* SeedFailedNotice();

}  // namespace NpStartupCheck
