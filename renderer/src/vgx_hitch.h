// MSC_HITCH_MS: what made a long frame. Header-only so a test can run it on its own; no clock, no I/O, no GL. The back end feeds it one record per frame.
// A frame's "gap" is begin to begin on the back end's thread, so a long gap is paid for by the frame BEFORE the one that sees it: Note() holds a frame's counters back
// by one and judges them with the next frame's gap. The verdict is a single cause, first match in this order, so one line of the summary never counts a frame twice:
//   shader   a real compile, or link/variant work taking a quarter of the gap (a pack hit counts as a compile in the counters but costs little, hence shaderMs too)
//   target   render targets or pass objects made, a picture-size change
//   texture  64 KiB of texels or more uploaded
//   wait     the game thread waited for the disc this frame
//   display  the swap (vitaGL's queue, the GPU queue limit, the collector's credit) took half of the gap
//   backend  the back end's own work took 60 % of the gap, nothing above explains it
//   upstream the back end had nothing to do: the frame came late from the game thread or the front end (read [dvdtrace], the [lane] game/fe columns and the audio line)
//
// Wave 9: a frame gap is a hitch only against the cadence in force. MSC_HITCH_MS=25 is right at 60 (a 16.7 ms period), but a scene held at 30 shows a 33.4 ms gap on EVERY frame, so every frame of a held
// scene was a "hitch": the 240 live-play lines were used up in 8 seconds and the real hitches after them were lost (and each line is a 400-character fprintf on the render thread). Note() now takes the
// period in force (the display interval x 16.68 ms) and judges a gap against max(MSC_HITCH_MS, period x 1.45); the gaps between the two are counted as "at the period" in the summary. The summary also
// folds the hitches by frame % 600 in 60-frame bins: a cause that comes once every 600 frames (the thread sweep) is one bin.
#pragma once
#include <cstdint>
#include <cstdio>

#ifndef VGX_HITCH_BREAK
#define VGX_HITCH_BREAK 0   // host test mutants: 1 the period is ignored (every held frame is a hitch), 2 the fold bins by frame % 599, 3 held lines share the menu budget
#endif

namespace vgx::hitch {

enum Cause : unsigned { CShader, CTarget, CTexture, CWait, CDisplay, CBackend, CUpstream, CauseCount };
inline constexpr const char* CauseNames[CauseCount] = {"shader", "target", "texture", "wait", "display", "backend", "upstream"};

struct Frame {
    uint64_t frame = 0;
    float be = 0, swap = 0, shaderMs = 0, rzMs = 0;   // ms: the back end's whole frame, its swap, shader work, a size change
    uint32_t compiles = 0, packHits = 0, made = 0, resizes = 0, texKib = 0, syncReads = 0;   // made: render targets + pass objects + copy textures
};

inline Cause Classify(const Frame& f, float gap) {
    const uint32_t realCompiles = f.compiles > f.packHits ? f.compiles - f.packHits : 0;
    if (realCompiles || f.shaderMs >= 0.25f * gap) return CShader;
    if (f.made || f.resizes || f.rzMs >= 0.25f * gap) return CTarget;
    if (f.texKib >= 64) return CTexture;
    if (f.syncReads) return CWait;
    if (f.swap >= 0.5f * gap) return CDisplay;
    if (f.be >= 0.6f * gap) return CBackend;
    return CUpstream;
}

// How many long frames, by cause; `window` frames to a summary. MSC_HITCH_MS <= 0: everything off.
struct Ledger {
    float limitMs = 0;
    uint32_t window = 1800, seen = 0, count[CauseCount] = {}, over100 = 0, lines = 0, lineCap = 240;
    uint32_t atPeriod = 0, fold[10] = {};   // gaps over MSC_HITCH_MS that were only the cadence in force; hitches by frame % 600 in 60-frame bins
    // Wave 11 (logs16): a third budget for held scenes (cut-scene, replay). Before the first live-play picture of a match a held scene (the walk-out) may use a quarter of it, so the goal cuts and replays of the match always find lines left
    // (r13 gsx had no [hitch] line for the goal cut: menus and the walk-out had spent the one budget). MSC_HITCH_LINES sets all three caps.
    uint32_t heldLines = 0, heldCap = 240;
    bool matchSeen = false;
    uint32_t playLines = 0, playCap = 240;   // live play has its own budget of lines: the loading screens of the boot must not use up the lines a match needs (MSC_HITCH_LINES sets both caps)
    float longest = 0;
    Frame prev;
    bool havePrev = false;
    // One frame: its record and the gap since the previous frame began. True: `prev` was the cause of a long frame and `*cause` says what it was.
    // periodMs: the length of one picture under the policy in force (0: unknown, MSC_HITCH_MS alone decides).
    bool Note(const Frame& f, float gap, Cause* cause, Frame* blamed, float periodMs = 0.0f) {
        bool hit = false;
        float eff = limitMs;
#if VGX_HITCH_BREAK == 1
        (void)periodMs;
#else
        if (periodMs * 1.45f > eff) eff = periodMs * 1.45f;
#endif
        if (limitMs > 0 && havePrev && gap >= limitMs && gap < eff) ++atPeriod;
        if (limitMs > 0 && havePrev && gap >= eff) {
#if VGX_HITCH_BREAK == 2
            ++fold[(prev.frame % 599) / 60 % 10];
#else
            ++fold[(prev.frame % 600) / 60];
#endif
            *cause = Classify(prev, gap);
            *blamed = prev;
            ++count[*cause];
            if (gap >= 100.0f) ++over100;
            if (gap > longest) longest = gap;
            hit = true;
        }
        prev = f;
        havePrev = true;
        ++seen;
        return hit;
    }
    bool WantLine(bool play = false) {
        if (play) return playLines < playCap && ++playLines;
        return lines < lineCap && ++lines;
    }
    // cls: 0 outside play and held scenes (menus, loading), 1 live play, 2 held scene (cut-scene, replay).
    bool WantLineClass(int cls) {
        if (cls == 1) { matchSeen = true; return playLines < playCap && ++playLines; }
#if VGX_HITCH_BREAK == 3
        if (cls == 2) return lines < lineCap && ++lines;
#else
        if (cls == 2) {
            const uint32_t limit = matchSeen ? heldCap : heldCap / 4;
            return heldLines < limit && ++heldLines;
        }
#endif
        return lines < lineCap && ++lines;
    }
    bool WindowDone() const { return limitMs > 0 && seen >= window; }
    size_t Summary(char* out, size_t cap) {
        size_t n = 0;
        auto add = [&](int w) { if (w > 0) n += static_cast<size_t>(w) < cap - n ? static_cast<size_t>(w) : cap - n - 1; };
        uint32_t total = 0;
        for (uint32_t c : count) total += c;
        add(std::snprintf(out, cap, "[hitch] %u frames: %u at %.0f ms or more (%u at 100+; %u more were only the held cadence), longest %.1f ms:", seen, total, limitMs, over100, atPeriod, longest));
        for (unsigned i = 0; i < CauseCount; ++i) add(std::snprintf(out + n, cap - n, " %s %u", CauseNames[i], count[i]));
        add(std::snprintf(out + n, cap - n, "; by frame %% 600 in 60-frame bins:"));
        for (unsigned i = 0; i < 10; ++i) add(std::snprintf(out + n, cap - n, " %u", fold[i]));
        atPeriod = 0;
        for (uint32_t& b : fold) b = 0;
        seen = 0;
        over100 = 0;
        longest = 0;
        for (uint32_t& c : count) c = 0;
        return n;
    }
};

} // namespace vgx::hitch
