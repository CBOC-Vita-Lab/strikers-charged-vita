// A plain message on the screen for a game that has stopped presenting pictures (1.2, STR-006 / Galaxy's stall message).
//
// StallWatch (np_vita_wire.cpp) notices that no frame was presented for a while; the player then sees the last picture frozen and cannot tell a slow
// load from a hang. DrawBanner writes a dark band with up to three lines of text across the middle of a frame buffer in the display format
// (0xAABBGGRR, kVitaFont, 12 x 24 pixels per character), with nothing but plain writes: the buffer on the console is in CDRAM, which the CPU reads very
// slowly, so nothing is blended. The band covers a part of the frozen picture; the game's next picture replaces the buffer whole, so the message goes
// away by itself the moment the game presents again. Nothing is killed or stopped.
//
// Plain C++ with no console calls, so that tests/test_notice.cpp checks it on the host.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "vita_font.h"
#include "../../sdk/include/np/np_text.h"

namespace VitaNotice {

constexpr int kMaxLines = 3;
constexpr int kMargin = 36;        // pixels of band to the left and right of the text area
constexpr int kPadY = 14;          // pixels above the first line and below the last

struct Band {
    int x0, y0, x1, y1;            // the rectangle covered, x1 / y1 exclusive
};

// Where the band goes for a buffer of this size and this many lines (centred on the screen, a little above the middle).
inline Band BandFor(int width, int height, int lines) {
    if (lines < 1) lines = 1;
    if (lines > kMaxLines) lines = kMaxLines;
    Band b;
    b.x0 = kMargin;
    b.x1 = width - kMargin;
    const int h = lines * kVitaFontHeight + 2 * kPadY;
    b.y0 = (height - h) / 2 - height / 12;
    if (b.y0 < 0) b.y0 = 0;
    b.y1 = b.y0 + h;
    if (b.y1 > height) b.y1 = height;
    return b;
}

// Most characters a line may have: longer lines are cut, not wrapped.
inline int Columns(int width) { return (width - 2 * kMargin - 2 * 16) / kVitaFontWidth; }

inline uint32_t Pack(unsigned r, unsigned g, unsigned b) { return 0xFF000000u | (b << 16) | (g << 8) | r; }

// Draws the band and the lines (the first in white, the others in light grey; nullptr or "" ends the list) into `pixels`
// (`pitch` pixels per row). Returns the number of lines drawn. A zero-sized or null buffer draws nothing.
inline int DrawBanner(uint32_t* pixels, int pitch, int width, int height, const char* const* lines, int lineCount) {
    if (!pixels || pitch < width || width < 4 * kMargin || height < 4 * kVitaFontHeight || !lines) return 0;
    int n = 0;
    while (n < lineCount && n < kMaxLines && lines[n] && lines[n][0]) ++n;
    if (n == 0) return 0;
    const Band b = BandFor(width, height, n);
    const uint32_t fill = Pack(10, 14, 30), edge = Pack(255, 150, 30);
    for (int y = b.y0; y < b.y1; ++y) {
        uint32_t* row = pixels + static_cast<size_t>(y) * pitch;
        const bool border = y < b.y0 + 2 || y >= b.y1 - 2;
        for (int x = b.x0; x < b.x1; ++x) row[x] = (border || x < b.x0 + 2 || x >= b.x1 - 2) ? edge : fill;
    }
    const int columns = Columns(width);
    for (int i = 0; i < n; ++i) {
        const char* text = lines[i];
        const int length = VitaUtf8Length(text);
        const uint32_t color = i == 0 ? Pack(255, 255, 255) : Pack(205, 210, 225);
        const int top = b.y0 + kPadY + i * kVitaFontHeight;
        const int x0 = b.x0 + 16;
        for (int c = 0; c < columns && c < length; ++c) {
            const uint32_t ch = VitaUtf8Next(text);
            if (ch == ' ') continue;
            const uint16_t* glyph = VitaFontGlyph(ch);
            for (int y = 0; y < kVitaFontHeight; ++y) {
                const uint16_t bits = glyph[y];
                if (!bits) continue;
                uint32_t* out = pixels + static_cast<size_t>(top + y) * pitch + x0 + c * kVitaFontWidth;
                for (int x = 0; x < kVitaFontWidth; ++x)
                    if (bits & (1u << (kVitaFontWidth - 1 - x))) out[x] = color;
            }
        }
    }
    return n;
}

// The words, by how long the game has been quiet. Level 0: nothing yet; 1: a calm "still working"; 2: what to do about it.
// `seconds` is the quiet time the watch thread counted. Writes into `out` (kMaxLines lines of at most 96 characters).
struct Text {
    char line[kMaxLines][96];
    int count = 0;
};
inline int Level(int seconds, int firstAtSeconds) {
    if (firstAtSeconds <= 0 || seconds < firstAtSeconds) return 0;
    return seconds < 3 * firstAtSeconds ? 1 : 2;   // (20 s: still working; 60 s: what to do)
}
inline void Compose(int level, int seconds, Text& out) {
    out.count = 0;
    if (level <= 0) return;
    // (1.3) in the game's language (np_text.h); UTF-8, drawn in characters
    std::snprintf(out.line[0], sizeof out.line[0], NpText::Tr("Still working: no new picture for %d seconds."), seconds);
    std::snprintf(out.line[1], sizeof out.line[1], "%s", NpText::Tr("The game may be loading or waiting for the memory card. Please wait."));
    out.count = 2;
    if (level >= 2) {
        std::snprintf(out.line[2], sizeof out.line[2], "%s", NpText::Tr("If nothing changes, close the game from the PS menu and start it again."));
        out.count = 3;
    }
}

// 1.3 MSC_VITA_STALL_GUARD (no loading or "working" screen in play): what the watch thread counts as quiet. The notice is only ever for a game that has presented nothing for a long time:
//   - never earlier than kFloorSeconds, whatever MSC_VITA_STALL_NOTICE says (a value of 1 to 9 used to put the banner up at the first look, 5 s);
//   - the count is of looks (every 5 s) at an unchanged frame number, so a console that slept or a game the system suspended (the Home button) cannot add the time it was stopped:
//     when the power callback reports a wake (`epoch` changes) the count starts again and the next kGraceLooks looks do not count (the game needs a moment to present again);
//   - the Settings screen, which stops the game on purpose, resets it.
// Pure: tests/test_notice.cpp.
constexpr int kFloorSeconds = 10;
constexpr int kGraceLooks = 2;
inline int NoticeDelay(int configured, bool guard) {
    if (configured <= 0) return 0;                       // 0 = off
    return guard && configured < kFloorSeconds ? kFloorSeconds : configured;
}
struct Quiet {
    uint64_t lastFrames = ~0ull;
    uint32_t lastEpoch = 0;
    int seconds = 0, grace = 0;
    bool reset = false;                                  // true after the call that restarted the count (the caller clears its level)
    // One look, `step` seconds after the last: the frame number now, the wake counter, whether Settings has the game stopped. Returns the quiet seconds.
    int Look(uint64_t frames, uint32_t epoch, bool settings, int step, bool guard) {
        reset = false;
        if (guard && epoch != lastEpoch) { lastEpoch = epoch; seconds = 0; grace = kGraceLooks; reset = true; lastFrames = frames; return 0; }
        if (settings || frames != lastFrames) { lastFrames = frames; if (seconds || grace) reset = true; seconds = 0; grace = 0; return 0; }
        if (guard && grace > 0) { --grace; return 0; }
        seconds += step;
        return seconds;
    }
};

}  // namespace VitaNotice
