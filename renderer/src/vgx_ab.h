// MSC_AB (wave 15, helper ab15; after Galaxy's ab27.h GXV_AB): the in-run A/B harness. ONE console run answers "does switch K help?": inside the run the value of K alternates
//   a, b, a, b ... in fixed windows (a = the baseline, b = the candidate), every [lane] line is cut at the flip and tagged with its window, and tools/console-1.2/abpair.py --inrun
//   compares the b windows with the a windows beside them, so heat, battery and card drift (the run-to-run noise that made single runs useless) cancel.
//
//   MSC_AB=<KEY>,<a>,<b>[,<window s>][;<KEY2>,<a>,<b>...]   e.g.  MSC_AB=MSC_VGX_PX_NOOP,1,0,6;MSC_VGX_PX_SCISSOR,1,0   (default window 6 s; at most 6 keys, one after the other; the
//       console's test build refuses a ':' in an env value, so this is the form for an env file; the host also takes KEY:a,b[:window])
//       MSC_AB=list prints the registry; unset: nothing happens, DIAGNOSTIC ONLY). A key's block is a (b a) x MSC_AB_REPEATS (default 4: 9 windows, 54 s at 6 s), so every b window has an a
//       window on both sides; the other keys sit at their a value meanwhile.
//   MSC_AB_START_S (10)  seconds of play before the first flip (arm a all that time, tagged "ab-pre")    MSC_AB_SETTLE_S (1.0)  seconds after every flip that are cut out of the window
//   Log: "[ab] ..." at the start and at every flip; "[lane] ... ab <KEY>=<v> <a|b> w<N>" for a window (N counts over the whole run), "ab-settle" for the settle part, "ab-pre" before the
//   first flip, "ab-done" after the last block.
//
// What can be flipped (the registry kKnobs below; a key that is not in it is REFUSED with its reason, never silently ignored):
//   kKnob   a vgx::EnvInt switch read through VGX_KNOB at every use (not kept in a function-local static, not baked into a cache): the value lives in the knob's atomic word,
//           the flip re-works the expression out (EnvInt answers the override) and stores it: the very next read sees it.
//   kCall   a switch read by EnvInt (or VgxAbOverride) at every use: follows the override with no further step.
//   kApply  a switch whose effect is a state of the render thread: the flip is only requested (a generation counter) and made by the render thread itself at its next scene
//           boundary (RenderTick), so it never changes mid-scene.
// NOT flippable, by rule: anything that changes simulation state (game time, AI, physics), memory layout or sizes, threads and their cores, a value read once at boot (static const,
// a global filled at init), the shader text or the pack, anything a cache is keyed on.
// Bit-exact OFF arm: with MSC_AB unset nothing here runs (one relaxed load in EnvInt, nothing in the knob read path). With MSC_AB set and both arms equal the override only ever
// answers the value the environment gave (host proof: NP_DET_TRACE seed 1234abcd identical, tests/ab15.cpp).
// Pure and header-only: no clock, no platform include (tests/ab15.cpp runs this code).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vgx {
namespace ab {

// ---- the registry: what may be flipped --------------------------------------------------------------------------------------------------------
enum Kind : uint8_t { kKnob, kCall, kApply };
struct KnobInfo { const char* key; Kind kind; const char* why; };
inline constexpr KnobInfo kKnobs[] = {
    {"MSC_VGX_PX_SCISSOR", kKnob, "back end, decided at every draw: leave a scissor that contains the viewport as it is; same picture"},
    {"MSC_VGX_PX_NOOP", kKnob, "back end, decided at every draw: leave out a draw that can write nothing; same picture"},
    {"MSC_VGX_PX_DEADCLEAR", kKnob, "back end, decided at every clear (needs MSC_VGX_RT=1): leave out the depth of a clear nothing reads; same picture"},
    {"MSC_VGX_NOMERGE", kKnob, "recorder and frame-end pass, decided at every draw: merge consecutive draws or not; same picture, but the back-end record hash differs (measured on the host)"},
    {"MSC_VGX_BEPREFETCH", kKnob, "back end, per draw: prefetch hints for the draws ahead (0 off, 2..4 commands ahead); no state, no output"},
    {"MSC_VGX_MIDFLUSH", kApply, "back end, made by the render thread at a scene boundary: sceGxmMidSceneFlush inside the EFB scene (mode*10000+n, vgx_midflush.h); Vita only"},
    {"MSC_VGX_LEDGERBAND", kKnob, "back end, shadow ledger (FillPolyBands / FillDrawCoverPlain): band skip, edge cull, corner memo; the coverage bands are bit for bit the same (replay15)"},
    {"MSC_SHADOW_EVERY", kCall, "game thread, per character draw in replays and cut-scenes (np/scenecut.h): how often a player's shadow picture is redrawn; no simulation state"},
    {"MSC_REPLAY_FARLOD", kCall, "game thread, per character draw in replays and cut-scenes (np/scenecut.h): the distance past which the open-play mesh is used; chosen once per character draw"},
    {"MSC_VGX_VP_EXACT", kApply, "vitaGL glViewport halves exact or integer (patch 0025, stateless); made by the render thread at a scene boundary; Vita only"},
    {"MSC_VGX_VGL_MASKKEEP", kApply, "vitaGL scissor-mask rebuild kept or drawn (patch 0026: the records are kept either way, 'the switch can change at any time'); scene boundary; Vita only"},
    {"MSC_VGX_VGL_MASKBG", kApply, "vitaGL background mask for scenes that do not load the surface (patch 0029 clears a surface set while on); scene boundary; Vita only"},
    {"MSC_VGX_VGL_MASKCLIP", kApply, "vitaGL clear quad of a scissor-mask rebuild over the old region or the whole target (patch 0055, same mask either way; the records are kept either way); scene boundary; Vita only"},
    {"MSC_VGX_VGL_FRAGMEMO", kApply, "vitaGL per-program memo of the shader patcher's answer (patch 0018, same pointers); scene boundary; Vita only"},
    {"MSC_VGX_VGL_UNIREUSE", kApply, "vitaGL naming a program's last uniform copy again (patch 0018: a copy is never named outside the scene it was made for, the generation moves at every scene end); scene boundary; Vita only"},
    {"MSC_SKIN_POSE_PAR", kCall, "game thread, once a picture (RenderSnapshot::Render, np/skinpose.h): the characters' skin-mesh poses on the job lanes or in the draws; render-only state, nothing carried between pictures, the same bits"},
    {"MSC_GT18_QUAT", kCall, "game thread and pose lanes, read at every nlQuatNLerp / nlQuatRenormalize call and followed once a picture (GameRenderTask::Run, math.cpp NpGt18Frame): the quaternion blend written without calls, the same bits (a lane that still reads the old value for the rest of a picture gives the same result)"},
    {"MSC_HELD_GOV", kKnob, "renderer, read at every picture (HeldGovTick, VGX_KNOB): the held-scene GPU governor on or off; off: level 0 at once, the next held scene starts afresh (scenes30)"},
    {"MSC_FX_GOV_HOLD", kKnob, "renderer, read at every measured frame (FxGovHold, VGX_KNOB): frames the fill governor keeps its thinning without large sprites; only how long a level stands (scenes30)"},
    {"MSC_VGX_CROWDTRIM_WELD", kKnob, "back end, read at every figure-pass draw the crowd trim measures (CtWeldOn, VGX_KNOB): welded lists measured over the vertices their indices name; boxes are cached by that range, so both arms keep their own entries (gpueff18)"},
    {"MSC_VGX_CROWDTRIM_SKEW", kKnob, "back end, read at every crowd quad the trim looks at (CtSkewOk, VGX_KNOB): non-parallelogram quads cut too; per quad, nothing kept (gpueff18)"},
    {"MSC_FS_REFRESH_PREFETCH", kCall, "game thread, once a refresh (glRenderList.cpp): prefetch hints while the snapshot records are copied; the bytes copied are the same"},
};
// Keys people will ask for and why they are refused (the message of MSC_AB when the key is one of these).
struct Refusal { const char* key; const char* why; };
inline constexpr Refusal kRefused[] = {
    {"MSC_VGX_SOFTXFORM", "the display-list cache decides 'promotable' and soft-or-not when an entry is built: a flip would leave entries of the other arm in the cache"},
    {"MSC_VGX_UNICMP", "mode 0 keeps a shadow copy of every uniform block, mode 1 does not: a flip leaves a stale shadow (no rebuild path exists)"},
    {"MSC_VGX_BONECMP", "mode 0 keeps the skin block's shadow copy, mode 1 compares with vitaGL's buffer and keeps none: a flip leaves a stale shadow (no rebuild path exists)"},
    {"MSC_VGX_DLWATCH_BUCKETS", "the display-list entries' watch stamps are epoch sums or one epoch; a flip would compare stamps of the other kind (read once)"},
    {"MSC_VGX_CMPRA", "acts when a texture is uploaded (texture cache): textures already uploaded keep their arm"},
    {"MSC_FX_COMPOSITE", "the effects compositor allocates its targets and compiles its pass programs (warm-up at init): a flip would compile and allocate in the window"},
    {"MSC_FX_GOV", "the governor's state and the game-side particle tightening pool are made at init"},
    {"MSC_FX_TIGHT", "allocates the tightening pool at init (ParticleSystem.cpp TightInit)"},
    {"MSC_FX_LOWRES", "read once at init into the effects-pass policy"},
    {"MSC_VGX_TEXSAME", "texture memo state is keyed on it"},
    {"MSC_VGX_ABSIDX", "changes the index layout of the buffers"},
    {"MSC_VGX_ATLAS", "changes memory layout (texture atlas)"},
    {"MSC_VGX_SKINMEMO", "memo state is keyed on it"},
    {"MSC_VGX_FS_HALF", "changes the shader text: a flip compiles shaders in the window"},
    {"MSC_VGX_VS_TRIM", "changes the shader text: a flip compiles shaders in the window"},
    {"MSC_VGX_TEV_CANON", "changes the shader text: a flip compiles shaders in the window"},
    {"MSC_VGX_YIELD", "threads"},
    {"MSC_SMOOTH_CLOCK", "changes the game time of a frame (simulation state)"},
    {"MSC_LOCK60", "changes the frame rate and the pacing (simulation state, time)"},
    {"MSC_SIMT", "threads: the match step on its own thread"},
    {"MSC_VITA_TEXCOPY", "acts only while a texture is uploaded (a stage load): no window to alternate in"},
    {"MSC_VGX_TEX_CEILING", "the picture is wrong on purpose and uploaded textures keep their arm"},
    {"MSC_VGX_ASYNC_HEAPONLY", "read once when the shader worker starts"},
};
inline const KnobInfo* Find(const char* key) {
    for (const KnobInfo& k : kKnobs) if (!std::strcmp(k.key, key)) return &k;
    return nullptr;
}
inline const char* RefusalOf(const char* key) {
    for (const Refusal& r : kRefused) if (!std::strcmp(r.key, key)) return r.why;
    return nullptr;
}

// ---- the schedule (pure) ----------------------------------------------------------------------------------------------------------------------
constexpr int kMaxKeys = 6;
struct Key {
    char key[48] = {};
    long v[2] = {0, 0};
    double windowS = 6.0;
};
struct Spec {
    Key k[kMaxKeys];
    int n = 0;
    int repeats = 4;   // b windows a key gets; its block is a (b a) x repeats: every b has an a on both sides
    double settleS = 1.0, startS = 10.0;
};
// One key. The console's test build refuses a ':' (and '/') in a value, so the form that goes into an env file is  KEY,a,b[,window]  ; the host also takes KEY:a,b[:window].
inline bool ParseKey(const char* text, Key& s, char* err, size_t cap) {
    auto bad = [&](const char* m) { if (err && cap) std::snprintf(err, cap, "%s", m); return false; };
    if (!text || !*text) return bad("empty");
    const bool colon = std::strchr(text, ':') != nullptr;
    const char sep = colon ? ':' : ',';
    const char* c1 = std::strchr(text, sep);
    if (!c1 || c1 == text) return bad("expected KEY,a,b[,window s]");
    const size_t kn = static_cast<size_t>(c1 - text);
    if (kn >= sizeof s.key) return bad("key too long");
    std::memcpy(s.key, text, kn);
    s.key[kn] = 0;
    char* e = nullptr;
    s.v[0] = std::strtol(c1 + 1, &e, 0);
    if (e == c1 + 1 || *e != ',') return bad("expected two values a,b");
    const char* p = e + 1;
    s.v[1] = std::strtol(p, &e, 0);
    if (e == p) return bad("expected two values a,b");
    s.windowS = 6.0;
    if (*e == sep) {
        const char* w = e + 1;
        char* e2 = nullptr;
        const double d = std::strtod(w, &e2);
        if (e2 == w || *e2 != 0 || !(d >= 1.0 && d <= 600.0)) return bad("window must be 1..600 seconds");
        s.windowS = d;
    } else if (*e != 0) return bad("trailing text after the values");
    return true;
}
// "KEY:a,b[:w]" or several separated by ';' (at most kMaxKeys): one after the other, in this order.
inline bool ParseSpec(const char* text, Spec& s, char* err, size_t cap) {
    s.n = 0;
    if (!text || !*text) { if (err && cap) std::snprintf(err, cap, "empty"); return false; }
    char buf[384];
    std::snprintf(buf, sizeof buf, "%s", text);
    for (char* p = buf; *p;) {
        char* semi = std::strchr(p, ';');
        if (semi) *semi = 0;
        if (s.n >= kMaxKeys) { if (err && cap) std::snprintf(err, cap, "at most %d keys", kMaxKeys); return false; }
        if (!ParseKey(p, s.k[s.n], err, cap)) return false;
        for (int i = 0; i < s.n; ++i) if (!std::strcmp(s.k[i].key, s.k[s.n].key)) { if (err && cap) std::snprintf(err, cap, "%s twice", s.k[i].key); return false; }
        ++s.n;
        p = semi ? semi + 1 : p + std::strlen(p);
    }
    return s.n > 0;
}
inline double BlockS(const Spec& s, int ki) { return (2 * s.repeats + 1) * s.k[ki].windowS; }
inline double TotalS(const Spec& s) { double t = s.startS; for (int i = 0; i < s.n; ++i) t += BlockS(s, i); return t; }
struct Phase {
    int ki = -1;           // the key being flipped; -1 before the first block, n after the last one
    int wi = 0;            // window inside the block (0 = a, 1 = b, 2 = a ...)
    int win = -1;          // the window's number over the whole run; -1 before the start
    int arm = 0;           // 0 = a (baseline), 1 = b (candidate)
    bool settle = false;   // the first settleS seconds after a flip: cut out
    bool operator==(const Phase& o) const { return win == o.win && settle == o.settle && ki == o.ki; }
    bool operator!=(const Phase& o) const { return !(*this == o); }
};
// The phase at `since` seconds after the first picture. Blocks follow each other from startS; each starts and ends with arm a.
inline Phase PhaseAt(const Spec& s, double since) {
    Phase p;
    if (since < s.startS) return p;
    double t = since - s.startS;
    int base = 0;
    for (int i = 0; i < s.n; ++i) {
        const double blk = BlockS(s, i);
        if (t < blk) {
            int w = static_cast<int>(t / s.k[i].windowS);
            if (w < 0) w = 0;
            if (w > 2 * s.repeats) w = 2 * s.repeats;
            p.ki = i;
            p.wi = w;
            p.win = base + w;
#ifdef VGX_AB_BREAK
            p.arm = VGX_AB_BREAK == 1 ? 0 : (w & 1);   // (test mutant 1: the arm never alternates)
#else
            p.arm = w & 1;
#endif
            p.settle = w > 0 && (t - w * s.k[i].windowS) < s.settleS;
            return p;
        }
        t -= blk;
        base += 2 * s.repeats + 1;
    }
    p.ki = s.n;
    p.win = base;
    return p;   // (all blocks done: arm a, "ab-done")
}
// " ab KEY=v a w3" (a measured window), " ab-settle ...", " ab-pre ...", " ab-done ..."
inline int FormatTag(char* out, size_t cap, const Spec& s, const Phase& ph) {
    if (ph.ki < 0) return std::snprintf(out, cap, " ab-pre %s=%ld a w0", s.k[0].key, s.k[0].v[0]);
    if (ph.ki >= s.n) return std::snprintf(out, cap, " ab-done");
    const char* kind = ph.settle ? "ab-settle" : "ab";
    return std::snprintf(out, cap, " %s %s=%ld %c w%d", kind, s.k[ph.ki].key, s.k[ph.ki].v[ph.arm], ph.arm ? 'b' : 'a', ph.win);
}

// ---- the override (what EnvInt answers) --------------------------------------------------------------------------------------------------------
struct Override {
    std::atomic<int> n{0};   // keys in force (0: MSC_AB is off)
    char key[kMaxKeys][48] = {};
    std::atomic<long> val[kMaxKeys];
};
inline Override g_ov;
inline std::atomic<unsigned> g_touch{0};   // counts the answers given from the override (the flip tells which knobs read a key by this)
inline bool Lookup(const char* name, long* out) {
    const int n = g_ov.n.load(std::memory_order_relaxed);
    if (__builtin_expect(n == 0, 1)) return false;
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(name, g_ov.key[i]) != 0) continue;
#if !defined(VGX_AB_BREAK) || VGX_AB_BREAK != 2   // (test mutant 2: the key's readers are never told apart, so no knob is re-stored)
        g_touch.fetch_add(1, std::memory_order_relaxed);
#endif
        *out = g_ov.val[i].load(std::memory_order_relaxed);
        return true;
    }
    return false;
}
// For a read that is not EnvInt (the game's own code): the override's value for `key` when MSC_AB is flipping it, else `dflt`.
inline long Get(const char* key, long dflt) {
    long v;
    return Lookup(key, &v) ? v : dflt;
}

// ---- the VGX_KNOB words, registered at their first read ----------------------------------------------------------------------------------------
constexpr int kMaxReg = 1024;
struct Reg {
    std::atomic<int32_t>* k = nullptr;
    std::atomic<int32_t>* kf = nullptr;
    int32_t (*f)() = nullptr;
};
inline Reg g_reg[kMaxReg];
inline std::atomic<uint8_t> g_regReady[kMaxReg];
inline std::atomic<int> g_regN{0};
inline void Register(std::atomic<int32_t>& k, std::atomic<int32_t>& kf, int32_t (*f)()) {
    const int i = g_regN.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxReg) return;
    g_reg[i].k = &k;
    g_reg[i].kf = &kf;
    g_reg[i].f = f;
    g_regReady[i].store(1, std::memory_order_release);
}
// Works every registered knob out again under the current override and stores the ones that read the key (the others are left alone). Returns how many were stored.
constexpr int32_t kKnobUnset = INT32_MIN;   // = vgx::knob::kUnset (vgx_common.h asserts it)
inline int Reread() {
    int stored = 0;
    const int n = std::min<int>(g_regN.load(std::memory_order_relaxed), kMaxReg);
    for (int i = 0; i < n; ++i) {
        if (!g_regReady[i].load(std::memory_order_acquire)) continue;
        const Reg& r = g_reg[i];
        const unsigned before = g_touch.load(std::memory_order_relaxed);
        const int32_t v = r.f();
        if (g_touch.load(std::memory_order_relaxed) == before || v == kKnobUnset) continue;   // (did not read the key: its value stands)
        (r.k->load(std::memory_order_relaxed) != kKnobUnset ? *r.k : *r.kf).store(v, std::memory_order_relaxed);
        ++stored;
    }
    return stored;
}

// ---- kApply: made by the render thread at a scene boundary ---------------------------------------------------------------------------------------
constexpr int kMaxApply = 16;
struct ApplyEntry { const char* key; void (*fn)(long); long last; bool has; };
inline ApplyEntry g_apply[kMaxApply];
inline std::atomic<int> g_applyN{0};
inline std::atomic<unsigned> g_applyGen{0};
// The back end registers the function that makes a kApply key's value (once, at its init, before any flip). Several keys may share one function (it then re-reads them all).
inline void SetApply(const char* key, void (*fn)(long)) {
    const int i = g_applyN.load(std::memory_order_relaxed);
    if (i >= kMaxApply) return;
    g_apply[i] = {key, fn, 0, false};
    g_applyN.store(i + 1, std::memory_order_release);
}
// One relaxed load per scene start. `seen` is the render thread's own counter. Makes the value of every registered key that MSC_AB is in charge of and that changed since it was last made.
inline void RenderTick(unsigned& seen) {
    const unsigned g = g_applyGen.load(std::memory_order_relaxed);
    if (__builtin_expect(g == seen, 1)) return;
    seen = g;
    const int n = g_applyN.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        long v;
        if (!Lookup(g_apply[i].key, &v)) continue;
#if !defined(VGX_AB_BREAK) || VGX_AB_BREAK != 4   // (test mutant 4: a kApply value is made again on every request)
        if (g_apply[i].has && g_apply[i].last == v) continue;
#endif
        g_apply[i].last = v;
        g_apply[i].has = true;
        g_apply[i].fn(v);
    }
}

// ---- the driver -------------------------------------------------------------------------------------------------------------------------------
inline Spec g_spec;
inline std::atomic<int> g_on{0};
inline bool Active() { return g_on.load(std::memory_order_relaxed) != 0; }
inline bool Requested() { const char* e = std::getenv("MSC_AB"); return e != nullptr && *e != 0; }
// Sets the value key `ki` answers from now on and brings the knobs along. Returns the number of knob words re-stored.
inline int SetValue(int ki, long v) {
    g_ov.val[ki].store(v, std::memory_order_relaxed);
    const int n = Reread();
#if !defined(VGX_AB_BREAK) || VGX_AB_BREAK != 3   // (test mutant 3: a kApply flip is never requested)
    const KnobInfo* ki2 = Find(g_ov.key[ki]);
    if (ki2 && ki2->kind == kApply) g_applyGen.fetch_add(1, std::memory_order_release);
#endif
    return n;
}
// Called once after the environment is applied (vgx_state.cpp GxdInit). Idempotent.
inline void Start() {
    static std::atomic<int> done{0};
    if (done.exchange(1)) return;
    const char* e = std::getenv("MSC_AB");
    if (!e || !*e) return;
    if (!std::strcmp(e, "list")) {
        for (const KnobInfo& k : kKnobs) std::fprintf(stderr, "[ab] MSC_AB key %s (%s): %s\n", k.key, k.kind == kKnob ? "knob" : k.kind == kCall ? "call" : "apply", k.why);
        for (const Refusal& r : kRefused) std::fprintf(stderr, "[ab] MSC_AB key %s is refused: %s\n", r.key, r.why);
        return;
    }
    Spec s;
    char err[96];
    if (!ParseSpec(e, s, err, sizeof err)) { std::fprintf(stderr, "[ab] MSC_AB=%s: %s: off\n", e, err); return; }
    for (int i = 0; i < s.n; ++i) {
        if (Find(s.k[i].key)) continue;
        const char* why = RefusalOf(s.k[i].key);
        std::fprintf(stderr, "[ab] MSC_AB=%s: %s is not in the flip-safe registry (%s): off (MSC_AB=list names the keys)\n", e, s.k[i].key, why ? why : "not audited");
        return;
    }
    if (const char* x = std::getenv("MSC_AB_START_S")) { const double d = std::atof(x); if (d >= 0) s.startS = d; }
    if (const char* x = std::getenv("MSC_AB_SETTLE_S")) { const double d = std::atof(x); if (d >= 0) s.settleS = d; }
    if (const char* x = std::getenv("MSC_AB_REPEATS")) { const int r = std::atoi(x); if (r >= 1 && r <= 8) s.repeats = r; }
    for (int i = 0; i < s.n; ++i) if (s.settleS >= s.k[i].windowS) s.settleS = 0.25 * s.k[i].windowS;
    g_spec = s;
    for (int i = 0; i < s.n; ++i) { std::snprintf(g_ov.key[i], sizeof g_ov.key[i], "%s", s.k[i].key); g_ov.val[i].store(s.k[i].v[0], std::memory_order_relaxed); }
    g_ov.n.store(s.n, std::memory_order_release);
    int n = 0;
    for (int i = 0; i < s.n; ++i) n += SetValue(i, s.k[i].v[0]);
    g_on.store(1, std::memory_order_release);
    std::fprintf(stderr, "[ab] MSC_AB: %d key(s), %d b window(s) each, %.1f s settle after every flip, first flip after %.1f s of play, the plan takes %.0f s of play; %d knob word(s) of %d registered re-read\n", s.n,
                 s.repeats, s.settleS, s.startS, TotalS(s), n, std::min<int>(g_regN.load(), kMaxReg));
    for (int i = 0; i < s.n; ++i)
        std::fprintf(stderr, "[ab]   %s = %ld (a) / %ld (b), windows of %.1f s (%s)\n", s.k[i].key, s.k[i].v[0], s.k[i].v[1], s.k[i].windowS, Find(s.k[i].key)->kind == kKnob ? "knob" : Find(s.k[i].key)->kind == kCall ? "call" : "apply");
}
struct Cut {
    bool boundary = false;
    Phase closed;   // the phase whose pictures end here (valid when boundary)
};
// The front-end thread, once a picture, with the clock in microseconds (32-bit, the console's process time: unwrapped here). Returns where a window ended.
class Driver {
  public:
    Cut OnPicture(uint32_t now32) {
        Cut c;
        if (now32 < last32_) wraps_ += 1ull << 32;
        last32_ = now32;
        const uint64_t now = wraps_ + now32;
        if (!started_) { started_ = true; t0_ = now; cur_ = Phase(); return c; }
        const Phase p = PhaseAt(g_spec, static_cast<double>(now - t0_) * 1e-6);
        if (p != cur_) {
            c.boundary = true;
            c.closed = cur_;
            if (p.ki >= 0 && p.ki < g_spec.n && p.arm != cur_.arm && cur_.ki == p.ki) {
                const int n = SetValue(p.ki, g_spec.k[p.ki].v[p.arm]);
                std::fprintf(stderr, "[ab] flip to %c (%s = %ld) at %.1f s, window %d (%d knob word(s) re-read)\n", p.arm ? 'b' : 'a', g_spec.k[p.ki].key, g_spec.k[p.ki].v[p.arm],
                             static_cast<double>(now - t0_) * 1e-6, p.win, n);
            } else if (p.ki >= g_spec.n && cur_.ki < g_spec.n) {
                std::fprintf(stderr, "[ab] plan finished at %.1f s: every key is back at a\n", static_cast<double>(now - t0_) * 1e-6);
            }
            cur_ = p;
        }
        return c;
    }
    Phase Current() const { return cur_; }

  private:
    bool started_ = false;
    uint32_t last32_ = 0;
    uint64_t wraps_ = 0, t0_ = 0;
    Phase cur_;
};
inline Driver g_drv;

}  // namespace ab
}  // namespace vgx
