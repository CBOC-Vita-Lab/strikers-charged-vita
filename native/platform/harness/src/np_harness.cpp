// np_harness.cpp: scripted runs of the native port (the harness core).
//
// The same environment interface and script language as the recompiled build's harness
// (one/vita/src/plat_headless.cpp, documented in tools/README-nav.md); the script engine, the
// condition evaluator and the [nav] / [watch] line formats are that file's code, ported, so that
// every nav script and every log tool (tools/nav_summary.py) of the recompiled build works on the
// native build unchanged. What differs: the state variables are read from the game's own globals
// (np_harness_game.cpp, symbols of the decomp) instead of guest addresses; the clock is
// np_time.h (np_vclock.cpp); the hooks are the functions of np_harness.h.
//
// Environment (Vita: ux0:data/mscharged/env.txt):
//   MSC_MAX_FRAMES=N       exit(0) after N presented frames
//   MSC_INPUT_SCRIPT=f     input script, see "Input scripts" below
//   MSC_FRAME_LOG=N        print a timing line every N frames (default 60 on Linux, 0 on the Vita)
//   MSC_QEMU_MARK=N        every N frames issue the frame-mark syscall the qemu plugin counts on
//                          (tools/qemu-plugin/framecount.c; harmless ENOSYS elsewhere)
//   MSC_DUMP_MAPS=1        print the executable mappings (profile symbolisation), Linux
//   MSC_WATCH=1            report game state: a "[nav]" line whenever a discrete state changes
//                          (scene stack, overlay stack, task state, match state, score, pause)
//                          and a "[watch]" line every MSC_WATCH_EVERY frames.
//                          Anything other than "1" is a list of extra variables,
//                          name:type:expr[,name:type:expr...], type one of u8 u16 u32 s32 f32,
//                          expr = hex | symbol | [expr] | expr+hex, where `symbol` is the address of
//                          a global (the built-in list: g_pGame g_pBall g_pTeams g_pOverlayManager
//                          g_pFEInput g_pFEPointerManager; on Linux, with -rdynamic, any dynamic
//                          symbol by its mangled name) and [expr] reads the 32-bit word there:
//                          clk:f32:[[g_pGame]+8]+8
//   MSC_WATCH_EVERY=N      period of the "[watch]" line (default 60)
//   MSC_FRAME_TRACE=1      one short line per presented frame: "[ft] <frame> <sim steps so far>
//                          <retraces so far> <match state>", to pair with per-frame qemu marks
//   MSC_WATCH_BUTTONS=1    list the live menu buttons (pointer regions) whenever they change
//   MSC_SYMBOLS=f          symbol list for class names ("<hex address> <mangled or plain name>" per
//                          line, e.g. `nm -n` of the executable); without it Linux uses dladdr
//   MSC_STATE_HASH=N       print "[hash] f=.. sim=<digest of the simulation state> [ram=<hash of the
//                          registered regions>]" every N frames and at exit (determinism checks)
//   MSC_VIRTUAL_TIME=1     deterministic virtual clock (np_time.h)
//   MSC_VIRTUAL_TICK_NS=N  virtual nanoseconds per clock read on the game thread (default 1000)
//   MSC_VIRTUAL_FRAME_US=N virtual CPU cost charged per presented frame (default 0)
//   MSC_VIRTUAL_STEP_US=N  virtual CPU cost charged per simulation step (default 0)
//   MSC_FRAME_TIMING=1|120|300   frame interval statistics in windows (Codex's, as in the Vita layer)
//   MSC_SCRIPT_CHECK=1     with MSC_INPUT_SCRIPT: parse the script, print its size, exit (no game needed)
//
// Input scripts. Two kinds of line may be mixed; '#' starts a comment.
//   Absolute:    firstFrame lastFrame buttonsHex stickX stickY     (frames are presented frames)
//   Sequential:  executed in order, one command at a time, clocked by presented frames:
//     wait <cond> [timeoutFrames]               cond: <var> <op> <value>, several joined with
//                                               && or || (evaluated left to right); op: == != <
//                                               <= > >=. Default timeout 36000 frames; a timeout
//                                               prints "[script] TIMEOUT" and exits with code 3
//     if <cond> ... endif                       run the enclosed commands only if cond holds now
//     idle <frames>                             neutral controller
//     hold <frames> <buttonsHex> [sx sy [cx cy]]
//     press <buttonsHex> [frames=3] [gap=6]     hold, then neutral for <gap> frames
//     mash <buttonsHex> <period> <cond> [timeoutFrames]
//                                               press for 3 frames (D-pad only: 1 frame) every
//                                               <period> frames until the condition holds
//     log <text>                                prints "[script] f=<frame> <text>"
//     prof begin|end                            profile-window marks for the qemu plugin (run it
//                                               with from=1000000000,to=1000000001)
//     setf <global> <value>                     (W5 SEED) set a weather start chance (gLightningStartChance, gWindStartChance,
//                                               gLavaStartChance, gStormStartDelay): short matches then meet the weather
//     launch key=value ...                      (W5 SEED) start a match from anywhere, as the title's demo does
//                                               (TitleScene::StartDemoMatch): mode=friendly|demo|cup|challenge|0..4
//                                               stadium= home= away= (stadium / captain ids) hsk=a,b,c ask=a,b,c
//                                               (sidekick ids, home / away) human=<pad 0's side> powerup= env= player=
//                                               (cheats) skill= time= (seconds); pushes the loading scene (push=0: only sets the state)
//     scene <id> [pop]                          (W5 SEED) push a front-end scene by SceneList id (pop = pop the top one first)
//     item <side> <type> [count] [set|add]      (W5 ITEM, needs MSC_ITEM_TEST=1) give a team an item: side 0|1|me, type green red spiny
//                                               freeze banana bobomb chomp mushroom star | mario peach dk bowser luigi daisy wario waluigi
//                                               bowserjr diddy yoshi petey | captain (the team's own) | 0..20; set (default) empties both
//                                               slots first, add fills the first free one. Rides the game's deterministic input stream
//                                               (np/itemtest.h): applied at the same simulation step on every console of a LAN match
//     tap <buttonsHex> <period> <cond> [timeout] (W5 ITEM) mash, but a timeout is logged and the script goes on
//     aim <target> <buttonsHex> <cond> [timeout] (W5 ITEM) until cond holds, hold the left stick that walks this console's controlled
//                                               fielder toward target opp|owner|goal|keeper|ball (and hold buttons); a timeout is
//                                               logged and the script goes on
//     popscene                                  (W5 SEED) pop the top scene
//     exit                                      stop the run (exit code 0)
//     stopwhen <cond>                           from here on, stop the run (exit code 0) as soon
//                                               as cond holds, whatever command is executing
//     checkdef <name> <maxFails> <text>         (W5 CTL) declare a named check; variables $<name> (1 = passed), $<name>f (failures),
//                                               $open (checks neither passed nor failed maxFails times); "CHECK RESULT" lines at exit
//     expect <var> [up|same|down|changed]       (W5 CTL) remember var's value for the next check
//     check <name> <frames>                     (W5 CTL) up/down/changed: PASS as soon as var moved so, FAIL after <frames>; same: FAIL
//                                               as soon as it moves, PASS after <frames> (neutral pad meanwhile)
//   Variables for wait: f scene depth ovl ovldepth task mode ingame gs clock score0 score1 sim
//   simstarted paused nbuttons onctx onoff lock, the LAN session counters smode sstate peers machines
//   found launch joinst hoststate (np_lan.cpp NpLanStatus; the recompiled harness had them as MSC_WATCH
//   expressions), ballx bally, the W5 ITEM ones (np/itemtest.h: item0 item0b itemn0 item1 item1b itemn1 pups chomp fx0 fx1
//   hits hitk own side ctl dopp downer dgoal dkeep dball), and any MSC_WATCH extra variable.
//   GameCube pad bits: LEFT 1, RIGHT 2, DOWN 4, UP 8, Z 0x10, R 0x20, L 0x40, A 0x100, B 0x200,
//   X 0x400, Y 0x800, START 0x1000. Sequential commands override absolute lines while they
//   press something. L/R also report a full analog trigger pull.
//
// Not the same as in the recompiled build, on purpose: no guest RAM exists, so MSC_STATE_HASH's
// "ram=" covers only the regions the SDK layer registered (NpHarnessRegisterRegion) and the new
// "sim=" digest is the portable check; `onoff` is the byte offset of the hovered button inside
// the top scene handler as the NATIVE class layout has it (the recompiled build reports the Wii's
// layout: if a script waits on an onoff value that never comes, the layouts differ and the script
// needs the native value: run with MSC_WATCH_BUTTONS=1 and read the "scene+0x..." column).
#include "np_harness.h"
#include "np_frame_timing.h"
#include "np_harness_state.h"
#include "np_time.h"

#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#if defined(__linux__)
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#endif

extern "C" void NpJobsLogStats(const char* when) __attribute__((weak));   // platform/vita/src/np_jobs.cpp
extern "C" void NpLanStatus(int out[8]) __attribute__((weak));               // platform/sdk/net/np_lan.cpp (LAN session counters)
extern "C" int NpLanNetBrief(char* out, int size) __attribute__((weak));     // platform/sdk/net/np_lan.cpp (LAN match: delay, starved, rtt)
extern "C" void NpFsJoin(void) __attribute__((weak));                          // platform/vita/src/np_framesend.cpp
extern "C" void NpFsLogStats(const char* when) __attribute__((weak));
extern "C" void NpLatTakeWindow(uint32_t* n, uint32_t* p50Us, uint32_t* p95Us) __attribute__((weak));   // platform/compat/np_latency.cpp (W5 RT)
extern "C" void NpThpLogStats(void) __attribute__((weak));                   // platform/sdk/src/thp_native.cpp
// W5 ITEM (src/Game/NpItemTest.cpp, np/itemtest.h): the test-only item hook
extern "C" int NpItemTestQueue(int side, int type, int count, int mode) __attribute__((weak));
extern "C" void NpItemTestFrame(unsigned long long frame) __attribute__((weak));
extern "C" int NpItemTestAim(int target, int* sx, int* sy) __attribute__((weak));
extern "C" int NpItemTestVar(const char* name, double* out) __attribute__((weak));
extern "C" void __gcov_dump(void) __attribute__((weak));                     // only in a -fprofile-generate build (PGO recipe: np/docs/vita.md)

namespace NpHarness {
extern "C" long long NpFpsModelCharge(unsigned taskState, int sceneId, unsigned steps) __attribute__((weak));   // platform/compat/np_fps.cpp (np/fps.h)
namespace {

State g_state;

// Leaves the process at once (no static destructors, as on the desktop).
[[noreturn]] void HarnessExit(int code) {
    std::fflush(nullptr);
#if defined(__vita__)
    sceKernelExitProcess(code);
    for (;;) sceKernelDelayThread(1000000);
#else
    std::_Exit(code);
#endif
}

// ---------------------------------------------------------------------------------------------
// MSC_WATCH extra variables: name:type:expr
// ---------------------------------------------------------------------------------------------
struct Extra { std::string name, type, expr; };
std::vector<Extra> g_extras;

bool IsHexToken(const std::string& t) {
    size_t i = (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) ? 2 : 0;
    if (i == t.size()) return false;
    for (; i < t.size(); ++i) if (!std::isxdigit(static_cast<unsigned char>(t[i]))) return false;
    return true;
}
bool EvalExpr(const char*& p, uintptr_t& out) {
    uintptr_t acc = 0;
    for (bool first = true;; first = false) {
        while (*p == ' ') ++p;
        uintptr_t term = 0;
        if (*p == '[') {
            ++p;
            if (!EvalExpr(p, term) || *p != ']') return false;
            ++p;
            uint32_t word = 0;
            if (!SafeRead(term, &word, 4)) return false;
            term = word;
        } else {
            const char* q = p;
            while (std::isalnum(static_cast<unsigned char>(*q)) || *q == '_' || *q == ':') ++q;
            if (q == p) return false;
            const std::string tok(p, q);
            if (IsHexToken(tok)) term = static_cast<uintptr_t>(std::strtoul(tok.c_str(), nullptr, 16));
            else if (!ResolveSymbol(tok.c_str(), &term)) return false;
            p = q;
        }
        acc = first ? term : acc + term;
        while (*p == ' ') ++p;
        if (*p != '+') break;
        ++p;
    }
    out = acc;
    return true;
}
bool EvalExtra(const Extra& e, double& value) {
    const char* p = e.expr.c_str();
    uintptr_t addr = 0;
    if (!EvalExpr(p, addr)) return false;
    uint32_t w = 0;
    if (e.type == "u8") { uint8_t v; if (!SafeRead(addr, &v, 1)) return false; value = v; }
    else if (e.type == "u16") { uint16_t v; if (!SafeRead(addr, &v, 2)) return false; value = v; }
    else if (e.type == "s32") { if (!SafeRead(addr, &w, 4)) return false; value = static_cast<int32_t>(w); }
    else if (e.type == "f32") { float f; if (!SafeRead(addr, &f, 4)) return false; value = f; }
    else { if (!SafeRead(addr, &w, 4)) return false; value = w; }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Input scripts
// ---------------------------------------------------------------------------------------------
std::atomic<uint64_t> g_frames{0};
struct Step { uint64_t first, last; uint32_t buttons; int sx, sy; };
std::vector<Step> g_script;
struct Term { std::string var, op; double value = 0; bool orWithPrev = false; };
struct Cmd {
    enum Kind { Wait, Hold, Mash, If, EndIf, Log, ProfBegin, ProfEnd, Exit, StopWhen, Launch, Scene, PopScene, SetF, Item, Aim, Tap, Repeat, EndRepeat,
                Expect, Check } kind;
    uint64_t period = 0;
    std::vector<Term> cond;
    std::string text;
    uint64_t frames = 0;
    uint32_t buttons = 0;
    int sx = 0, sy = 0, cx = 0, cy = 0;
    int line = 0;
    LaunchSpec launch;      // Launch
    int sceneId = 0; bool scenePop = false;   // Scene
    std::string gname; float gvalue = 0;      // SetF
    int itemSide = -1, itemType = 0, itemCount = 1, itemMode = 1;   // Item
    int aimTarget = 0;                        // Aim (with buttons, cond, frames)
    // W5 CTL: Vita-button and touch commands (vhold vpress vmash touch swipe rtouch rswipe rmash swmash): `buttons` are SCE_CTRL
    // bits, the fingers move from (tx0, ty0) to (tx1, ty1) over the first `touchFrames` frames of each press
    bool vita = false;
    int touchKind = 0;                        // 0 none, 1 front panel, 2 rear panel
    float tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
    uint64_t touchFrames = 0;
    bool soft = false;                        // "try": a timeout goes on with the next command
    int chase = 0;                            // vchase: the left stick runs toward 1 the ball, 2 the opponent's net (game variables cbd* / cgd*)
};
std::vector<Cmd> g_prog;
std::vector<Term> g_stopWhen;
// W5 CTL: named checks (checkdef / expect / check): "did this input make the engine do that". A check passes once and stays passed; it
// fails at most `maxFails` times (then it is decided: FAIL); $open counts the undecided ones (the run can stop when it is 0).
struct NamedCheck { std::string name, desc; int maxFails = 4; int pass = 0, fails = 0, tries = 0; };
std::vector<NamedCheck> g_checks;
struct ExpectState { std::string var; int op = 0; double base = 0; bool set = false; };   // op 0 up, 1 same, 2 down
ExpectState g_expect;
NamedCheck* FindCheck(const std::string& name) {
    for (NamedCheck& c : g_checks) if (c.name == name) return &c;
    return nullptr;
}
void CheckSummary() {
    int p = 0, f = 0, n = 0;
    for (const NamedCheck& c : g_checks) {
        const char* r = c.pass ? "PASS" : c.fails >= c.maxFails ? "FAIL" : c.tries ? "FAIL (undecided)" : "NOT RUN";
        if (c.pass) ++p; else if (c.tries) ++f; else ++n;
        std::fprintf(stderr, "[script] CHECK RESULT %s %s tries=%d fails=%d : %s\n", c.name.c_str(), r, c.tries, c.fails, c.desc.c_str());
    }
    std::fprintf(stderr, "[script] checks: %d passed, %d failed, %d not run (of %d)\n", p, f, n, static_cast<int>(g_checks.size()));
}
size_t g_pc = 0;
std::map<size_t, uint64_t> g_loopLeft;   // W5 CTL: repeat counters by the repeat command's index
uint64_t g_cmdFrames = 0;       // frames spent in the current command
struct PadOut { bool active = false; uint32_t buttons = 0; int sx = 0, sy = 0, cx = 0, cy = 0; };
PadOut g_padOut;
// W5 CTL: the Vita-terms pad of the current command (NpHarnessFillRaw)
struct RawOut { bool active = false; uint32_t buttons = 0; int sx = 0, sy = 0, cx = 0, cy = 0; int touchKind = 0; float tx = 0, ty = 0; int touchId = 0; };
RawOut g_rawOut;
int g_touchId = 100;        // every new finger gets a new id

// "cross+circle", "l+r", "none" or a hex value (SCE_CTRL bits); false if malformed
bool ParseVitaButtons(const char* word, uint32_t& out) {
    static const struct { const char* name; uint32_t bit; } kNames[] = {
        {"select", 0x0001}, {"start", 0x0008}, {"up", 0x0010}, {"right", 0x0020}, {"down", 0x0040}, {"left", 0x0080}, {"l", 0x0100},
        {"r", 0x0200}, {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000}, {"none", 0}};
    out = 0;
    if (IsHexToken(word)) { out = static_cast<uint32_t>(std::strtoul(word, nullptr, 16)); return true; }
    std::string w(word);
    size_t at = 0;
    while (at <= w.size()) {
        size_t end = w.find('+', at);
        if (end == std::string::npos) end = w.size();
        const std::string tok = w.substr(at, end - at);
        bool found = false;
        for (const auto& n : kNames) if (tok == n.name) { out |= n.bit; found = true; }
        if (!found) return false;
        at = end + 1;
    }
    return true;
}
bool g_initDone = false;
bool g_watch = false, g_watchButtons = false, g_frameTrace = false;
uint64_t g_watchEvery = 60, g_hashEvery = 0;
int64_t g_frameCostNs = 0, g_stepCostNs = 0;
bool g_fpsModel = false;   // MSC_FPS_MODEL (np/fps.h): a per-scene cost model charged to the virtual clock instead of MSC_VIRTUAL_FRAME_US / _STEP_US

// "<var> <op> <value> [&&|| <var> <op> <value>]... [timeout]"; false if malformed.
bool ParseCond(const char* text, std::vector<Term>& cond, uint64_t& timeout) {
    std::vector<std::string> tok;
    for (const char* p = text; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        const char* q = p;
        while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r') ++q;
        if (q > p) tok.emplace_back(p, q);
        p = q;
    }
    size_t i = 0;
    bool orNext = false;
    while (i + 3 <= tok.size()) {
        Term t;
        t.var = tok[i]; t.op = tok[i + 1];
        const std::string& v = tok[i + 2];
        t.value = (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) ? static_cast<double>(std::strtoul(v.c_str(), nullptr, 16))
                                                                                : std::strtod(v.c_str(), nullptr);
        t.orWithPrev = orNext;
        static const char* ops[] = {"==", "!=", "<", "<=", ">", ">="};
        bool okOp = false;
        for (const char* o : ops) okOp = okOp || t.op == o;
        if (!okOp) return false;
        cond.push_back(t);
        i += 3;
        if (i < tok.size() && (tok[i] == "||" || tok[i] == "&&")) { orNext = tok[i] == "||"; ++i; continue; }
        break;
    }
    if (cond.empty()) return false;
    if (i < tok.size()) { timeout = std::strtoull(tok[i].c_str(), nullptr, 10); ++i; }
    return i == tok.size();
}

// "launch key=value ...": mode (friendly|demo|cup|challenge|0..4), stadium, home, away (eStadiumID / eTeamID numbers), hsk, ask ("a,b,c" sidekick ids),
// powerup, env, player (cheats), human (side of pad 0), skill, time (match seconds). Unknown keys are an error.
bool ParseLaunch(const char* text, LaunchSpec& l) {
    std::string t(text);
    size_t i = 0;
    auto three = [](const std::string& v, int out[3]) {
        int a[3] = {-1, -1, -1};
        const int n = std::sscanf(v.c_str(), "%d,%d,%d", &a[0], &a[1], &a[2]);
        if (n < 1) return false;
        for (int k = 0; k < 3; ++k) out[k] = a[k];
        return true;
    };
    while (i < t.size()) {
        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
        if (i >= t.size()) break;
        size_t j = i;
        while (j < t.size() && !std::isspace(static_cast<unsigned char>(t[j]))) ++j;
        const std::string kv = t.substr(i, j - i);
        i = j;
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) return false;
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        const int n = static_cast<int>(std::strtol(v.c_str(), nullptr, 0));
        if (k == "mode") l.mode = v == "friendly" || v == "domination" ? 0 : v == "demo" ? 2 : v == "cup" ? 3 : v == "challenge" ? 4 : n;
        else if (k == "stadium") l.stadium = n;
        else if (k == "home") l.home = n;
        else if (k == "away") l.away = n;
        else if (k == "hsk") { if (!three(v, l.hsk)) return false; }
        else if (k == "ask") { if (!three(v, l.ask)) return false; }
        else if (k == "powerup") l.powerup = n;
        else if (k == "env") l.env = n;
        else if (k == "player") l.player = n;
        else if (k == "human") l.human = n;
        else if (k == "skill") l.skill = n;
        else if (k == "time") l.time = n;
        else if (k == "push") l.push = n;
        else return false;
    }
    return true;
}

void LoadScript() {
    const char* path = std::getenv("MSC_INPUT_SCRIPT");
    if (!path) return;
    FILE* f = std::fopen(path, "r");
    if (!f) { std::fprintf(stderr, "[script] cannot open %s\n", path); return; }
    char line[512];
    int lineNo = 0;
    while (std::fgets(line, sizeof line, f)) {
        ++lineNo;
        if (char* hash = std::strchr(line, '#')) *hash = 0;
        char word[32] = {0};
        int used = 0;
        if (std::sscanf(line, " %31s%n", word, &used) != 1) continue;
        const char* rest = line + used;
        Cmd c{}; c.line = lineNo;
        // W5 CTL: "try <wait|mash|vmash|rmash|swmash> ...": a timeout is not a failure, the script goes on
        if (!std::strcmp(word, "try")) {
            int used2 = 0;
            if (std::sscanf(rest, " %31s%n", word, &used2) != 1) goto bad;
            rest += used2;
            c.soft = true;
        }
        if (word[0] >= '0' && word[0] <= '9') {
            Step s{}; unsigned long long a, b; unsigned btn; int sx = 0, sy = 0;
            if (std::sscanf(line, "%llu %llu %x %d %d", &a, &b, &btn, &sx, &sy) >= 3) {
                s.first = a; s.last = b; s.buttons = btn; s.sx = sx; s.sy = sy;
                g_script.push_back(s);
            }
            continue;
        }
        if (!std::strcmp(word, "wait") || !std::strcmp(word, "if")) {
            c.kind = word[0] == 'w' ? Cmd::Wait : Cmd::If;
            c.frames = 36000;
            if (!ParseCond(rest, c.cond, c.frames)) goto bad;
        } else if (!std::strcmp(word, "stopwhen")) {
            c.kind = Cmd::StopWhen;
            if (!ParseCond(rest, c.cond, c.frames)) goto bad;
        } else if (!std::strcmp(word, "endif")) {
            c.kind = Cmd::EndIf;
        } else if (!std::strcmp(word, "mash") || !std::strcmp(word, "tap")) {
            unsigned btn; unsigned long long period; int used2 = 0;
            if (std::sscanf(rest, " %x %llu%n", &btn, &period, &used2) < 2 || !period) goto bad;
            c.kind = word[0] == 't' ? Cmd::Tap : Cmd::Mash; c.buttons = btn; c.period = period; c.frames = 36000;
            if (!ParseCond(rest + used2, c.cond, c.frames)) goto bad;
        } else if (!std::strcmp(word, "idle")) {
            unsigned long long n;
            if (std::sscanf(rest, " %llu", &n) != 1) goto bad;
            c.kind = Cmd::Hold; c.frames = n;
        } else if (!std::strcmp(word, "hold")) {
            unsigned long long n; unsigned btn;
            if (std::sscanf(rest, " %llu %x %d %d %d %d", &n, &btn, &c.sx, &c.sy, &c.cx, &c.cy) < 2) goto bad;
            c.kind = Cmd::Hold; c.frames = n; c.buttons = btn;
        } else if (!std::strcmp(word, "press")) {
            unsigned btn; unsigned long long n = 3, gap = 6;
            if (std::sscanf(rest, " %x %llu %llu", &btn, &n, &gap) < 1) goto bad;
            c.kind = Cmd::Hold; c.frames = n; c.buttons = btn;
            g_prog.push_back(c);
            c.buttons = 0; c.frames = gap;
            if (!gap) continue;
        } else if (!std::strcmp(word, "vhold")) {
            unsigned long long n; char b[64] = {0};
            if (std::sscanf(rest, " %llu %63s %d %d %d %d", &n, b, &c.sx, &c.sy, &c.cx, &c.cy) < 2 || !ParseVitaButtons(b, c.buttons)) goto bad;
            c.kind = Cmd::Hold; c.frames = n; c.vita = true;
        } else if (!std::strcmp(word, "vchase")) {
            // vchase <frames> <buttons> ball|goal [cx cy]: hold buttons while the left stick runs the controlled player toward the target
            unsigned long long n; char b[64] = {0}, target[16] = {0};
            if (std::sscanf(rest, " %llu %63s %15s %d %d", &n, b, target, &c.cx, &c.cy) < 3 || !ParseVitaButtons(b, c.buttons)) goto bad;
            if (!std::strcmp(target, "ball")) c.chase = 1;
            else if (!std::strcmp(target, "goal")) c.chase = 2;
            else goto bad;
            c.kind = Cmd::Hold; c.frames = n; c.vita = true;
        } else if (!std::strcmp(word, "vpress")) {
            char b[64] = {0}; unsigned long long n = 3, gap = 6;
            if (std::sscanf(rest, " %63s %llu %llu", b, &n, &gap) < 1 || !ParseVitaButtons(b, c.buttons)) goto bad;
            c.kind = Cmd::Hold; c.frames = n; c.vita = true;
            g_prog.push_back(c);
            c.buttons = 0; c.frames = gap; c.vita = false;
            if (!gap) continue;
        } else if (!std::strcmp(word, "vmash")) {
            char b[64] = {0}; unsigned long long period; int used2 = 0;
            if (std::sscanf(rest, " %63s %llu%n", b, &period, &used2) < 2 || !period || !ParseVitaButtons(b, c.buttons)) goto bad;
            c.kind = Cmd::Mash; c.period = period; c.frames = 36000; c.vita = true;
            if (!ParseCond(rest + used2, c.cond, c.frames)) goto bad;
        } else if (!std::strcmp(word, "touch") || !std::strcmp(word, "rtouch") || !std::strcmp(word, "swipe") || !std::strcmp(word, "rswipe")) {
            const bool sw = word[0] == 's' || word[1] == 's';
            float x0, y0, x1, y1; unsigned long long n = sw ? 6 : 3, gap = 6;
            if (sw) { if (std::sscanf(rest, " %f %f %f %f %llu %llu", &x0, &y0, &x1, &y1, &n, &gap) < 4) goto bad; }
            else { if (std::sscanf(rest, " %f %f %llu %llu", &x0, &y0, &n, &gap) < 2) goto bad; x1 = x0; y1 = y0; }
            c.kind = Cmd::Hold; c.frames = n ? n : 1; c.vita = true; c.touchKind = word[0] == 'r' ? 2 : 1;
            c.tx0 = x0; c.ty0 = y0; c.tx1 = x1; c.ty1 = y1; c.touchFrames = c.frames;
            g_prog.push_back(c);
            c = Cmd{}; c.line = lineNo; c.kind = Cmd::Hold; c.frames = gap;
            if (!gap) continue;
        } else if (!std::strcmp(word, "rmash") || !std::strcmp(word, "swmash")) {
            // rmash <x> <y> <period> <cond>: rear taps (3 frames down) every period frames; swmash <x0> <y0> <x1> <y1> <period> <cond>: front swipes
            const bool sw = word[0] == 's';
            float x0, y0, x1 = 0, y1 = 0; unsigned long long period; int used2 = 0;
            if (sw) { if (std::sscanf(rest, " %f %f %f %f %llu%n", &x0, &y0, &x1, &y1, &period, &used2) < 5) goto bad; }
            else { if (std::sscanf(rest, " %f %f %llu%n", &x0, &y0, &period, &used2) < 3) goto bad; x1 = x0; y1 = y0; }
            if (period < 5) goto bad;
            c.kind = Cmd::Mash; c.period = period; c.frames = 36000; c.vita = true; c.touchKind = sw ? 1 : 2;
            c.tx0 = x0; c.ty0 = y0; c.tx1 = x1; c.ty1 = y1; c.touchFrames = 3;
            if (!ParseCond(rest + used2, c.cond, c.frames)) goto bad;
        } else if (!std::strcmp(word, "repeat")) {
            // W5 CTL: repeat <n> ... endrepeat (nestable): the body runs n times (a long script stays small)
            unsigned long long n;
            if (std::sscanf(rest, " %llu", &n) != 1 || !n) goto bad;
            c.kind = Cmd::Repeat; c.frames = n;
        } else if (!std::strcmp(word, "endrepeat")) {
            c.kind = Cmd::EndRepeat;
        } else if (!std::strcmp(word, "checkdef")) {
            // W5 CTL: checkdef <name> <maxFails> <description>: declares a named check (variables $<name> 1 = passed, $<name>f = failures,
            // $open = checks not decided yet); a summary of all of them is printed at exit
            char nm[64] = {0}; int maxFails = 0, used2 = 0;
            if (std::sscanf(rest, " %63s %d%n", nm, &maxFails, &used2) < 2 || maxFails < 1 || FindCheck(nm)) goto bad;
            NamedCheck k; k.name = nm; k.maxFails = maxFails; k.desc = rest + used2;
            while (!k.desc.empty() && (k.desc.back() == '\n' || k.desc.back() == '\r' || k.desc.back() == ' ')) k.desc.pop_back();
            while (!k.desc.empty() && k.desc.front() == ' ') k.desc.erase(k.desc.begin());
            if (g_checks.empty()) NpHarnessAtExit(CheckSummary);
            g_checks.push_back(k);
            continue;
        } else if (!std::strcmp(word, "expect")) {
            // expect <var> [up|same|down|changed]: the value now is the base the next "check" compares with
            char v[64] = {0}, op[16] = "up";
            if (std::sscanf(rest, " %63s %15s", v, op) < 1) goto bad;
            c.kind = Cmd::Expect; c.text = v;
            c.period = !std::strcmp(op, "up") ? 0 : !std::strcmp(op, "same") ? 1 : !std::strcmp(op, "down") ? 2 : !std::strcmp(op, "changed") ? 3 : 9;
            if (c.period == 9) goto bad;
        } else if (!std::strcmp(word, "check")) {
            // check <name> <frames>: up / down / changed: passes as soon as the variable moved so, fails after <frames>; same: fails as soon as
            // it moves, passes after <frames>. Neutral pad while it waits.
            char nm[64] = {0}; unsigned long long n = 0;
            if (std::sscanf(rest, " %63s %llu", nm, &n) != 2 || !n || !FindCheck(nm)) goto bad;
            c.kind = Cmd::Check; c.text = nm; c.frames = n;
        } else if (!std::strcmp(word, "log")) {
            c.kind = Cmd::Log; c.text = rest;
            while (!c.text.empty() && (c.text.back() == '\n' || c.text.back() == '\r' || c.text.back() == ' ')) c.text.pop_back();
            while (!c.text.empty() && c.text.front() == ' ') c.text.erase(c.text.begin());
        } else if (!std::strcmp(word, "prof")) {
            char w[16] = {0};
            std::sscanf(rest, " %15s", w);
            c.kind = !std::strcmp(w, "end") ? Cmd::ProfEnd : Cmd::ProfBegin;
        } else if (!std::strcmp(word, "exit")) {
            c.kind = Cmd::Exit;
        } else if (!std::strcmp(word, "scene")) {
            int id = -1, pop = 0;
            if (std::sscanf(rest, " %d %d", &id, &pop) < 1 || id < 0) goto bad;
            c.kind = Cmd::Scene; c.sceneId = id; c.scenePop = pop != 0;
        } else if (!std::strcmp(word, "setf")) {
            char nm[64] = {0};
            float v = 0;
            if (std::sscanf(rest, " %63s %f", nm, &v) != 2) goto bad;
            c.kind = Cmd::SetF; c.gname = nm; c.gvalue = v;
        } else if (!std::strcmp(word, "popscene")) {
            c.kind = Cmd::PopScene;
        } else if (!std::strcmp(word, "launch")) {
            if (!ParseLaunch(rest, c.launch)) goto bad;
            c.kind = Cmd::Launch;
        } else if (!std::strcmp(word, "item")) {
            // item <side> <type> [count] [set|add]
            static const char* const names[21] = {"green", "red", "spiny", "freeze", "banana", "bobomb", "chomp", "mushroom", "star", "mario", "peach",
                                                   "dk", "bowser", "luigi", "daisy", "wario", "waluigi", "bowserjr", "diddy", "yoshi", "petey"};
            char sd[16] = {0}, ty[16] = {0}, md[8] = {0};
            int cnt = 1;
            const int n = std::sscanf(rest, " %15s %15s %d %7s", sd, ty, &cnt, md);
            if (n < 2) goto bad;
            c.kind = Cmd::Item;
            c.itemSide = !std::strcmp(sd, "me") ? -1 : std::atoi(sd);
            c.itemType = -100;
            if (!std::strcmp(ty, "captain")) c.itemType = -2;
            for (int k = 0; k < 21; ++k) if (!std::strcmp(ty, names[k])) c.itemType = k;
            if (c.itemType == -100 && ty[0] >= '0' && ty[0] <= '9') c.itemType = std::atoi(ty);
            if (c.itemType == -100 || c.itemType > 20) goto bad;
            c.itemCount = n >= 3 ? cnt : 1;
            c.itemMode = (n >= 4 && !std::strcmp(md, "add")) ? 0 : 1;
        } else if (!std::strcmp(word, "aim")) {
            // aim <target> <buttonsHex> <cond> [timeout]
            char tg[16] = {0};
            unsigned btn = 0;
            int used2 = 0;
            if (std::sscanf(rest, " %15s %x%n", tg, &btn, &used2) < 2) goto bad;
            c.kind = Cmd::Aim; c.buttons = btn; c.frames = 36000;
            c.aimTarget = !std::strcmp(tg, "opp") ? 0 : !std::strcmp(tg, "owner") ? 1 : !std::strcmp(tg, "goal") ? 2 : !std::strcmp(tg, "keeper") ? 3
                        : !std::strcmp(tg, "ball") ? 4 : -1;
            if (c.aimTarget < 0) goto bad;
            if (!ParseCond(rest + used2, c.cond, c.frames)) goto bad;
        } else {
        bad:
            std::fprintf(stderr, "[script] %s:%d: cannot parse: %s", path, lineNo, line);
            HarnessExit(2);
        }
        g_prog.push_back(c);
    }
    std::fclose(f);
    // W5 CTL: pair repeat / endrepeat (period = the partner's index)
    std::vector<size_t> open;
    for (size_t i = 0; i < g_prog.size(); ++i) {
        if (g_prog[i].kind == Cmd::Repeat) open.push_back(i);
        else if (g_prog[i].kind == Cmd::EndRepeat) {
            if (open.empty()) { std::fprintf(stderr, "[script] %s:%d: endrepeat without repeat\n", path, g_prog[i].line); HarnessExit(2); }
            g_prog[i].period = open.back();
            g_prog[open.back()].period = i;
            open.pop_back();
        }
    }
    if (!open.empty()) { std::fprintf(stderr, "[script] %s:%d: repeat without endrepeat\n", path, g_prog[open.back()].line); HarnessExit(2); }
}

bool VarValue(const std::string& v, double& out) {
    const State& s = g_state;
    if (!v.empty() && v[0] == '$') {
        // W5 CTL: the named checks
        if (v == "$open") {
            int n = 0;
            for (const NamedCheck& c : g_checks) if (!c.pass && c.fails < c.maxFails) ++n;
            out = n;
            return true;
        }
        const bool fails = v.size() > 2 && v.back() == 'f' && !FindCheck(v.substr(1));
        const NamedCheck* c = FindCheck(fails ? v.substr(1, v.size() - 2) : v.substr(1));
        if (!c) return false;
        out = fails ? c->fails : (c->pass ? 1 : 0);
        return true;
    }
    if (v == "f") out = static_cast<double>(g_frames.load(std::memory_order_relaxed));
    else if (v == "scene") out = s.scene.depth ? s.scene.ids[s.scene.depth - 1] : -1;
    else if (v == "depth") out = s.scene.depth;
    else if (v == "ovl") out = s.overlay.depth ? s.overlay.ids[s.overlay.depth - 1] : -1;
    else if (v == "ovldepth") out = s.overlay.depth;
    else if (v == "task") out = s.taskState;
    else if (v == "mode") out = s.mode;
    else if (v == "ingame") out = s.game ? 1 : 0;
    else if (v == "gs") out = s.gameState;
    else if (v == "clock") out = s.clock;
    else if (v == "score0") out = s.score[0];
    else if (v == "score1") out = s.score[1];
    else if (v == "sim") out = s.simFrame;
    else if (v == "simstarted") out = s.simStarted ? 1 : 0;
    else if (v == "paused") out = s.paused ? 1 : 0;
    else if (v == "nbuttons") out = s.nButtons;
    else if (v == "onctx") out = s.onCtx;
    else if (v == "onoff") out = s.onOff;
    else if (v == "lock") out = s.inputLock;
    else if (v == "ballx") out = s.ball[0];
    else if (v == "bally") out = s.ball[1];
    else if (v == "smode" || v == "sstate" || v == "peers" || v == "machines" || v == "found" || v == "launch" || v == "joinst" || v == "hoststate") {
        // LAN session (multiplayer): NetworkSession mode / state, lobby peer count, machines, games found, launch / join / host state
        int n[8] = {0};
        if (NpLanStatus) NpLanStatus(n);
        out = v == "smode" ? n[0] : v == "sstate" ? n[1] : v == "peers" ? n[2] : v == "machines" ? n[3] : v == "found" ? n[4]
            : v == "launch" ? n[5] : v == "joinst" ? n[6] : n[7];
    }
    else if (NpItemTestVar && NpItemTestVar(v.c_str(), &out)) return true;   // W5 ITEM: item0 pups fx0 dopp dgoal ... (np/itemtest.h)
    else {
        for (const Extra& e : g_extras) if (e.name == v) return EvalExtra(e, out);
        if (NpHarnessGameVar && NpHarnessGameVar(v.c_str(), &out)) return true;   // W5 CTL: the controls' counters (np_game_input.cpp)
        return false;
    }
    return true;
}
bool Compare(double a, const std::string& op, double b) {
    if (op == "==") return a == b;
    if (op == "!=") return a != b;
    if (op == "<") return a < b;
    if (op == "<=") return a <= b;
    if (op == ">") return a > b;
    if (op == ">=") return a >= b;
    return false;
}

void Finish(int code, const char* why);
bool CondHolds(const std::vector<Term>& cond, std::string* text = nullptr) {
    bool result = false;
    for (size_t i = 0; i < cond.size(); ++i) {
        double v = 0;
        const bool ok = VarValue(cond[i].var, v) && Compare(v, cond[i].op, cond[i].value);
        if (text) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s%s %s %g (now %g)", i ? (cond[i].orWithPrev ? " || " : " && ") : "",
                          cond[i].var.c_str(), cond[i].op.c_str(), cond[i].value, v);
            *text += buf;
        }
        result = i == 0 ? ok : (cond[i].orWithPrev ? (result || ok) : (result && ok));
    }
    return result;
}

// Runs the sequential program for the frame about to be produced.
void ProgramTick() {
    g_padOut = {};
    g_rawOut = {};
    const unsigned long long frame = g_frames.load(std::memory_order_relaxed);
    if (!g_stopWhen.empty() && CondHolds(g_stopWhen)) {
        std::string text;
        CondHolds(g_stopWhen, &text);
        std::fprintf(stderr, "[script] f=%llu stopwhen: %s\n", frame, text.c_str());
        Finish(0, "script stop condition");
    }
    while (g_pc < g_prog.size()) {
        const Cmd& c = g_prog[g_pc];
        switch (c.kind) {
        case Cmd::Wait:
        case Cmd::Mash:
        case Cmd::Tap: {
            if (CondHolds(c.cond)) { ++g_pc; g_cmdFrames = 0; continue; }
            if (++g_cmdFrames > c.frames && c.soft) {
                std::fprintf(stderr, "[script] f=%llu try line %d: timed out, going on\n", frame, c.line);
                ++g_pc; g_cmdFrames = 0; continue;
            }
            if (g_cmdFrames > c.frames) {
                std::string text;
                CondHolds(c.cond, &text);
                if (c.kind == Cmd::Tap) {
                    std::fprintf(stderr, "[script] f=%llu tap timeout (line %d, going on): %s\n", frame, c.line, text.c_str());
                    ++g_pc; g_cmdFrames = 0; continue;
                }
                std::fprintf(stderr, "[script] TIMEOUT f=%llu line %d: %s\n", frame, c.line, text.c_str());
                Finish(3, "script timeout");
            }
            // D-pad presses last one frame: the port's menu navigation auto-repeats a held
            // direction after 350 ms of host time, which a slow host reaches within a few frames.
            if (c.kind == Cmd::Mash && c.vita) {
                const uint64_t phase = (g_cmdFrames - 1) % c.period;
                if (c.touchKind) {
                    if (phase == 0) ++g_touchId;
                    if (phase < c.touchFrames) {
                        const float k = c.touchFrames > 1 ? static_cast<float>(phase) / static_cast<float>(c.touchFrames - 1) : 0.0f;
                        g_rawOut.active = true; g_rawOut.touchKind = c.touchKind; g_rawOut.touchId = g_touchId;
                        g_rawOut.tx = c.tx0 + (c.tx1 - c.tx0) * k; g_rawOut.ty = c.ty0 + (c.ty1 - c.ty0) * k;
                    }
                } else if (phase < 3) {
                    g_rawOut.active = true;
                    g_rawOut.buttons = c.buttons;
                }
                return;
            }
            // (W5 ITEM: Mash, Tap and Aim press their buttons here; Wait presses nothing)
            if (c.kind != Cmd::Wait && (g_cmdFrames - 1) % c.period < ((c.buttons & ~0xFu) ? 3u : 1u)) {
                g_padOut.active = true;
                g_padOut.buttons = c.buttons;
            }
            return;
        }
        case Cmd::If:
            if (!CondHolds(c.cond)) {
                // Skip to the matching endif.
                int depth = 1;
                while (++g_pc < g_prog.size() && depth > 0) {
                    if (g_prog[g_pc].kind == Cmd::If) ++depth;
                    else if (g_prog[g_pc].kind == Cmd::EndIf) --depth;
                }
                continue;
            }
            ++g_pc; continue;
        case Cmd::EndIf:
            ++g_pc; continue;
        case Cmd::Repeat:
            g_loopLeft[g_pc] = c.frames;   // entered from above: a fresh count
            ++g_pc; continue;
        case Cmd::EndRepeat: {
            uint64_t& left = g_loopLeft[c.period];
            if (left > 1) { --left; g_pc = c.period + 1; }
            else ++g_pc;
            continue;
        }
        case Cmd::StopWhen:
            g_stopWhen = c.cond;
            ++g_pc; continue;
        case Cmd::Hold:
            if (g_cmdFrames >= c.frames) { ++g_pc; g_cmdFrames = 0; continue; }
            ++g_cmdFrames;
            if (c.vita) {
                // a Vita-terms command: buttons, sticks and one finger (front or rear)
                g_rawOut.active = true;
                g_rawOut.buttons = c.buttons; g_rawOut.sx = c.sx; g_rawOut.sy = c.sy; g_rawOut.cx = c.cx; g_rawOut.cy = c.cy;
                if (c.chase) {
                    double x = 0, y = 0;
                    const bool ball = c.chase == 1;
                    if (NpHarnessGameVar && NpHarnessGameVar(ball ? "cbdx" : "cgdx", &x) && NpHarnessGameVar(ball ? "cbdy" : "cgdy", &y)) {
                        g_rawOut.sx = static_cast<int>(std::lround(x * 72.0));
                        g_rawOut.sy = static_cast<int>(std::lround(y * 72.0));
                    }
                }
                if (c.touchKind) {
                    if (g_cmdFrames == 1) ++g_touchId;
                    const float k = c.touchFrames > 1 ? static_cast<float>(g_cmdFrames - 1) / static_cast<float>(c.touchFrames - 1) : 0.0f;
                    g_rawOut.touchKind = c.touchKind; g_rawOut.touchId = g_touchId;
                    g_rawOut.tx = c.tx0 + (c.tx1 - c.tx0) * (k > 1.0f ? 1.0f : k); g_rawOut.ty = c.ty0 + (c.ty1 - c.ty0) * (k > 1.0f ? 1.0f : k);
                }
                return;
            }
            // "idle" leaves the pad to the absolute lines, if the script has any.
            g_padOut.active = c.buttons || c.sx || c.sy || c.cx || c.cy;
            g_padOut.buttons = c.buttons; g_padOut.sx = c.sx; g_padOut.sy = c.sy; g_padOut.cx = c.cx; g_padOut.cy = c.cy;
            return;
        case Cmd::Log:
            std::fprintf(stderr, "[script] f=%llu %s\n", frame, c.text.c_str());
            ++g_pc; continue;
        case Cmd::ProfBegin:
        case Cmd::ProfEnd:
            std::fprintf(stderr, "[script] f=%llu profile %s\n", frame, c.kind == Cmd::ProfBegin ? "begin" : "end");
#if defined(__linux__)
            // The qemu plugin starts its window at the first mark >= from and ends it at the first
            // mark >= to; these two values are above any frame number.
            syscall(0x7f00, c.kind == Cmd::ProfBegin ? 1000000000L : 1000000001L);
#endif
            ++g_pc; continue;
        case Cmd::Scene: {
            std::string msg;
            const bool ok = PushScene(c.sceneId, c.scenePop, msg);
            std::fprintf(stderr, "[script] f=%llu scene %d%s: %s%s\n", frame, c.sceneId, c.scenePop ? " (pop first)" : "", ok ? "" : "FAILED: ", msg.c_str());
            if (!ok) Finish(4, "scene failed");
            ++g_pc; continue;
        }
        case Cmd::SetF: {
            std::string msg;
            const bool ok = SetGlobalFloat(c.gname.c_str(), c.gvalue, msg);
            std::fprintf(stderr, "[script] f=%llu setf %s %g: %s%s\n", frame, c.gname.c_str(), c.gvalue, ok ? "" : "FAILED: ", msg.c_str());
            if (!ok) Finish(4, "setf failed");
            ++g_pc; continue;
        }
        case Cmd::PopScene: {
            std::string msg;
            const bool ok = PopScene(msg);
            std::fprintf(stderr, "[script] f=%llu popscene: %s%s\n", frame, ok ? "" : "FAILED: ", msg.c_str());
            if (!ok) Finish(4, "popscene failed");
            ++g_pc; continue;
        }
        case Cmd::Launch: {
            std::string msg;
            const bool ok = LaunchMatch(c.launch, msg);
            std::fprintf(stderr, "[script] f=%llu launch mode=%d stadium=%d home=%d away=%d: %s%s\n", frame, c.launch.mode, c.launch.stadium, c.launch.home,
                         c.launch.away, ok ? "" : "FAILED: ", msg.c_str());
            if (!ok) Finish(4, "launch failed");
            ++g_pc; continue;
        }
        case Cmd::Item: {
            const bool ok = NpItemTestQueue && NpItemTestQueue(c.itemSide, c.itemType, c.itemCount, c.itemMode);
            std::fprintf(stderr, "[script] f=%llu item side=%d type=%d count=%d %s: %s\n", frame, c.itemSide, c.itemType, c.itemCount, c.itemMode ? "set" : "add",
                         ok ? "queued" : NpItemTestQueue ? "NOT queued (MSC_ITEM_TEST=1? a match running?)" : "NOT queued (no item hook in this build)");
            ++g_pc; continue;
        }
        case Cmd::Aim: {
            if (CondHolds(c.cond)) { ++g_pc; g_cmdFrames = 0; continue; }
            if (++g_cmdFrames > c.frames) {
                std::string text;
                CondHolds(c.cond, &text);
                std::fprintf(stderr, "[script] f=%llu aim timeout (line %d, going on): %s\n", frame, c.line, text.c_str());
                ++g_pc; g_cmdFrames = 0; continue;
            }
            int sx = 0, sy = 0;
            if (NpItemTestAim) NpItemTestAim(c.aimTarget, &sx, &sy);
            g_padOut.active = true;
            g_padOut.buttons = c.buttons; g_padOut.sx = sx; g_padOut.sy = sy;
            return;
        }
        case Cmd::Expect: {
            double v = 0;
            g_expect.set = VarValue(c.text, v);
            g_expect.var = c.text; g_expect.op = static_cast<int>(c.period); g_expect.base = v;
            if (!g_expect.set) std::fprintf(stderr, "[script] f=%llu expect: unknown variable %s\n", frame, c.text.c_str());
            ++g_pc; continue;
        }
        case Cmd::Check: {
            NamedCheck* k = FindCheck(c.text);
            double v = 0;
            const bool known = g_expect.set && VarValue(g_expect.var, v);
            int verdict = 0;   // 1 pass, -1 fail, 0 keep waiting
            if (!known) verdict = -1;
            else if (g_expect.op == 0) verdict = v > g_expect.base ? 1 : 0;
            else if (g_expect.op == 2) verdict = v < g_expect.base ? 1 : 0;
            else if (g_expect.op == 3) verdict = v != g_expect.base ? 1 : 0;
            else verdict = v != g_expect.base ? -1 : 0;
            ++g_cmdFrames;
            if (verdict == 0 && g_cmdFrames > c.frames) verdict = g_expect.op == 1 ? 1 : -1;
            if (verdict == 0) return;   // neutral pad
            ++k->tries;
            if (verdict > 0) ++k->pass; else ++k->fails;
            std::fprintf(stderr, "[script] f=%llu CHECK %s %s (%s %s: %g -> %g after %llu frames)\n", frame, k->name.c_str(), verdict > 0 ? "PASS" : "FAIL",
                         g_expect.var.c_str(), g_expect.op == 0 ? "up" : g_expect.op == 1 ? "same" : g_expect.op == 2 ? "down" : "changed", g_expect.base, v,
                         static_cast<unsigned long long>(g_cmdFrames));
            ++g_pc; g_cmdFrames = 0; continue;
        }
        case Cmd::Exit:
            std::fprintf(stderr, "[script] f=%llu exit\n", frame);
            Finish(0, "script exit");
            return;
        }
    }
}


std::string StackText(const SceneStack& s) {
    if (!s.depth) return "-";
    std::string out;
    for (uint32_t i = 0; i < s.depth; ++i) {
        char buf[16]; std::snprintf(buf, sizeof buf, "%s%d", i ? " " : "", s.ids[i]);
        out += buf;
        const std::string n = SceneName(s.ids[i]);
        if (!n.empty()) out += ":" + n;
    }
    return out;
}

double g_realStart = 0;     // real monotonic seconds at harness start
uint32_t g_stepHist[6]{};   // presented frames with 0,1,2,3,4,5+ simulation steps since the last [watch] line
uint32_t g_lastPadButtons = 0;
int g_lastPadSx = 0, g_lastPadSy = 0;

void WatchLine(const State& s, unsigned long long n) {
    char sc[64] = "-", ov[64] = "-";
    if (s.scene.depth) std::snprintf(sc, sizeof sc, "%d:%s", s.scene.ids[s.scene.depth - 1], SceneName(s.scene.ids[s.scene.depth - 1]).c_str());
    if (s.overlay.depth) std::snprintf(ov, sizeof ov, "%d:%s", s.overlay.ids[s.overlay.depth - 1], SceneName(s.overlay.ids[s.overlay.depth - 1]).c_str());
    std::fprintf(stderr,
                 "[watch] f=%llu vt=%.3f rt=%.3f ret=%u scene=%s d=%u ovl=%s task=%u dt=%.2f mode=%d teams=%dv%d stad=%d game=%d gs=%d clk=%.2f/%.0f "
                 "score=%d-%d sim=%u simt=%.2f sph=%u/%u/%u/%u/%u/%u ball=%.2f,%.2f,%.2f own=%d btn=%d on=%d pad=%04x,%d,%d",
                 n, NpVirtualSeconds(), NpRealSeconds() - g_realStart, s.retrace, sc, s.scene.depth, ov, s.taskState, s.dtMs, s.mode, s.team[0], s.team[1], s.stadium, s.game ? 1 : 0,
                 s.gameState, s.clock, s.duration, s.score[0], s.score[1], s.simFrame, s.simTime, g_stepHist[0],
                 g_stepHist[1], g_stepHist[2], g_stepHist[3], g_stepHist[4], g_stepHist[5], s.ball[0], s.ball[1], s.ball[2],
                 s.ballOwner ? 1 : 0, s.nButtons, s.onCtx, g_lastPadButtons, g_lastPadSx, g_lastPadSy);
    for (const Extra& e : g_extras) {
        double v = 0;
        if (!EvalExtra(e, v)) std::fprintf(stderr, " %s=?", e.name.c_str());
        else if (e.type == "f32") std::fprintf(stderr, " %s=%.4f", e.name.c_str(), v);
        else if (e.type == "u32") std::fprintf(stderr, " %s=0x%X", e.name.c_str(), static_cast<uint32_t>(v));
        else std::fprintf(stderr, " %s=%.0f", e.name.c_str(), v);
    }
    if (NpLanNetBrief) {  // a LAN match: the lockstep's input delay, starved steps, steps per second and round trip (wave 5 NET)
        char lan[160];
        if (NpLanNetBrief(lan, sizeof lan) > 0) std::fputs(lan, stderr);
    }
    std::fputc('\n', stderr);
    std::memset(g_stepHist, 0, sizeof g_stepHist);
}

void ReportChanges(const State& prev, const State& s, unsigned long long n, const std::vector<Button>& buttons,
                   const float ptr[2]) {
    if (!(prev.scene == s.scene))
        std::fprintf(stderr, "[nav] f=%llu scenes: [%s] top=%s\n", n, StackText(s.scene).c_str(), ClassName(s.scene.topVtable).c_str());
    if (!(prev.overlay == s.overlay))
        std::fprintf(stderr, "[nav] f=%llu overlays: [%s] top=%s\n", n, StackText(s.overlay).c_str(), ClassName(s.overlay.topVtable).c_str());
    if (prev.taskState != s.taskState) std::fprintf(stderr, "[nav] f=%llu task state %u -> %u\n", n, prev.taskState, s.taskState);
    if (prev.mode != s.mode) std::fprintf(stderr, "[nav] f=%llu game mode %d -> %d\n", n, prev.mode, s.mode);
    if ((prev.game != 0) != (s.game != 0)) std::fprintf(stderr, "[nav] f=%llu match object %s\n", n, s.game ? "created" : "destroyed");
    if (prev.gameState != s.gameState)
        std::fprintf(stderr, "[nav] f=%llu match state %d -> %d (clock %.2f sim %u)\n", n, prev.gameState, s.gameState, s.clock, s.simFrame);
    if (prev.simStarted != s.simStarted) std::fprintf(stderr, "[nav] f=%llu simulation %s\n", n, s.simStarted ? "started" : "stopped");
    if (prev.score[0] != s.score[0] || prev.score[1] != s.score[1])
        std::fprintf(stderr, "[nav] f=%llu score %d-%d -> %d-%d (clock %.2f)\n", n, prev.score[0], prev.score[1], s.score[0], s.score[1], s.clock);
    if (prev.paused != s.paused) std::fprintf(stderr, "[nav] f=%llu pause menu %s\n", n, s.paused ? "on" : "off");
    if (prev.swapMode != s.swapMode) std::fprintf(stderr, "[nav] f=%llu swap mode %u -> %u\n", n, prev.swapMode, s.swapMode);
    // Extra variables of integer type are reported when they change (floats only in [watch] lines).
    static std::vector<double> lastExtra;
    lastExtra.resize(g_extras.size(), -1e300);
    for (size_t i = 0; i < g_extras.size(); ++i) {
        if (g_extras[i].type == "f32") continue;
        double v = 0;
        if (!EvalExtra(g_extras[i], v)) v = -1e300;
        if (v != lastExtra[i]) {
            if (v == -1e300) std::fprintf(stderr, "[nav] f=%llu %s: unreadable\n", n, g_extras[i].name.c_str());
            else if (g_extras[i].type == "u32") std::fprintf(stderr, "[nav] f=%llu %s: 0x%X\n", n, g_extras[i].name.c_str(), static_cast<uint32_t>(v));
            else std::fprintf(stderr, "[nav] f=%llu %s: %.0f\n", n, g_extras[i].name.c_str(), v);
            lastExtra[i] = v;
        }
    }
    if (g_watchButtons) {
        // The whole list when the set of buttons changes, one line when only the hovered one does.
        static uint64_t lastSet = 0, lastHover = 0;
        uint64_t set = buttons.size(), hover = 0;
        for (const Button& b : buttons) {
            uint32_t geom[2];
            std::memcpy(&geom[0], &b.cx, 4); std::memcpy(&geom[1], &b.cy, 4);
            set = (set ^ b.listener ^ (static_cast<uint64_t>(geom[0] ^ (geom[1] << 1)) << 32)) * 1099511628211ull;
            if (b.hover) hover = (hover ^ b.listener) * 1099511628211ull;
        }
        const auto print = [&](const Button& b) {
            const uint32_t off = b.listener - s.scene.top;
            char where[24];
            if (s.scene.top && off < 0x10000) std::snprintf(where, sizeof where, "scene+0x%X", off);
            else std::snprintf(where, sizeof where, "%08lX", static_cast<unsigned long>(b.listener));
            std::fprintf(stderr, "[nav]    %s%-14s %s ctx=%u centre=(%.0f,%.0f) size=%.0fx%.0f\n", b.hover ? "*" : " ", where,
                         ClassName(b.vtable).c_str(), b.ctx, b.cx, b.cy, b.maxX - b.minX, b.maxY - b.minY);
        };
        if (set != lastSet) {
            std::fprintf(stderr, "[nav] f=%llu buttons=%u pointer=(%.0f,%.0f) lock=%u\n", n, static_cast<unsigned>(buttons.size()), ptr[0], ptr[1], s.inputLock);
            for (const Button& b : buttons) print(b);
        } else if (hover != lastHover) {
            std::fprintf(stderr, "[nav] f=%llu pointer=(%.0f,%.0f) now on:\n", n, ptr[0], ptr[1]);
            for (const Button& b : buttons) if (b.hover) print(b);
        }
        lastSet = set; lastHover = hover;
    }
}



// ---------------------------------------------------------------------------------------------
// Registered regions and the hash line
// ---------------------------------------------------------------------------------------------
struct Region { std::string name; const uint8_t* base; uint32_t size; };
std::vector<Region> g_regions;

uint64_t RegionHash() {
    uint64_t h = 1469598103934665603ull;
    for (const Region& r : g_regions) {
        for (uint32_t i = 0; i + 8 <= r.size; i += 8) {
            uint64_t w;
            std::memcpy(&w, r.base + i, 8);
            h = (h ^ w) * 1099511628211ull;
            h ^= h >> 29;
        }
    }
    return h;
}
void HashLine(unsigned long long n, bool final) {
    // deep=<game checksum of the whole simulation> rng=<seed of the simulation's random numbers> (only while a match is simulated)
    uint32_t chk = 0, rng = 0;
    char deep[64] = "";
    if (DeepDigest(&chk, &rng)) std::snprintf(deep, sizeof deep, " deep=%08x rng=%08x", chk, rng);
    if (g_regions.empty()) std::fprintf(stderr, "[hash] f=%llu sim=%016" PRIx64 "%s%s\n", n, SimDigest(), deep, final ? " (final)" : "");
    else std::fprintf(stderr, "[hash] f=%llu sim=%016" PRIx64 " ram=%016" PRIx64 "%s%s\n", n, SimDigest(), RegionHash(), deep, final ? " (final)" : "");
}

// (a plain array: NpHarnessAtExit is called from allocations made by other layers' static initialisers, before this file's own
// dynamically initialised objects exist; a std::vector here would be constructed afterwards and lose the hook)
void (*g_exitHooks[16])();
unsigned g_nExitHooks = 0;

// MSC_FRAME_TIMING (from Codex): intervals between presented frames in windows, with the game
// identity the window belongs to. At most 1024 records.
unsigned TimingWindow() {
    static const unsigned window = NpFrameTiming::ParseWindow(std::getenv("MSC_FRAME_TIMING"));
    return window;
}
unsigned g_timingRecords = 0;
constexpr unsigned kTimingRecordLimit = 1024;
NpFrameTiming::Window& TimingSamples() {
    static NpFrameTiming::Window samples(TimingWindow());
    return samples;
}
const char* TimingReason(NpFrameTiming::Reason reason) {
    switch (reason) {
    case NpFrameTiming::Reason::Window: return "window";
    case NpFrameTiming::Reason::Identity: return "identity";
    case NpFrameTiming::Reason::Reset: return "reset";
    case NpFrameTiming::Reason::End: return "end";
    }
    return "unknown";
}
void EmitTiming(const NpFrameTiming::Event& event) {
    if (event.hasReport && g_timingRecords < kTimingRecordLimit) {
        ++g_timingRecords;
        const auto& r = event.report;
        // W5 RT: the input delay of the frames shown since the previous line (np/latency.h: pad read -> the refresh that shows the frame)
        uint32_t latN = 0, latP50 = 0, latP95 = 0;
        if (&NpLatTakeWindow) NpLatTakeWindow(&latN, &latP50, &latP95);
        std::fprintf(stderr, "[frame-timing] reason=%s first_frame=%llu last_frame=%llu n=%u start_us=%llu end_us=%llu "
                     "total_us=%llu min_us=%llu p50_us=%llu p95_us=%llu p99_us=%llu max_us=%llu over_33333=%u over_16667=%u "
                     "scene=%d top=%08x game=%08x gs=%d task=%u paused=%u gpu_complete=0 lat_n=%u lat_p50_us=%u lat_p95_us=%u\n",
                     TimingReason(r.reason), (unsigned long long)r.firstFrame, (unsigned long long)r.lastFrame, r.count,
                     (unsigned long long)r.startUs, (unsigned long long)r.endUs, (unsigned long long)r.totalUs,
                     (unsigned long long)r.minUs, (unsigned long long)r.p50Us, (unsigned long long)r.p95Us,
                     (unsigned long long)r.p99Us, (unsigned long long)r.maxUs, r.over33333, r.over16667,
                     r.segment.scene, r.segment.sceneObject, r.segment.game, r.segment.gameState, r.segment.task,
                     unsigned(r.segment.paused), latN, latP50, latP95);
    }
    if (event.boundary && g_timingRecords < kTimingRecordLimit) {
        ++g_timingRecords;
        std::fprintf(stderr, "[frame-timing-boundary] reason=%s from_frame=%llu to_frame=%llu gap_us=%llu "
                     "from_scene=%d to_scene=%d from_game=%08x to_game=%08x from_gs=%d to_gs=%d gpu_complete=0\n",
                     event.reset ? "reset" : "identity", (unsigned long long)event.fromFrame,
                     (unsigned long long)event.toFrame, (unsigned long long)event.gapUs,
                     event.from.scene, event.to.scene, event.from.game, event.to.game,
                     event.from.gameState, event.to.gameState);
    }
}
void RecordTiming(uint64_t frame, uint64_t nowUs) {
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::fprintf(stderr, "[frame-timing-config] clock=real-monotonic hook=game-frame-accounting metric=accounting-pacing-interval "
                     "gpu_complete=0 window=%u max_records=%u percentile=nearest-rank\n", TimingWindow(), kTimingRecordLimit);
    }
    State s;
    SampleState(s, nullptr, nullptr);
    NpFrameTiming::Segment seg;
    seg.scene = s.scene.depth ? s.scene.ids[s.scene.depth - 1] : -1;
    seg.sceneObject = static_cast<uint32_t>(s.scene.top);
    seg.game = static_cast<uint32_t>(s.game);
    seg.gameState = s.gameState;
    seg.task = s.taskState;
    seg.paused = s.paused;
    EmitTiming(TimingSamples().Add(frame, nowUs, seg));
}
void FinishFrameTiming() {
    if (TimingWindow()) EmitTiming(TimingSamples().Finish());
}

void Init() {
    g_initDone = true;
    g_realStart = NpRealSeconds();
#if defined(__linux__)
    // For profile symbolisation: where the executable and its libraries sit.
    if (std::getenv("MSC_DUMP_MAPS")) {
        if (FILE* m = std::fopen("/proc/self/maps", "r")) {
            char line[512];
            while (std::fgets(line, sizeof line, m))
                if (std::strstr(line, "r-x") || std::strstr(line, "r-xp")) std::fprintf(stderr, "[maps] %s", line);
            std::fclose(m);
        }
    }
#endif
    if (const char* w = std::getenv("MSC_WATCH")) {
        if (*w && std::strcmp(w, "0") != 0) {
            g_watch = true;
            if (std::strcmp(w, "1") != 0) {
                std::string spec = w;
                size_t pos = 0;
                while (pos < spec.size()) {
                    size_t end = spec.find(',', pos);
                    if (end == std::string::npos) end = spec.size();
                    const std::string item = spec.substr(pos, end - pos);
                    const size_t a = item.find(':'), b = item.find(':', a == std::string::npos ? a : a + 1);
                    if (a != std::string::npos && b != std::string::npos)
                        g_extras.push_back({item.substr(0, a), item.substr(a + 1, b - a - 1), item.substr(b + 1)});
                    pos = end + 1;
                }
            }
        }
    }
    if (const char* e = std::getenv("MSC_WATCH_EVERY")) g_watchEvery = std::strtoull(e, nullptr, 10);
    if (const char* e = std::getenv("MSC_WATCH_BUTTONS")) g_watchButtons = *e && *e != '0';
    if (const char* e = std::getenv("MSC_FRAME_TRACE")) g_frameTrace = *e && *e != '0';
    if (const char* e = std::getenv("MSC_STATE_HASH")) g_hashEvery = std::strtoull(e, nullptr, 10);
    if (const char* e = std::getenv("MSC_VIRTUAL_FRAME_US")) g_frameCostNs = std::strtoll(e, nullptr, 10) * 1000;
    if (const char* e = std::getenv("MSC_VIRTUAL_STEP_US")) g_stepCostNs = std::strtoll(e, nullptr, 10) * 1000;
    if (const char* e = std::getenv("MSC_FPS_MODEL")) g_fpsModel = *e && std::strcmp(e, "0") != 0 && std::strcmp(e, "off") != 0;
    LoadScript();
    if (const char* chk = std::getenv("MSC_SCRIPT_CHECK")) {
        // Parse-only: `MSC_SCRIPT_CHECK=1 MSC_INPUT_SCRIPT=f` reports the script's size and leaves.
        if (*chk && *chk != '0') {
            std::fprintf(stderr, "[script] OK: %lu absolute line(s), %lu command(s)\n", static_cast<unsigned long>(g_script.size()), static_cast<unsigned long>(g_prog.size()));
            std::fflush(nullptr);
            HarnessExit(0);
        }
    }
    if (NpVirtualTimeActive())
        std::fprintf(stderr, "[plat] virtual time: frame cost %lld us, step cost %lld us\n",
                     static_cast<long long>(g_frameCostNs / 1000), static_cast<long long>(g_stepCostNs / 1000));
    ProgramTick();
}

void Finish(int code, const char* why) {
    if (NpFsJoin) NpFsJoin();   // a frame limit or a script's exit comes from the frame hook, which runs right after the fork: finish the send first
    const unsigned long long n = g_frames.load(std::memory_order_relaxed);
    if (g_watch) WatchLine(g_state, n);
    if (g_hashEvery) HashLine(n, true);
    // The process leaves without running exit handlers: the other layers' end-of-run reports
    // (the platform's memory high-water marks, the SDK layer's statistics) are asked for here.
    for (unsigned i = 0; i < g_nExitHooks; ++i) g_exitHooks[i]();
    if (NpJobsLogStats) NpJobsLogStats("the end of the run");
    if (NpFsLogStats) NpFsLogStats("the end of the run");
    if (NpThpLogStats) NpThpLogStats();
    if (__gcov_dump) __gcov_dump();   // the process leaves without exit handlers: write the profile now
    if (NpVitaExitReport) NpVitaExitReport();
    FinishFrameTiming();
    std::fprintf(stderr, "[plat] %s at frame %llu, exiting\n", why, n);
    if (NpVitaEndRun) NpVitaEndRun(code, why);
    std::fflush(nullptr);
#if defined(__linux__)
    // If the SDK layer routes stdout/stderr through a pipe with pump threads, _Exit would drop
    // whatever they have not read yet: wait (in real time) until the pipes are empty.
    for (int i = 0; i < 2000; ++i) {
        int pending1 = 0, pending2 = 0;
        if (ioctl(STDOUT_FILENO, FIONREAD, &pending1) != 0) pending1 = 0;
        if (ioctl(STDERR_FILENO, FIONREAD, &pending2) != 0) pending2 = 0;
        if (pending1 <= 0 && pending2 <= 0) break;
        poll(nullptr, 0, 1);
    }
    poll(nullptr, 0, 30);
#endif
    HarnessExit(code);
}

bool HarnessWanted() {
    static const bool wanted = [] {
        static const char* const names[] = {"MSC_INPUT_SCRIPT", "MSC_WATCH", "MSC_WATCH_BUTTONS", "MSC_MAX_FRAMES",
                                            "MSC_FRAME_TRACE",  "MSC_STATE_HASH", "MSC_QEMU_MARK", "MSC_FRAME_LOG",
                                            "MSC_FRAME_TIMING", "MSC_VIRTUAL_FRAME_US", "MSC_VIRTUAL_STEP_US"};
        for (const char* name : names) {
            const char* value = std::getenv(name);
            if (value && *value) {
#if defined(__vita__)
                std::fprintf(stderr, "[vita] test harness active (%s=%s): scripted input / state log / frame limit\n", name, value);
#endif
                return true;
            }
        }
        return false;
    }();
    return wanted;
}


} // namespace
} // namespace NpHarness

using namespace NpHarness;

// ---------------------------------------------------------------------------------------------
// The frame hook (the body of the recompiled build's PlatFrameStats::OnFramePresented)
// ---------------------------------------------------------------------------------------------
namespace {
std::atomic<uint64_t> g_countAlways{0};

void OnFrameBody() {
    if (!g_initDone) Init();
    static const double start = NpRealSeconds();
    static double last = start;
    auto threadCpu = [] {
#if defined(__linux__) && defined(CLOCK_THREAD_CPUTIME_ID)   // not in VitaSDK's newlib; the Vita's own status line has the figure
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return ts.tv_sec + ts.tv_nsec * 1e-9;
#else
        return 0.0;
#endif
    };
    static double lastCpu = threadCpu();
    static const uint64_t maxFrames = std::getenv("MSC_MAX_FRAMES") ? std::strtoull(std::getenv("MSC_MAX_FRAMES"), nullptr, 10) : 0;
#if defined(__vita__)
    constexpr uint64_t kDefaultFrameLog = 0;   // the Vita platform layer logs its own line every 600 frames
#else
    constexpr uint64_t kDefaultFrameLog = 60;
#endif
    static const uint64_t every = std::getenv("MSC_FRAME_LOG") ? std::strtoull(std::getenv("MSC_FRAME_LOG"), nullptr, 10) : kDefaultFrameLog;
    const uint64_t n = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(__linux__)
    // Under qemu-user with tools/qemu-plugin/framecount.c loaded, this (invalid) syscall makes
    // the plugin print per-thread instruction counts. On a real kernel it fails with ENOSYS.
    static const uint64_t markEvery = std::getenv("MSC_QEMU_MARK") ? std::strtoull(std::getenv("MSC_QEMU_MARK"), nullptr, 10) : 0;
    if (markEvery && n % markEvery == 0) syscall(0x7f00, static_cast<long>(n));
#endif
    if (every && n % every == 0) {
        const double now = NpRealSeconds();
        const double span = now - last;
        const double cpu = threadCpu();
        static uint64_t lastReads = 0;
        const uint64_t reads = NpVirtualReads();
        std::fprintf(stderr, "[plat] frame %llu  t=%.2fs  last %llu frames: %.1f fps, game thread %.3f ms CPU/frame",
                     (unsigned long long)n, now - start, (unsigned long long)every, every / (span > 0 ? span : 1e-9),
                     (cpu - lastCpu) * 1000.0 / every);
        if (NpVirtualTimeActive())
            std::fprintf(stderr, ", virtual t=%.3fs, %.0f clock reads/frame", NpVirtualSeconds(),
                         static_cast<double>(reads - lastReads) / every);
        std::fputc('\n', stderr);
        lastReads = reads;
        last = now;
        lastCpu = cpu;
    }
    // State after this frame; the script decides the next frame's input from it.
    const bool needState = g_watch || g_watchButtons || g_frameTrace || !g_prog.empty() || g_stepCostNs != 0 || g_fpsModel;
    uint32_t modelSteps = 0;
    if (needState) {
        const State prev = g_state;
        std::vector<Button> buttons;
        float ptr[2] = {0, 0};
        SampleState(g_state, &buttons, ptr);
        const uint32_t steps = g_state.simFrame >= prev.simFrame ? g_state.simFrame - prev.simFrame : 0;
        ++g_stepHist[steps > 5 ? 5 : steps];
        if (g_stepCostNs) NpVirtualAdvanceNs(g_stepCostNs * static_cast<int64_t>(steps));
        modelSteps = steps;
        if (g_watch || g_watchButtons) ReportChanges(prev, g_state, n, buttons, ptr);
        if (g_frameTrace)
            std::fprintf(stderr, "[ft] %llu %u %u %d\n", (unsigned long long)n, g_state.simFrame, g_state.retrace, g_state.gameState);
        if (g_watch && g_watchEvery && n % g_watchEvery == 0) WatchLine(g_state, n);
    }
    if (NpItemTestFrame) NpItemTestFrame(n);   // W5 ITEM: item slots, objects, hits and effects -> [item] lines (only with MSC_ITEM_TEST=1)
    if (g_frameCostNs) NpVirtualAdvanceNs(g_frameCostNs);
    if (g_fpsModel && &NpFpsModelCharge != 0) {
        // FPS (np/fps.h): the cost of this frame on the busiest core, by kind of scene (the steady-state period of the pipelined build)
        const int sceneId = g_state.scene.depth ? static_cast<int>(g_state.scene.ids[g_state.scene.depth - 1]) : -1;
        const long long ns = NpFpsModelCharge(g_state.taskState, sceneId, modelSteps);
        if (ns > 0) NpVirtualAdvanceNs(ns);
    }
    if (g_hashEvery && n % g_hashEvery == 0) HashLine(n, false);
    if (maxFrames && n >= maxFrames) {
        char why[64];
        std::snprintf(why, sizeof why, "reached MSC_MAX_FRAMES=%llu", (unsigned long long)maxFrames);
        Finish(0, why);
    }
    ProgramTick();
}
} // namespace

extern "C" {

void NpOnFramePresented(void) {
    g_countAlways.fetch_add(1, std::memory_order_relaxed);
    // Sample first, before the platform's own logging and console work.
    const bool timing = TimingWindow() && g_timingRecords < kTimingRecordLimit;
    const uint64_t timingNowUs = timing ? static_cast<uint64_t>(NpRealSeconds() * 1e6) : 0;
    if (NpVitaOnFramePresented) NpVitaOnFramePresented();
    if (timing) RecordTiming(g_countAlways.load(std::memory_order_relaxed), timingNowUs);
    if (!HarnessWanted()) return;
#if defined(__vita__)
    // An unattended run: nobody touches the pad, so the console would dim the screen and then
    // suspend (the system counts idle time from the real controls, not from what a script feeds
    // the game). Tell it the application is in use.
    if (g_countAlways.load(std::memory_order_relaxed) % 120 == 0) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
#endif
    // Last: with a frame limit or a script's "exit" this call does not return.
    OnFrameBody();
}

int NpHarnessFillPad(NpPadState* st) {
    if (!st || !HarnessWanted()) return 0;
    if (!g_initDone) Init();
    bool changed = false;
    const uint64_t n = g_frames.load(std::memory_order_relaxed);
    for (const Step& s : g_script) {
        if (n >= s.first && n <= s.last) {
            st->buttons = static_cast<uint16_t>(s.buttons);
            st->stickX = static_cast<int8_t>(s.sx);
            st->stickY = static_cast<int8_t>(s.sy);
            changed = true;
        }
    }
    if (g_padOut.active) {
        st->buttons = static_cast<uint16_t>(g_padOut.buttons);
        st->stickX = static_cast<int8_t>(g_padOut.sx);
        st->stickY = static_cast<int8_t>(g_padOut.sy);
        st->substickX = static_cast<int8_t>(g_padOut.cx);
        st->substickY = static_cast<int8_t>(g_padOut.cy);
        changed = true;
    }
    // A digital shoulder click implies a full analog pull, as on a real pad.
    if (st->buttons & 0x40) st->triggerL = 255;   // PAD_TRIGGER_L
    if (st->buttons & 0x20) st->triggerR = 255;   // PAD_TRIGGER_R
    g_lastPadButtons = st->buttons;
    g_lastPadSx = st->stickX;
    g_lastPadSy = st->stickY;
    return changed ? 1 : 0;
}

int NpHarnessFillRaw(NpHarnessRaw* raw) {
    if (!raw || !HarnessWanted()) return 0;
    if (!g_initDone) Init();
    if (!g_rawOut.active) return 0;
    std::memset(raw, 0, sizeof(*raw));
    raw->buttons = g_rawOut.buttons;
    raw->stickX = static_cast<int8_t>(g_rawOut.sx);
    raw->stickY = static_cast<int8_t>(g_rawOut.sy);
    raw->substickX = static_cast<int8_t>(g_rawOut.cx);
    raw->substickY = static_cast<int8_t>(g_rawOut.cy);
    if (g_rawOut.touchKind == 1) {
        raw->touchCount = 1;
        raw->touch[0].id = g_rawOut.touchId; raw->touch[0].x = g_rawOut.tx; raw->touch[0].y = g_rawOut.ty;
    } else if (g_rawOut.touchKind == 2) {
        raw->rearCount = 1;
        raw->rear[0].id = g_rawOut.touchId; raw->rear[0].x = g_rawOut.tx; raw->rear[0].y = g_rawOut.ty;
    }
    g_lastPadButtons = 0;
    return 1;
}

void NpHarnessFinish(int code, const char* why) { Finish(code, why ? why : "harness finish"); HarnessExit(code); }
int NpHarnessActive(void) { return HarnessWanted() ? 1 : 0; }
// 17 Oct (the maintainer play test): only an input script owns the controller. Logging alone (MSC_WATCH, MSC_FRAME_TIMING, ...) also makes the
// harness active, and the Vita's input source used to stop reading the real pad then: the buttons did nothing.
int NpHarnessOwnsPad(void) {
    if (!HarnessWanted()) return 0;
    if (!g_initDone) Init();
    const char* s = std::getenv("MSC_INPUT_SCRIPT");
    return ((s && *s) || !g_script.empty() || !g_prog.empty()) ? 1 : 0;
}
uint64_t NpHarnessFrameCount(void) { return g_countAlways.load(std::memory_order_relaxed); }
void NpHarnessRegisterRegion(const char* name, const void* base, uint32_t size) {
    if (base && size) g_regions.push_back({name ? name : "?", static_cast<const uint8_t*>(base), size});
}
void NpHarnessAtExit(void (*fn)(void)) { if (fn && g_nExitHooks < 16) g_exitHooks[g_nExitHooks++] = fn; }

} // extern "C"

// ---------------------------------------------------------------------------------------------
// Task marks (MSC_TASK_MARKS=1, armhf under qemu with tools/qemu-plugin/libframecount.so): the
// instruction counts of every nlTask of the game's task loop, per presented frame. The task manager
// (src/NL/nlTask.cpp, nlTaskManager::RunAllTasks) calls NpTaskBegin / NpTaskEnd around each
// `taskIterator->Run(deltaTime)` (a weak reference there: four lines, np/docs/vita.md "Task
// budgets"); each call issues the plugin's mark syscall with 0x20000000 + 2 * index (+1 at the end);
// platform/harness/tools/task_budget.py turns the plugin's cumulative counts into a table. Do not
// combine with the plugin's profile= option (any mark value above `from` would open its window).
// ---------------------------------------------------------------------------------------------
namespace {
bool TaskMarksOn() {
    static const bool on = [] { const char* e = std::getenv("MSC_TASK_MARKS"); return e && *e && *e != '0'; }();
    return on;
}
} // namespace
extern "C" void NpTaskBegin(const char* name, unsigned index) {
    if (!TaskMarksOn() || index > 255) return;
    static bool named[256];
    if (!named[index]) { named[index] = true; std::fprintf(stderr, "[taskmarks] %u=%s\n", index, name ? name : "?"); }
#if defined(__linux__)
    syscall(0x7f00, 0x20000000L + static_cast<long>(index) * 2);
#endif
}
extern "C" void NpTaskEnd(const char*, unsigned index) {
    if (!TaskMarksOn() || index > 255) return;
#if defined(__linux__)
    syscall(0x7f00, 0x20000000L + static_cast<long>(index) * 2 + 1);
#endif
}
