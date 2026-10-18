// A scripted fake of the game's state for the unit tests of the harness core: boots through the
// scene sequence of the nav scripts (boot loading -> title -> main menu -> ... -> match) at fixed
// frame numbers, with a button list and a hover position that follow the pad the harness feeds it.
#include "../src/np_harness_state.h"
#include "np_harness.h"
#include <cmath>
#include <cstdio>

namespace NpHarness {
namespace {
struct Mock {
    uint64_t frame = 0;
    int onButton = 0;
    uint32_t padPrev = 0;
};
Mock g;
}
// The test drives time by calling this before each NpOnFramePresented().
void MockSetFrame(uint64_t f) { g.frame = f; }

void SampleState(State& s, std::vector<Button>* buttons, float* pointer) {
    const uint64_t f = g.frame;
    s = State{};
    auto push = [&](SceneStack& st, int32_t id) { st.ids[st.depth++] = id; };
    if (f < 10) push(s.scene, 18);                            // boot loading
    else if (f < 20) { push(s.scene, 0); }                    // title
    else if (f < 40) { push(s.scene, 1); }                    // main menu
    else if (f < 50) { push(s.scene, 1); push(s.scene, 27); } // options
    else { push(s.scene, 5); }
    s.scene.top = s.scene.depth ? 0x1000 : 0;
    s.taskState = f < 60 ? 4 : 2;
    s.mode = f >= 40 ? 0 : -1;
    s.game = f >= 60 ? 0x2000 : 0;
    s.gameState = f >= 60 ? (f < 65 ? 1 : 5) : -1;
    s.clock = f >= 65 ? static_cast<float>(f - 65) * 0.2f : 0.0f;
    s.duration = 180;
    s.simFrame = f >= 65 ? static_cast<uint32_t>((f - 65) * 10) : 0;
    s.simTime = s.simFrame * 0.02f;
    s.simStarted = f >= 65;
    s.score[0] = f >= 90 ? 1 : 0;
    s.ball[0] = std::sin(f * 0.1f); s.ball[1] = std::cos(f * 0.1f); s.ball[2] = 0.42f;
    s.nButtons = (f >= 20 && f < 60) ? 8 : 0;
    s.onCtx = s.nButtons ? 0 : -1;
    s.onOff = s.nButtons ? 0x1470 : -1;
    s.paused = f >= 100 && f < 110;
    s.retrace = static_cast<uint32_t>(f);
    if (buttons) {
        buttons->clear();
        for (int i = 0; i < s.nButtons; ++i) { Button b; b.listener = 0x1000 + 0x100 * i; b.ctx = i; b.hover = i == 0; buttons->push_back(b); }
    }
    if (pointer) { pointer[0] = 320; pointer[1] = 240; }
}
std::string SceneName(int32_t id) { char b[16]; std::snprintf(b, sizeof b, "scene%d", id); return b; }
std::string ClassName(uintptr_t vt) { char b[24]; std::snprintf(b, sizeof b, "%08lX", (unsigned long)vt); return vt ? b : "-"; }
bool SafeRead(uintptr_t addr, void* out, size_t size) { if (addr < 0x10000 || size > 8) return false; std::memcpy(out, reinterpret_cast<void*>(addr), size); return true; }
bool ResolveSymbol(const char* name, uintptr_t* addr) {
    static float testVar = 12.5f;
    if (!std::strcmp(name, "testVar")) { *addr = reinterpret_cast<uintptr_t>(&testVar); return true; }
    return false;
}
bool DeepDigest(uint32_t* chk, uint32_t* rng) { *chk = 0; *rng = 0; return false; }
bool LaunchMatch(const LaunchSpec&, std::string& msg) { msg = "mock"; return false; }
bool PushScene(int, bool, std::string& msg) { msg = "mock"; return false; }
bool PopScene(std::string& msg) { msg = "mock"; return false; }
bool SetGlobalFloat(const char*, float, std::string& msg) { msg = "mock"; return false; }
uint64_t SimDigest() {
    State s; SampleState(s, nullptr, nullptr);
    uint64_t h = 1469598103934665603ull;
    h = (h ^ s.simFrame) * 1099511628211ull; h = (h ^ (uint64_t)s.gameState) * 1099511628211ull;
    return h;
}
} // namespace NpHarness
