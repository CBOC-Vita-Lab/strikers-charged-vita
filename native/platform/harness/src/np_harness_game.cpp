// np_harness_game.cpp: the harness's view of the game, read from the game's own globals.
//
// Replaces plat_headless.cpp's "guest memory" accessors: the recompiled build read the game's
// state at fixed guest addresses (symbols.txt of the decomp); here the same state is read through
// the C++ globals of the native build. The globals are declared WEAK with their Itanium-mangled
// names (asm labels), so that this file needs no game header (the decomp's headers need NPB's
// compat layer to compile with a host compiler at all) and the harness links into any program:
// a variable that is not there reads as "not available" (zero). Member offsets are the Wii's
// (MWCC) offsets the recompiled harness used, valid for the native build where the layout is
// the same; tests/layout_check.cpp includes the game's headers and asserts them (compile it with
// the game's flags once the headers build), and the weak extras below (ContainsPoint, FEBackButton's
// vtable) are found by symbol, not by slot, so they survive differences in vtable layout.
//
// ILP32 only: the Wii, armhf and the Vita are 32-bit, and so is host32. (A 64-bit syntax check:
// -DNP_HARNESS_ALLOW_64, never run.)
#include "np_harness_state.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#if !defined(NP_HARNESS_ALLOW_64)
static_assert(sizeof(void*) == 4, "the game's structure offsets are the 32-bit ones");
#endif
#if defined(__linux__)
#include <cxxabi.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#endif

// ---- the game's globals (weak: absent = reads as unavailable) -------------------------------
#define NP_WEAK_VAR(type, id, mangled) extern type id asm(mangled) __attribute__((weak))
NP_WEAK_VAR(void*, np_sceneMgr, "_ZN11nlSingletonI16GameSceneManagerE11s_pInstanceE");
NP_WEAK_VAR(void*, np_gameInfoMgr, "_ZN11nlSingletonI15GameInfoManagerE11s_pInstanceE");
NP_WEAK_VAR(void*, np_taskMgr, "_ZN13nlTaskManager11m_pInstanceE");
NP_WEAK_VAR(void*, np_overlayMgr, "g_pOverlayManager");
NP_WEAK_VAR(void*, np_game, "g_pGame");
NP_WEAK_VAR(void*, np_ball, "g_pBall");
NP_WEAK_VAR(void*, np_teams[2], "g_pTeams");
NP_WEAK_VAR(void*, np_feInput, "g_pFEInput");
NP_WEAK_VAR(void*, np_fePointerMgr, "g_pFEPointerManager");
NP_WEAK_VAR(float, np_fePointerPos[8], "gFEPointerPositions");
NP_WEAK_VAR(uint32_t, np_netRandomSeed, "gNetworkRandomSeed");
NP_WEAK_VAR(bool, np_paused, "_ZN8FrontEnd19m_bInPauseMenuStateE");
NP_WEAK_VAR(uint32_t, np_sceneEntryTable[], "SceneEntryTable");
NP_WEAK_VAR(uint32_t, np_vtableBackButton[], "_ZTV12FEBackButton");
extern "C++" uint32_t np_CalculateChecksum(void* self) asm("_ZN15FixedUpdateTask17CalculateChecksumEv") __attribute__((weak));
extern "C++" void* np_GetFixedUpdateTask() asm("_Z18GetFixedUpdateTaskv") __attribute__((weak));
extern "C++" void np_ContainsPoint() asm("_ZNK15FEPointerRegion13ContainsPointE9nlVector2") __attribute__((weak));
extern "C++" int np_glxGetSwapMode() asm("_Z14glxGetSwapModev") __attribute__((weak));
extern "C" uint32_t VIGetRetraceCount(void) __attribute__((weak));
// W5-SEED, script command `launch` (LaunchMatch below): the calls TitleScene::StartDemoMatch makes
extern "C++" void np_SetMode(void* self, int mode, bool flag) asm("_ZN15GameInfoManager7SetModeEib") __attribute__((weak));
extern "C++" void np_SetTeam(void* self, short side, int team) asm("_ZN15GameInfoManager7SetTeamEsi") __attribute__((weak));
extern "C++" void np_SetSidekick(void* self, short side, int sidekick, int slot) asm("_ZN15GameInfoManager11SetSidekickEsii") __attribute__((weak));
extern "C++" void np_SetStadium(void* self, int stadium) asm("_ZN15GameInfoManager10SetStadiumEi") __attribute__((weak));
extern "C++" void np_ResetPlayingSides(void* self) asm("_ZN15GameInfoManager17ResetPlayingSidesEv") __attribute__((weak));
extern "C++" void np_SetPlayingSide(void* self, unsigned short pad, short side) asm("_ZN15GameInfoManager14SetPlayingSideEts") __attribute__((weak));
extern "C++" void np_PushLoadingScene(void* self, bool popFirst) asm("_ZN20BaseGameSceneManager16PushLoadingSceneEb") __attribute__((weak));
extern "C++" void* np_PushScene(void* self, int scene, int movement, bool popFirst) asm("_ZN20BaseGameSceneManager4PushE9SceneList14ScreenMovementb") __attribute__((weak));
extern "C++" void np_PopScene(void* self) asm("_ZN20BaseGameSceneManager3PopEv") __attribute__((weak));
// W5-SEED, script command `setf`: the weather start chances of src/Game/Weather.cpp (percent per update; lightning 0.033, wind 0.01 by default, so a short match never meets them)
extern "C++" float gLightningStartChance __attribute__((weak));
extern "C++" float gWindStartChance __attribute__((weak));
extern "C++" float gLavaStartChance __attribute__((weak));
extern "C++" float gStormStartDelay __attribute__((weak));

// The address of a weak variable, hidden from the optimiser: GCC folds `&array != nullptr` to
// true even for a weak declaration, and an absent variable must read as absent.
template <typename T> static inline T* Opaque(T* p) { __asm__("" : "+r"(p)); return p; }

namespace NpHarness {
namespace {

constexpr uint32_t kSceneEntryCount = 0x358 / 8;     // SceneEntryTable[107] {SceneList id, const char* fen}

// Plain reads of the game's own memory. Only objects the game itself holds pointers to are
// followed, on the game thread at a frame end; a pointer is checked for null, alignment and a
// sane range, not probed (the menu list below walks hundreds of nodes per frame).
inline bool Plausible(uintptr_t a, unsigned align = 4) { return a >= 0x10000 && (a & (align - 1)) == 0; }
inline uint32_t R32(uintptr_t a) { return Plausible(a) ? *reinterpret_cast<const volatile uint32_t*>(a) : 0; }
inline uint8_t R8(uintptr_t a) { return Plausible(a, 1) ? *reinterpret_cast<const volatile uint8_t*>(a) : 0; }
inline float RF(uintptr_t a) { uint32_t w = R32(a); float f; std::memcpy(&f, &w, 4); return f; }
inline uintptr_t RP(uintptr_t a) { return static_cast<uintptr_t>(R32(a)); }

SceneStack ReadStack(uintptr_t mgr) {
    SceneStack s;
    if (!mgr) return s;
    const uint32_t depth = R32(mgr + 0x04);
    if (depth > 32) return s;
    s.depth = depth;
    for (uint32_t i = 0; i < depth; ++i) s.ids[i] = static_cast<int32_t>(R32(mgr + 0x08 + i * 4));
    if (depth) {
        s.top = RP(mgr + 0x88 + (depth - 1) * 4);
        s.topVtable = s.top ? RP(s.top) : 0;
    }
    return s;
}

// ---- class names ---------------------------------------------------------------------------
std::map<uintptr_t, std::string>& VtableFile() {
    static std::map<uintptr_t, std::string> names;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        if (const char* path = std::getenv("MSC_SYMBOLS")) {
            if (FILE* f = std::fopen(path, "r")) {
                char line[512];
                while (std::fgets(line, sizeof line, f)) {
                    // "<hex address> <name>" or "<hex address> <type> <name>" (nm -n)
                    char a[32], b[256], c[256];
                    const int n = std::sscanf(line, "%31s %255s %255s", a, b, c);
                    const char* name = n == 3 ? c : n == 2 ? b : nullptr;
                    if (!name || std::strncmp(name, "_ZTV", 4) != 0) continue;
                    const uintptr_t addr = static_cast<uintptr_t>(std::strtoul(a, nullptr, 16));
                    std::string cls = name + 4;       // _ZTV10SHMainMenu -> 10SHMainMenu
                    size_t i = 0;
                    while (i < cls.size() && cls[i] >= '0' && cls[i] <= '9') ++i;
                    names[addr + 8] = cls.substr(i);  // Itanium: the vptr points 8 bytes into the table
                }
                std::fclose(f);
            }
        }
    }
    return names;
}

} // namespace

std::string ClassName(uintptr_t vt) {
    if (!vt) return "-";
    auto& file = VtableFile();
    auto it = file.find(vt);
    if (it != file.end()) return it->second;
#if defined(__linux__)
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(vt), &info) && info.dli_sname && std::strncmp(info.dli_sname, "_ZTV", 4) == 0) {
        int status = 0;
        char* dem = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        std::string name = (status == 0 && dem) ? dem : info.dli_sname;
        std::free(dem);
        if (name.rfind("vtable for ", 0) == 0) name = name.substr(11);
        return name;
    }
#endif
    char buf[24];
    std::snprintf(buf, sizeof buf, "%08lX", static_cast<unsigned long>(vt));
    return buf;
}

std::string SceneName(int32_t id) {
    static std::map<int32_t, std::string> cache;
    auto it = cache.find(id);
    if (it != cache.end()) return it->second;
    std::string name;
    if (!Opaque(&np_sceneEntryTable[0])) return name;
    for (uint32_t i = 0; i < kSceneEntryCount; ++i) {
        if (static_cast<int32_t>(np_sceneEntryTable[i * 2]) != id) continue;
        const uintptr_t str = np_sceneEntryTable[i * 2 + 1];
        for (size_t k = 0; k < 48 && Plausible(str + k, 1); ++k) {
            const char c = static_cast<char>(R8(str + k));
            if (c == 0) break;
            name.push_back((c >= 32 && c < 127) ? c : '?');
        }
        const size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        break;
    }
    if (!name.empty()) cache[id] = name;   // the table is static data
    return name;
}

bool SafeRead(uintptr_t addr, void* out, size_t size) {
    if (size == 0 || size > 64 || addr < 0x10000) return false;
#if defined(__linux__)
    // write() reports EFAULT for an unreadable range instead of faulting: the bytes go through
    // a pipe and come back.
    static int fds[2] = {-1, -1};
    if (fds[0] < 0) {
        if (pipe(fds) != 0) { fds[0] = fds[1] = -1; return false; }
        fcntl(fds[1], F_SETFL, O_NONBLOCK);
        fcntl(fds[0], F_SETFL, O_NONBLOCK);
    }
    if (write(fds[1], reinterpret_cast<const void*>(addr), size) != static_cast<ssize_t>(size)) return false;
    return read(fds[0], out, size) == static_cast<ssize_t>(size);
#else
    // The Vita: user memory lies in 0x81000000..0xF0000000; anything else is refused.
    if (addr < 0x80000000u || addr + size > 0xF0000000u) return false;
    std::memcpy(out, reinterpret_cast<const void*>(addr), size);
    return true;
#endif
}

bool ResolveSymbol(const char* name, uintptr_t* addr) {
    struct Entry { const char* name; const void* p; };
    const Entry table[] = {
        {"g_pGame", Opaque(&np_game)},           {"g_pBall", Opaque(&np_ball)},
        {"g_pTeams", Opaque(&np_teams[0])},      {"g_pOverlayManager", Opaque(&np_overlayMgr)},
        {"g_pFEInput", Opaque(&np_feInput)},     {"g_pFEPointerManager", Opaque(&np_fePointerMgr)},
        {"g_pSceneManager", Opaque(&np_sceneMgr)}, {"g_pGameInfoManager", Opaque(&np_gameInfoMgr)},
        {"g_pTaskManager", Opaque(&np_taskMgr)}, {"gFEPointerPositions", Opaque(&np_fePointerPos[0])},
        {"FrontEnd_m_bInPauseMenuState", Opaque(&np_paused)},
    };
    for (const Entry& e : table)
        if (!std::strcmp(name, e.name)) {
            if (!e.p) return false;
            *addr = reinterpret_cast<uintptr_t>(e.p);
            return true;
        }
#if defined(__linux__)
    if (void* p = dlsym(RTLD_DEFAULT, name)) { *addr = reinterpret_cast<uintptr_t>(p); return true; }
#endif
    return false;
}

namespace {
// Live menu buttons for pad 0: the same test the port's menu navigation uses (pad.cpp,
// MenuNav::CollectButtons), read-only.
std::vector<Button> CollectButtons(float& px, float& py, const SceneStack& scene) {
    (void)scene;
    std::vector<Button> out;
    px = Opaque(&np_fePointerPos[0]) ? np_fePointerPos[0] : 0.0f;
    py = Opaque(&np_fePointerPos[0]) ? np_fePointerPos[1] : 0.0f;
    const uintptr_t mgr = Opaque(&np_fePointerMgr) ? reinterpret_cast<uintptr_t>(np_fePointerMgr) : 0;
    const uintptr_t input = Opaque(&np_feInput) ? reinterpret_cast<uintptr_t>(np_feInput) : 0;
    const bool locked = input && R32(input + 0x24) != 0;
    const uintptr_t containsPoint = reinterpret_cast<uintptr_t>(Opaque(&np_ContainsPoint));
    const uintptr_t backVtable = Opaque(&np_vtableBackButton[0]) ? reinterpret_cast<uintptr_t>(&np_vtableBackButton[0]) + 8 : 0;
    uintptr_t entry = mgr ? RP(mgr + 4) : 0;
    for (int guard = 0; entry && guard < 512; ++guard, entry = RP(entry)) {
        const uintptr_t l = RP(entry + 4);
        if (!l) continue;
        const uintptr_t vt = RP(l);
        // Is it a FEPointerRegion? Its vtable holds ContainsPoint; the slot depends on the ABI's
        // vtable layout, so look through the first slots.
        bool region = false;
        if (containsPoint && Plausible(vt))
            for (int slot = 0; slot < 24 && !region; ++slot) region = RP(vt + slot * 4) == containsPoint;
        if (!region) continue;
        if (R8(l + 0x80)) continue;                 // mDisabled
        if (locked && !R8(l + 0x81)) continue;      // !mIgnoreInputLock
        // Fed a pointer event for pad 0 at the pointer's current position: shown this frame.
        if (static_cast<int32_t>(R32(l + 0x3C)) != 0 || std::fabs(RF(l + 0x40) - px) > 1.0f ||
            std::fabs(RF(l + 0x44) - py) > 1.0f) continue;
        Button b{};
        b.listener = l; b.vtable = vt; b.ctx = R32(l + 0x7C);
        b.minX = RF(l + 0x84); b.maxX = RF(l + 0x88); b.maxY = RF(l + 0x8C); b.minY = RF(l + 0x90);
        if (!(b.maxX > b.minX && b.maxY > b.minY) || b.maxX - b.minX > 600.0f || b.maxY - b.minY > 420.0f) continue;
        const float rot = RF(l + 0x94), pvx = RF(l + 0x98), pvy = RF(l + 0x9C);
        b.cx = (b.minX + b.maxX) * 0.5f; b.cy = (b.minY + b.maxY) * 0.5f;
        float tx = px, ty = py;
        if (rot != 0.0f) {
            const float c = std::cos(rot), sn = std::sin(rot);
            const float dx = b.cx - pvx, dy = b.cy - pvy;
            b.cx = pvx + dx * c - dy * sn; b.cy = pvy + dx * sn + dy * c;
            const float lx = px - pvx, ly = py - pvy;
            tx = lx * c + ly * sn + pvx; ty = -lx * sn + ly * c + pvy;
        }
        b.hover = tx >= b.minX && tx <= b.maxX && ty >= b.minY && ty <= b.maxY;
        b.back = backVtable && vt == backVtable;
        if (std::isfinite(b.cx) && std::isfinite(b.cy)) out.push_back(b);
    }
    return out;
}
} // namespace

void SampleCoreState(State& s) {
    const uintptr_t sceneMgr = Opaque(&np_sceneMgr) ? reinterpret_cast<uintptr_t>(np_sceneMgr) : 0;
    const uintptr_t overlayMgr = Opaque(&np_overlayMgr) ? reinterpret_cast<uintptr_t>(np_overlayMgr) : 0;
    s.scene = ReadStack(sceneMgr);
    s.overlay = ReadStack(overlayMgr);
    const uintptr_t tm = Opaque(&np_taskMgr) ? reinterpret_cast<uintptr_t>(np_taskMgr) : 0;
    s.taskState = tm ? R32(tm + 0x08) : 0;
    s.dtMs = tm ? RF(tm + 0x18) * 1000.0f : 0.0f;
    const uintptr_t gim = Opaque(&np_gameInfoMgr) ? reinterpret_cast<uintptr_t>(np_gameInfoMgr) : 0;
    s.mode = gim ? static_cast<int32_t>(R32(gim + 0x11C)) : -1;
    const uintptr_t info = (gim && s.mode >= 0 && s.mode < 5) ? RP(gim + 0x80 + static_cast<uint32_t>(s.mode) * 4) : 0;
    s.team[0] = info ? static_cast<int32_t>(R32(info)) : -1;          // BasicGameInfo::mTeamIndex[2]
    s.team[1] = info ? static_cast<int32_t>(R32(info + 4)) : -1;
    s.stadium = info ? static_cast<int32_t>(R32(info + 0x20)) : -1;   // mStadiumIndex
    s.game = Opaque(&np_game) ? reinterpret_cast<uintptr_t>(np_game) : 0;
    s.gameState = s.game ? static_cast<int32_t>(R32(s.game + 0x18)) : -1;
    s.duration = s.game ? RF(s.game + 0x1C) : 0.0f;
    const uintptr_t clock = s.game ? RP(s.game + 0x08) : 0;   // cGame::m_pGameClock
    s.clock = clock ? RF(clock + 0x08) : 0.0f;                // Clock::m_fTimer
    s.clockState = clock ? static_cast<int32_t>(R32(clock + 0x18)) : -1;
    for (int i = 0; i < 2; ++i) {
        const uintptr_t team = (s.game && Opaque(&np_teams[0])) ? reinterpret_cast<uintptr_t>(np_teams[i]) : 0;
        s.score[i] = team ? static_cast<int32_t>(R32(team + 0x04)) : 0;  // cTeam::m_nScore
    }
    const uintptr_t fixed = Opaque(&np_GetFixedUpdateTask) ? reinterpret_cast<uintptr_t>(np_GetFixedUpdateTask()) : 0;
    s.simFrame = fixed ? R32(fixed + 0x34) : 0;
    s.simTime = fixed ? RF(fixed + 0x2C) : 0.0f;
    s.simStarted = fixed ? R8(fixed + 0x38) != 0 : false;
    s.timeScale = fixed ? RF(fixed + 0x60) : 0.0f;
    s.paused = Opaque(&np_paused) ? *reinterpret_cast<volatile bool*>(&np_paused) : false;
    s.retrace = Opaque(&VIGetRetraceCount) ? VIGetRetraceCount() : 0;
    s.swapMode = Opaque(&np_glxGetSwapMode) ? static_cast<uint32_t>(np_glxGetSwapMode()) : 0;
    const uintptr_t ball = (s.game && Opaque(&np_ball)) ? reinterpret_cast<uintptr_t>(np_ball) : 0;
    for (int i = 0; i < 3; ++i) s.ball[i] = ball ? RF(ball + 0x54 + i * 4) : 0.0f;  // m_v3Position
    s.ballOwner = ball ? RP(ball + 0xC8) : 0;
    const uintptr_t input = Opaque(&np_feInput) ? reinterpret_cast<uintptr_t>(np_feInput) : 0;
    s.inputLock = input ? R32(input + 0x24) : 0;
}

void SampleState(State& s, std::vector<Button>* buttonsOut, float* ptr) {
    SampleCoreState(s);
    float px = 0, py = 0;
    std::vector<Button> buttons = CollectButtons(px, py, s.scene);
    s.nButtons = static_cast<int>(buttons.size());
    s.onCtx = -1;
    s.onOff = -1;
    for (const Button& b : buttons) if (b.hover && !b.back) { s.onCtx = static_cast<int>(b.ctx); break; }
    for (const Button& b : buttons) {
        if (!b.hover) continue;
        const uint32_t off = static_cast<uint32_t>(b.listener - s.scene.top);
        s.onOff = (s.scene.top && off < 0x10000) ? static_cast<int32_t>(off) : 0x7FFFFFFF;
        break;
    }
    if (ptr) { ptr[0] = px; ptr[1] = py; }
    if (buttonsOut) *buttonsOut = std::move(buttons);
}

bool DeepDigest(uint32_t* chk, uint32_t* rng) {
    *chk = 0; *rng = 0;
    if (Opaque(&np_netRandomSeed)) *rng = np_netRandomSeed;
    if (!Opaque(&np_CalculateChecksum) || !Opaque(&np_GetFixedUpdateTask) || !Opaque(&np_game) || !np_game || !Opaque(&np_ball) || !np_ball) return false;
    if (!Opaque(&np_teams[0]) || !np_teams[0] || !np_teams[1]) return false;
    void* task = np_GetFixedUpdateTask();
    if (!task || !R8(reinterpret_cast<uintptr_t>(task) + 0x38)) return false;   // mSimulationStarted
    for (int t = 0; t < 2; ++t) for (int i = 0; i < 5; ++i) if (!R32(reinterpret_cast<uintptr_t>(np_teams[t]) + 0xA4 + 4 * i)) return false;
    *chk = np_CalculateChecksum(task);
    return true;
}

// ---- W5-SEED: start a match from the script (the shader-key sweeps need every stadium / captain / cheat without walking the menus)
// The sequence is TitleScene::StartDemoMatch's (SH/SHTitleScreen.cpp): SetMode, SetStadium, SetTeam, SetSidekick, ResetPlayingSides,
// PushLoadingScene(true) (which pops the scene on top, normally the title). Called from the harness at a frame end on the game thread.
// GameInfoManager offsets (include/Game/GameInfo.h, the Wii's): mUserInfo 0x9C { mGameplayOptions +0x24 {SkillLevel +0, GameTime +8},
// mCheatOptions +0x40 {mCustomPowerups, mEnvironmentCheat, mPlayerCheat} }.
bool LaunchMatch(const LaunchSpec& l, std::string& msg) {
    void* gim = Opaque(&np_gameInfoMgr) ? np_gameInfoMgr : nullptr;
    void* scm = Opaque(&np_sceneMgr) ? np_sceneMgr : nullptr;
    if (!gim || !scm || !Opaque(&np_SetMode) || !Opaque(&np_SetStadium) || !Opaque(&np_SetTeam) || !Opaque(&np_SetSidekick) || !Opaque(&np_ResetPlayingSides) ||
        !Opaque(&np_SetPlayingSide) || !Opaque(&np_PushLoadingScene)) {
        msg = std::string("the game's GameInfoManager / GameSceneManager functions are not linked in (missing:") + (gim ? "" : " gameInfoMgr") + (scm ? "" : " sceneMgr") +
              (Opaque(&np_SetMode) ? "" : " SetMode") + (Opaque(&np_SetStadium) ? "" : " SetStadium") + (Opaque(&np_SetTeam) ? "" : " SetTeam") +
              (Opaque(&np_SetSidekick) ? "" : " SetSidekick") + (Opaque(&np_ResetPlayingSides) ? "" : " ResetPlayingSides") + (Opaque(&np_SetPlayingSide) ? "" : " SetPlayingSide") +
              (Opaque(&np_PushLoadingScene) ? "" : " PushLoadingScene") + ")";
        return false;
    }
    if (l.mode < 0 || l.mode > 4) { msg = "mode out of range"; return false; }
    np_SetMode(gim, l.mode, false);
    if (l.stadium >= 0) np_SetStadium(gim, l.stadium);
    if (l.home >= 0) np_SetTeam(gim, 0, l.home);
    if (l.away >= 0) np_SetTeam(gim, 1, l.away);
    for (int i = 0; i < 3; ++i) {
        if (l.hsk[i] >= 0) np_SetSidekick(gim, 0, l.hsk[i], i);
        if (l.ask[i] >= 0) np_SetSidekick(gim, 1, l.ask[i], i);
    }
    np_ResetPlayingSides(gim);
    if (l.human >= 0) np_SetPlayingSide(gim, 0, static_cast<short>(l.human));
    uint8_t* g = static_cast<uint8_t*>(gim);
    auto put = [&](size_t off, int v) { if (v >= 0) *reinterpret_cast<int32_t*>(g + off) = v; };
    put(0x9C + 0x40, l.powerup); put(0x9C + 0x40 + 4, l.env); put(0x9C + 0x40 + 8, l.player);
    put(0x9C + 0x24, l.skill); put(0x9C + 0x24 + 8, l.time);
    if (l.push) np_PushLoadingScene(scm, true);
    msg = l.push ? "loading scene pushed" : "state set, nothing pushed";
    return true;
}

bool PushScene(int id, bool popFirst, std::string& msg) {
    void* scm = Opaque(&np_sceneMgr) ? np_sceneMgr : nullptr;
    if (!scm || !Opaque(&np_PushScene)) { msg = "BaseGameSceneManager::Push is not linked in"; return false; }
    if (id < 0 || id > 105) { msg = "scene id out of range"; return false; }
    np_PushScene(scm, id, 1 /* SCREEN_FORWARD */, popFirst);
    msg = "scene pushed";
    return true;
}
bool PopScene(std::string& msg) {
    void* scm = Opaque(&np_sceneMgr) ? np_sceneMgr : nullptr;
    if (!scm || !Opaque(&np_PopScene)) { msg = "BaseGameSceneManager::Pop is not linked in"; return false; }
    np_PopScene(scm);
    msg = "scene popped";
    return true;
}

bool SetGlobalFloat(const char* name, float value, std::string& msg) {
    struct Entry { const char* name; float* addr; };
    const Entry table[] = {{"gLightningStartChance", Opaque(&gLightningStartChance)}, {"gWindStartChance", Opaque(&gWindStartChance)},
                           {"gLavaStartChance", Opaque(&gLavaStartChance)}, {"gStormStartDelay", Opaque(&gStormStartDelay)}};
    for (const Entry& e : table) {
        if (std::strcmp(e.name, name) != 0) continue;
        if (!e.addr) { msg = std::string(name) + " is not linked in"; return false; }
        *e.addr = value;
        msg = "set";
        return true;
    }
    msg = std::string("unknown global ") + name;
    return false;
}

uint64_t SimDigest() {
    State s;
    SampleState(s, nullptr, nullptr);
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; h ^= h >> 29; };
    auto bits = [](float f) { uint32_t w; std::memcpy(&w, &f, 4); return static_cast<uint64_t>(w); };
    mix(s.simFrame); mix(bits(s.simTime)); mix(bits(s.clock)); mix(static_cast<uint32_t>(s.gameState));
    mix(static_cast<uint32_t>(s.score[0])); mix(static_cast<uint32_t>(s.score[1]));
    mix(s.scene.depth); for (uint32_t i = 0; i < s.scene.depth; ++i) mix(static_cast<uint32_t>(s.scene.ids[i]));
    mix(s.overlay.depth); for (uint32_t i = 0; i < s.overlay.depth; ++i) mix(static_cast<uint32_t>(s.overlay.ids[i]));
    for (float f : s.ball) mix(bits(f));
    mix(s.ballOwner ? 1 : 0);
    mix(s.taskState);
    return h;
}

} // namespace NpHarness
