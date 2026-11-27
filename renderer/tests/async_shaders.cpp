// 1.3 MSC_VGX_ASYNC_SHADERS: the shader compile worker's queue and rules (vgx_async_shaders.h), then the real GL back end over the stub GL with
// the host simulation (MSC_VGX_ASYNC_SHADERS_SIM=<ms>: a shader met after the warm-up frames is "missing from the cache" and the worker "compiles"
// it by sleeping that long). Draws are counted at glDrawElements.
//   async_shaders model        the pure rules and the queue with a real worker thread
//   async_shaders backend      (SIM=60) skippable draw left out then drawn; depth-only draw and a draw before an EFB copy drawn in their frame;
//                              the pack case (a shader the "cache" has): no job, no skip
//   async_shaders age          (SIM=1000) a skippable draw is left out for at most ~400 ms, then the blocking path waits for the worker
//   async_shaders off          (MSC_VGX_ASYNC_SHADERS=0) every new shader is drawn in its frame, no worker
// Exit 0 = every check passed. -DVGX_ASYNC_BREAK=1..4 must fail (run_async_shaders_test.sh).
#include <GLES2/gl2.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {
unsigned g_drawCalls = 0;
void TestDrawElements(GLenum, GLsizei, GLenum, const GLvoid*) { ++g_drawCalls; }
int g_fails = 0;
void Check(bool ok, const char* message) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", message);
    if (!ok) ++g_fails;
}
}
#define glDrawElements TestDrawElements
#include "../src/vgx_backend_gl.cpp"
#undef glDrawElements
#include "backend_test_hooks.h"

namespace vgx { bool LogEnabled() { return false; } const char* ShaderListPath() { return ""; } }   // (ShaderListPath: vgx_draw.cpp, not linked here)
extern "C" int VgxStillArmed(void) { return 0; }

using namespace vgx::async;
namespace asyncsh = vgx::backend::asyncsh;

static int ModelMain() {
    std::puts("-- rules");
    Check(Decide(false, true, 0) == Action::CompileNow && Decide(false, false, 0) == Action::CompileNow, "nothing pending: compile / load now");
    Check(Decide(true, true, 10) == Action::WaitThenSkip, "pending, skippable, young: wait then skip");
    Check(Decide(true, false, 0) == Action::Block, "pending, must not be skipped: block");
    Check(Decide(true, true, kMaxSkipAgeMs + 1) == Action::Block, "pending, skippable, older than the age bound: block");
    DrawClass plain;
    plain.writesColor = true;
    Check(Skippable(plain), "a colour draw of the main scene may be skipped");
    DrawClass d = plain; d.inPass = true;
    Check(!Skippable(d), "a render-target pass draw may not");
    d = plain; d.copyLater = true;
    Check(!Skippable(d), "a draw an EFB copy of the same frame may read may not");
    d = plain; d.writesColor = false;
    Check(!Skippable(d), "a depth-only draw may not");
    d = plain; d.fxTarget = true;
    Check(!Skippable(d), "an effects-target draw may not");
    d = plain; d.warmup = true;
    Check(!Skippable(d), "a warm-up / other caller may not");

    std::puts("-- queue with a worker thread (each compile 30 ms)");
    Queue q;
    std::thread worker([&] { q.Run([](uint32_t, const std::string&) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); return true; }); });
    const uint32_t a = ShaderKey(false, 1), b = ShaderKey(true, 1), c = ShaderKey(true, 2);
    Check(q.Submit(a, "a") && q.Submit(b, "b") && q.Submit(c, "c"), "three jobs queued");
    Check(!q.Submit(a, "a"), "a key already queued is not queued again");
    Check(!q.Wait(&a, 1, 1.0), "a 1 ms wait does not see a 30 ms compile finish");
    Check(q.Cancel(c), "a job not started yet is taken back");
    Check(q.State(c) == JobState::None, "and is known no more");
    const uint32_t ab[2] = {a, b};
    Check(q.Wait(ab, 2, -1.0), "an unbounded wait returns when both are done");
    Check(q.State(a) == JobState::Done && q.State(b) == JobState::Done, "both done");
    const Stats st = q.Snapshot();
    Check(st.compiled == 2 && st.cancelled == 1 && st.failed == 0, "two compiled, one taken back");
    Check(!q.Cancel(a), "a finished job cannot be taken back");
    q.Forget(a);
    Check(q.State(a) == JobState::None && q.Submit(a, "a"), "forgotten: may be queued again");
    Check(q.Wait(&a, 1, 1000.0), "and is compiled again");
    q.Stop();
    worker.join();
    return g_fails ? 1 : 0;
}

// The back end over the stub GL. Frame 1 is the warm-up (vertex shader 0, fragment shader 0, one draw); frames to 9 draw it again (the warm-up
// frames end at 8). Each later case is a new fragment shader met in a running game (CMD_SHADER), drawn with vertex shader 0.
namespace {
std::vector<vgx::ShaderSource*> g_src;
uint32_t g_frame = 0;
vgx::ShaderSource* Fs(uint16_t id) {
    auto* s = new vgx::ShaderSource;
    s->fragment = true;
    s->id = id;
    s->text = "precision mediump float; void main() { gl_FragColor = vec4(" + std::to_string(id) + ".0); }\n";
    g_src.push_back(s);
    return s;
}
// One frame: optional CMD_SHADER of `fresh`, draws of (0, fs) for each entry of `draws`, optional EFB copy at the end. Returns the draws made.
unsigned Frame(const vgx::ShaderSource* fresh, const std::vector<std::pair<uint16_t, bool>>& draws, int copyAfter = 0, const vgx::ShaderSource* vs0 = nullptr) {
    vgx::CmdBuf b;
    b.frame = ++g_frame;
    b.scene = 1;
    b.vtx.assign(256, 0);
    b.idx = {0, 1, 2};
    b.targets.push_back(vgx::SetTargetCmd{});
    b.cmds.push_back(vgx::Cmd{vgx::CMD_SET_TARGET, 0});
    if (vs0) { b.shaders.push_back(vs0); b.cmds.push_back(vgx::Cmd{vgx::CMD_SHADER, static_cast<uint32_t>(b.shaders.size() - 1)}); }
    if (fresh) { b.shaders.push_back(fresh); b.cmds.push_back(vgx::Cmd{vgx::CMD_SHADER, static_cast<uint32_t>(b.shaders.size() - 1)}); }
    for (const auto& d : draws) {
        vgx::DrawRec r;
        r.vsId = 0; r.fsId = d.first; r.vtxCount = 3; r.idxCount = 3;
        r.rs.colorWrite = d.second ? 1 : 0;
        r.rs.alphaWrite = d.second ? 1 : 0;
        r.rs.depthTest = 1;
        r.rs.depthWrite = 1;   // (a draw that writes nothing at all is dropped earlier: MSC_VGX_PX_NOOP)
        b.draws.push_back(r);
        b.cmds.push_back(vgx::Cmd{vgx::CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1)});
    }
    if (copyAfter) {   // 1: an EFB copy to a texture, then a clear-only copy; 2: only the clear-only copy
        vgx::CopyTexCmd efb;   // (texture 0 does not exist: CopyTex has nothing to write; the rule sees an EFB read)
        vgx::CopyTexCmd clearOnly;
        clearOnly.skip = true;   // (only its clear: reads nothing a draw wrote)
        if (copyAfter == 1) { b.copyTex.push_back(efb); b.cmds.push_back(vgx::Cmd{vgx::CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size() - 1)}); }
        b.copyTex.push_back(clearOnly);
        b.cmds.push_back(vgx::Cmd{vgx::CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size() - 1)});
    }
    b.cmds.push_back(vgx::Cmd{vgx::CMD_PRESENT, 0});
    const unsigned before = g_drawCalls;
    vgx::backend::Execute(b);
    return g_drawCalls - before;
}
vgx::ShaderSource* WarmUp() {
    auto* vs = new vgx::ShaderSource;
    vs->fragment = false; vs->id = 0; vs->text = "attribute vec4 a_pos; void main() { gl_Position = a_pos; }\n";
    vgx::ShaderSource* fs0 = Fs(0);
    Check(Frame(fs0, {{0, true}}, false, vs) == 1, "warm-up frame: drawn (warm-up shaders compile on the render thread as before)");
    for (int f = 2; f <= 9; ++f) Frame(nullptr, {{0, true}});
    return vs;
}
double Now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

static int BackendMain() {
    WarmUp();
    std::puts("-- a new shader on a skippable draw (worker 60 ms, wait 2 ms)");
    Check(Frame(Fs(1), {{1, true}}) == 0, "frame of the miss: the draw is left out");
    const Stats s1 = asyncsh::St().q.Snapshot();
    Check(s1.skipped == 1 && s1.blockMust == 0, "counted as skipped, no blocking path");
    const double t0 = Now();
    unsigned frames = 0, drawn = 0;
    while (!drawn && Now() - t0 < 2000) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); drawn = Frame(nullptr, {{1, true}}); ++frames; }
    std::printf("  (drawn after %u more frames, %.0f ms)\n", frames, Now() - t0);
    Check(drawn == 1 && Now() - t0 < 300, "drawn once the worker is done (well before the age bound)");
    Check(asyncsh::St().q.Snapshot().compiled == 1, "one background compile");
    Check(Frame(nullptr, {{1, true}}) == 1, "and every frame after");

    std::puts("-- a new shader on a depth-only draw: never skipped");
    const Stats s15 = asyncsh::St().q.Snapshot();
    Check(Frame(Fs(2), {{2, false}}) == 1, "drawn in the frame of the miss");
    const Stats s2 = asyncsh::St().q.Snapshot();
    Check(s2.blockMust == s15.blockMust + 1 && s2.skipped == s15.skipped, "blocking path, no skip");

    std::puts("-- a new shader on a colour draw an EFB copy of the same frame comes after: never skipped");
    Check(Frame(Fs(3), {{3, true}}, 1) == 1, "drawn in the frame of the miss");
    Check(asyncsh::St().q.Snapshot().skipped == s2.skipped && asyncsh::St().q.Snapshot().blockMust == s2.blockMust + 1, "blocking path, no skip");

    std::puts("-- a new shader on a colour draw followed only by a clear-only copy: skippable");
    const Stats s3 = asyncsh::St().q.Snapshot();
    Check(Frame(Fs(5), {{5, true}}, 2) == 0, "left out in the frame of the miss (the copy reads nothing)");
    Check(asyncsh::St().q.Snapshot().skipped == s3.skipped + 1, "counted as skipped");

    std::puts("-- the pack case: a shader the cache already has");
    {
        vgx::ShaderSource* s = Fs(4);
        std::lock_guard<std::mutex> lk(asyncsh::St().simMtx);
        asyncsh::St().simCache.insert(asyncsh::SimHash(GL_FRAGMENT_SHADER, s->text));
    }
    const Stats before = asyncsh::St().q.Snapshot();
    Check(Frame(g_src.back(), {{4, true}}) == 1, "drawn in its frame");
    const Stats after = asyncsh::St().q.Snapshot();
    Check(after.found == before.found + 1 && after.compiled == before.compiled && after.skipped == before.skipped && after.blockMust == before.blockMust,
          "found in the cache: no job, no skip, no blocking path");
    std::printf("  stats: compiled %llu, skipped %llu, block %llu+%llu, own %llu, taken back %llu, found %llu/%llu\n", static_cast<unsigned long long>(after.compiled),
                static_cast<unsigned long long>(after.skipped), static_cast<unsigned long long>(after.blockMust), static_cast<unsigned long long>(after.blockAge),
                static_cast<unsigned long long>(after.waitedOwn), static_cast<unsigned long long>(after.cancelled), static_cast<unsigned long long>(after.found),
                static_cast<unsigned long long>(after.probes));
    return g_fails ? 1 : 0;
}

static int AgeMain() {
    WarmUp();
    std::puts("-- a new shader on a skippable draw, worker 1000 ms: the age bound");
    const double t0 = Now();
    unsigned drawn = Frame(Fs(1), {{1, true}});
    Check(drawn == 0, "frame of the miss: left out");
    double firstDrawn = -1;
    while (Now() - t0 < 3000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double tf = Now();
        if (Frame(nullptr, {{1, true}}) == 1) { firstDrawn = tf - t0; break; }
    }
    std::printf("  (first drawn in a frame that began %.0f ms after the miss)\n", firstDrawn);
    Check(firstDrawn >= kMaxSkipAgeMs - 30 && firstDrawn < kMaxSkipAgeMs + 100, "left out until the age bound, then the blocking path draws it");
    const Stats st = asyncsh::St().q.Snapshot();
    Check(st.blockAge >= 1 && st.waitedOwn >= 1, "blocking path (waited too long), waiting for the worker's own compile");
    return g_fails ? 1 : 0;
}

static int OffMain() {
    WarmUp();
    std::puts("-- MSC_VGX_ASYNC_SHADERS=0");
    Check(Frame(Fs(1), {{1, true}}) == 1, "a new shader is drawn in its frame");
    Check(asyncsh::St().mode == 0 && asyncsh::St().q.Snapshot().probes == 0, "no worker, no probe");
    return g_fails ? 1 : 0;
}

int main(int argc, char** argv) {
    const char* what = argc > 1 ? argv[1] : "model";
    int rc = 1;
    if (!std::strcmp(what, "model")) rc = ModelMain();
    else if (!std::strcmp(what, "backend")) rc = BackendMain();
    else if (!std::strcmp(what, "age")) rc = AgeMain();
    else if (!std::strcmp(what, "off")) rc = OffMain();
    std::printf("async_shaders %s: %s\n", what, rc ? "FAILED" : "PASS");
    std::fflush(stdout);
    std::_Exit(rc);   // (the worker thread is detached and may be waiting)
}
