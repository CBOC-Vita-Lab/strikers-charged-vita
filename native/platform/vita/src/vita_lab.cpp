// Lab mode: a remote test loop INSIDE the application (LAB=1 builds only).
//
// Why: every test run on the owner's console used to cost his hands (open VitaShell for the
// file transfer, close it, start the game, reopen VitaShell for the log), and nothing outside
// this application's own folders may be installed on his console. So the remote control
// lives here. He starts the application once; from then on a client on the same network
// (tools/vitalab.py) uploads builds and settings, starts runs and fetches logs.
//
// How a lab build behaves (unless the file ux0:data/mscharged/nolab exists):
//   1. main() calls ServeBeforeRun() first. It brings the network up and serves one client at
//      a time on TCP port 1339 (text commands, one per line; see HandleCommand). The text
//      console shows the address. START on the pad = "play normally": lab mode is left and the
//      game boots as an ordinary start would.
//   2. RUN [max_seconds] closes the sockets, shuts the network library down and unloads its
//      module, re-reads env.txt, starts the watchdog thread and returns: the game boots in
//      this same process with nothing of the network stack left running.
//   3. The run ends in one of these ways, and each of them ends in EndRun():
//        - any exit call (the harness's script exit, main returning, a fatal error screen,
//          exit()/abort() anywhere): the link wraps sceKernelExitProcess and _exit;
//        - a crash caught by the crash handler (vita_crash.cpp calls CrashEnd());
//        - the watchdog: time limit (RUN's argument, default 900 s; taken by the game thread at
//          its next frame end, by the watchdog itself if no frame ends within 15 s) or a stall
//          (no presented frame for 120 s after the first one, 300 s before it; MSC_LAB_STALL_S
//          / MSC_LAB_STALL_BOOT_S in env.txt change the two).
//      EndRun() writes the last log line and the status file, then
//      sceAppMgrLoadExec("app0:eboot.bin"): the application starts again from the top and is
//      back in the command server within seconds. If the reload is refused it exits to the
//      home screen (the owner starts it again; the log and the status are on disk either way).
//   4. EXEC <file> starts another executable of ours from the application folder (a new build
//      uploaded under ANOTHER name: the running eboot.bin is never replaced while it runs).
//      "EXEC <file> RUN [s]" makes that executable begin its run at once, without a server
//      phase: a run in a process that never loaded the network module and read env.txt at
//      the ordinary place.
//
// Files (all under ux0:data/mscharged/):
//   lab.txt          this process's log while it serves commands (appended, rotated at 2 MiB)
//   log.txt          the complete log of ONE run: created at RUN (the previous one is kept as
//                    log.prev.txt), beginning with everything this process logged before
//   lab/state.txt    one line: "running ..." while a run is on, "ended: <how> ..." after it.
//                    A server that finds "running" at start-up knows the last run died without
//                    passing EndRun() (a fault the system handled itself, power loss).
//   lab/exec.txt     written by EXEC for the executable it starts, consumed at once
//   <file>.part      an upload in progress; PUTR (the client's resumable upload) keeps it when
//                    the connection breaks and continues from it the next time
//
// What the server will touch: only ux0:data/mscharged/ and this application's own folder
// ux0:app/<title id>/ (also reachable as app0:). Never anything else: paths are rebuilt from
// those two roots, ".." and device names are refused. Inside them the game data (game/),
// the package's system files (sce_sys/ ...) and the eboot.bin that is running are read-only.
//
// The watchdog: one thread, priority 64 (above every thread of the game), on core 2 by
// default (MSC_LAB_WD_CORE). It waits on a semaphore with a one-second timeout, so it runs
// once a second for the time of three system calls and two comparisons (a few tens of
// microseconds, well under 0.01 % of a core); every tenth pass it tells the system the
// application is in use so that the console neither dims nor suspends during a run.
#if defined(__vita__) && defined(MSC_LAB)

#include "vita_lab.h"
#include "np_vita_internal.h"

#include <kubridge.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <strings.h>

#ifndef MSC_LAB_BUILD_ID
#define MSC_LAB_BUILD_ID "unnamed"
#endif

extern "C" {
extern const char kMscVitaVariantTag[];   // vita_process.cpp: "gl", "null", "probe"
extern const char kMscVitaTitleId[];      // the title id the package is built with
int __real_sceKernelExitProcess(int code);
void __real__exit(int code);
}

// np_harness.cpp: ends the run as a script's exit does (end-of-run reports first).
extern "C" void NpHarnessFinish(int code, const char* why) __attribute__((weak));

namespace VitaLab {
namespace {

constexpr int kPort = 1339;
constexpr unsigned kDefaultMaxSeconds = 900;
constexpr const char* kDataDir = "ux0:data/mscharged";
constexpr const char* kLabLog = "ux0:data/mscharged/lab.txt";
constexpr const char* kRunLog = "ux0:data/mscharged/log.txt";
constexpr const char* kPrevLog = "ux0:data/mscharged/log.prev.txt";
constexpr const char* kLabDir = "ux0:data/mscharged/lab";
constexpr const char* kStateFile = "ux0:data/mscharged/lab/state.txt";
constexpr const char* kExecFile = "ux0:data/mscharged/lab/exec.txt";
constexpr const char* kNoLabFile = "ux0:data/mscharged/nolab";
constexpr const char* kMonitor = "app0:eboot.bin";
constexpr SceSize kNetMemory = 1024 * 1024;   // what ftpvita gives sceNet for file transfers
constexpr SceSize kIoMemory = 256 * 1024;

int g_decided = 0;                 // 0 = not yet, 1 = lab mode, -1 = not lab mode
std::atomic<bool> g_runActive{false};
std::atomic<bool> g_ending{false};
std::atomic<bool> g_crashPending{false};
std::atomic<int> g_endRequest{-1};   // >= 0: the game thread is to end the run with this code at its next frame
char g_reason[200];
std::atomic<bool> g_haveReason{false};
char g_title[16];
char g_appDir[40];                 // "ux0:app/MSCH00002"
char g_self[64] = "eboot.bin";     // the file this process was started from, as far as known
char g_lastRun[240] = "none";      // how the previous run ended (state file)
uint64_t g_runStartUs = 0;
unsigned g_maxSeconds = kDefaultMaxSeconds, g_stallSeconds = 120, g_stallBootSeconds = 300;
unsigned g_wdPollMs = 0;           // MSC_LAB_WD_POLL_MS: the watchdog sleeps this long per pass instead of waiting on its semaphore
SceUID g_wdSema = -1, g_wdThread = -1;

// ---- small helpers --------------------------------------------------------------------------

uint64_t NowUs() { return sceKernelGetProcessTimeWide(); }

bool Exists(const char* path, SceIoStat* out = nullptr) {
    SceIoStat st;
    std::memset(&st, 0, sizeof(st));
    const bool ok = sceIoGetstat(path, &st) >= 0;
    if (out) *out = st;
    return ok;
}

void WriteWholeFile(const char* path, const char* text) {
    // (Removed first: Vita3K does not shorten an existing file on SCE_O_TRUNC, and a status
    // line followed by the tail of an older, longer one is worse than no line.)
    sceIoRemove(path);
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    sceIoWrite(fd, text, static_cast<SceSize>(std::strlen(text)));
    sceIoClose(fd);
}

int ReadWholeFile(const char* path, char* out, int size) {
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    int got = sceIoRead(fd, out, static_cast<SceSize>(size - 1));
    sceIoClose(fd);
    if (got < 0) got = 0;
    out[got] = '\0';
    while (got > 0 && (out[got - 1] == '\n' || out[got - 1] == '\r' || out[got - 1] == ' ')) out[--got] = '\0';
    return got;
}

void WriteState(const char* format, ...) {
    char text[400];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text) - 1, format, args);
    va_end(args);
    std::strcat(text, "\n");
    sceIoMkdir(kLabDir, 0777);
    WriteWholeFile(kStateFile, text);
}

uint32_t Crc32(uint32_t crc, const void* data, size_t size) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        built = true;
    }
    const unsigned char* p = static_cast<const unsigned char*>(data);
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

// ---- which paths a client may name -----------------------------------------------------------

enum class Access { Read, Write };

// Rebuilds `in` as "<root>/<rest>" with <root> one of the two allowed folders, or refuses.
// `*underApp` / `*rest`: which root, and the part after it (empty = the root itself).
bool CheckPath(const char* in, Access access, char* out, size_t outSize, const char** why, const char** restOut = nullptr) {
    while (*in == ' ') ++in;
    const char* rest = nullptr;
    bool underApp = false;
    const size_t titleLength = std::strlen(g_title);
    if (!std::strncmp(in, "app0:", 5)) {
        rest = in + 5;
        underApp = true;
        if (*rest == '/') ++rest;
    } else if (!std::strncmp(in, "ux0:", 4)) {
        const char* p = in + 4;
        if (*p == '/') ++p;
        if (!std::strncmp(p, "data/mscharged", 14) && (p[14] == '/' || p[14] == '\0')) {
            rest = p + 14;
        } else if (!std::strncmp(p, "app/", 4) && !std::strncmp(p + 4, g_title, titleLength) &&
                   (p[4 + titleLength] == '/' || p[4 + titleLength] == '\0')) {
            rest = p + 4 + titleLength;
            underApp = true;
        }
        if (rest && *rest == '/') ++rest;
    }
    if (!rest) {
        *why = "path outside ux0:data/mscharged/ and this application's folder";
        return false;
    }
    // Component by component: nothing that could leave the root or name another device.
    size_t length = std::strlen(rest);
    while (length != 0 && rest[length - 1] == '/') --length;
    if (length > 200) {
        *why = "path too long";
        return false;
    }
    size_t start = 0;
    for (size_t i = 0; i <= length; ++i) {
        const unsigned char c = i < length ? static_cast<unsigned char>(rest[i]) : '/';
        if (c == '/') {
            const size_t n = i - start;
            if (length != 0 && (n == 0 || (n == 1 && rest[start] == '.') || (n == 2 && rest[start] == '.' && rest[start + 1] == '.'))) {
                *why = "empty, '.' or '..' path component";
                return false;
            }
            // (The memory card's file system ignores case and may ignore trailing dots and
            // spaces: "GAME./x" must not be a way around the read-only names below.)
            if (n != 0 && (rest[i - 1] == '.' || rest[i - 1] == ' ' || rest[start] == ' ')) {
                *why = "path component begins with a space or ends with a dot or a space";
                return false;
            }
            start = i + 1;
        } else if (c < 0x20 || c == 0x7F || c == ':' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            *why = "character not allowed in a path";
            return false;
        }
    }
    if (access == Access::Write) {
        const auto firstIs = [&](const char* name) {
            const size_t n = std::strlen(name);
            return length >= n && !strncasecmp(rest, name, n) && (length == n || rest[n] == '/');   // the card's file system ignores case
        };
        if (!underApp && firstIs("game")) {
            *why = "the game data (game/) is read-only for the lab server";
            return false;
        }
        if (underApp && (firstIs("sce_sys") || firstIs("sce_pfs") || firstIs("sce_module"))) {
            *why = "the package's system files are read-only for the lab server";
            return false;
        }
        if (underApp && length == std::strlen(g_self) && !strncasecmp(rest, g_self, length)) {
            *why = "that is the executable that is running: upload under another name and EXEC it (this one can be replaced from there)";
            return false;
        }
    }
    std::snprintf(out, outSize, "%s%s%.*s", underApp ? g_appDir : kDataDir, length ? "/" : "", static_cast<int>(length), rest);
    if (restOut) *restOut = out + std::strlen(underApp ? g_appDir : kDataDir) + (length ? 1 : 0);
    return true;
}

// Creates the folders above `path` (which CheckPath produced), below its root.
void MakeParents(char* path, const char* rest) {
    for (char* p = const_cast<char*>(rest); *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        sceIoMkdir(path, 0777);
        *p = '/';
    }
}

// ---- network ---------------------------------------------------------------------------------

bool g_moduleLoadedHere = false, g_netInitedHere = false, g_ctlInitedHere = false;
SceUID g_netBlock = -1, g_ioBlock = -1;
char* g_io = nullptr;              // kIoMemory bytes: transfer buffer / listing text
int g_listen = -1;
char g_ip[24] = "";
char g_netError[96] = "";
unsigned g_commands = 0;
char g_lastCommand[72] = "(none yet)";

void NetDown() {
    if (g_listen >= 0) sceNetSocketClose(g_listen);
    g_listen = -1;
    if (g_ctlInitedHere) sceNetCtlTerm();
    g_ctlInitedHere = false;
    if (g_netInitedHere) sceNetTerm();
    g_netInitedHere = false;
    if (g_netBlock >= 0) sceKernelFreeMemBlock(g_netBlock);
    g_netBlock = -1;
    if (g_moduleLoadedHere) sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    g_moduleLoadedHere = false;
    g_ip[0] = '\0';
}

bool NetFail(const char* what, int code) {
    std::snprintf(g_netError, sizeof(g_netError), "%s: 0x%08X", what, static_cast<unsigned>(code));
    NetDown();
    return false;
}

bool NetUp() {
    if (g_listen >= 0) return true;
    int r;
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) {
        r = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        if (r < 0) return NetFail("loading the network module", r);
        g_moduleLoadedHere = true;
    }
    g_netBlock = sceKernelAllocMemBlock("msc_lab_net", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, kNetMemory, nullptr);
    void* base = nullptr;
    if (g_netBlock < 0 || sceKernelGetMemBlockBase(g_netBlock, &base) < 0) return NetFail("memory for the network library", g_netBlock);
    SceNetInitParam param;
    std::memset(&param, 0, sizeof(param));
    param.memory = base;
    param.size = static_cast<int>(kNetMemory);
    r = sceNetInit(&param);
    if (r < 0 && static_cast<unsigned>(r) != static_cast<unsigned>(SCE_NET_ERROR_EBUSY)) return NetFail("sceNetInit", r);
    g_netInitedHere = r >= 0;
    r = sceNetCtlInit();
    if (r < 0 && static_cast<unsigned>(r) != 0x80412102u /* already initialised */) return NetFail("sceNetCtlInit", r);
    g_ctlInitedHere = r >= 0;

    const int s = sceNetSocket("msc_lab", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (s < 0) return NetFail("socket", s);
    int on = 1;
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &on, sizeof(on));
    SceNetSockaddrIn address;
    std::memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = SCE_NET_AF_INET;
    address.sin_port = sceNetHtons(kPort);
    address.sin_addr.s_addr = SCE_NET_INADDR_ANY;
    r = sceNetBind(s, reinterpret_cast<SceNetSockaddr*>(&address), sizeof(address));
    if (r >= 0) r = sceNetListen(s, 1);
    if (r < 0) {
        sceNetSocketClose(s);
        return NetFail("bind/listen on port 1339", r);
    }
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));   // accept() must not block: the pad is polled
    g_listen = s;
    g_netError[0] = '\0';
    return true;
}

void QueryAddress() {
    SceNetCtlInfo info;
    std::memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0 && info.ip_address[0])
        std::snprintf(g_ip, sizeof(g_ip), "%s", info.ip_address);
    else
        g_ip[0] = '\0';
}

void ShowScreen() {
    if (g_listen >= 0 && g_ip[0])
        VitaConsole::SetLine(1, 0xFFFF80, "LAB MODE: waiting for commands at %s port %d", g_ip, kPort);
    else if (g_listen >= 0)
        VitaConsole::SetLine(1, 0xFFFF80, "LAB MODE: listening on port %d, no network address yet (Wi-Fi?)", kPort);
    else
        VitaConsole::SetLine(1, 0xFF8080, "LAB MODE: no network (%s); retrying", g_netError);
    VitaConsole::SetLine(2, 0xFFFFFF, "START = play normally");
    VitaConsole::SetLine(3, 0xA0FFA0, "build %s (%s), started from %s", MSC_LAB_BUILD_ID, kMscVitaVariantTag, g_self);
    VitaConsole::SetLine(4, 0xC0C0FF, "last run: %.68s", g_lastRun);
    VitaConsole::SetLine(5, 0xE0E0A0, "commands served: %u, last: %.50s", g_commands, g_lastCommand);
    VitaConsole::Refresh();
}

// START on the pad, seen released first (a START still held from launching does not count).
bool StartPressed() {
    static bool seenReleased = false;
    SceCtrlData pad;
    std::memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(0, &pad, 1) <= 0) return false;
    const bool down = (pad.buttons & SCE_CTRL_START) != 0;
    if (!down) seenReleased = true;
    return down && seenReleased;
}

struct Conn {
    int s = -1;
    char buffer[2048];
    int have = 0, pos = 0;
};

bool IsTimeout(int r) {
    const unsigned u = static_cast<unsigned>(r);
    return u == static_cast<unsigned>(SCE_NET_ERROR_EAGAIN) || u == static_cast<unsigned>(SCE_NET_ERROR_ETIMEDOUT) ||
           u == static_cast<unsigned>(SCE_NET_ERROR_EINTR);
}

// Up to `max` bytes; waits at most `idleSeconds` for the first of them. > 0 bytes, 0 = the
// peer closed, < 0 = error or nothing came in time. (The socket's own receive timeout is one
// second, so that the console is not left without power ticks.)
int RecvSome(Conn& c, void* out, int max, int idleSeconds) {
    if (c.pos < c.have) {
        int n = c.have - c.pos;
        if (n > max) n = max;
        std::memcpy(out, c.buffer + c.pos, static_cast<size_t>(n));
        c.pos += n;
        return n;
    }
    for (int waited = 0; waited < idleSeconds; ++waited) {
        const int r = sceNetRecv(c.s, out, static_cast<unsigned>(max), 0);
        if (r >= 0) return r;
        if (!IsTimeout(r)) return r;
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    }
    return -1;
}

bool ReadLine(Conn& c, char* line, int size, int idleSeconds) {
    int n = 0;
    for (;;) {
        if (c.pos == c.have) {
            c.pos = 0;
            c.have = 0;
            int r = -1;
            for (int waited = 0; waited < idleSeconds; ++waited) {
                r = sceNetRecv(c.s, c.buffer, sizeof(c.buffer), 0);
                if (r >= 0 || !IsTimeout(r)) break;
                sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
                r = -1;
            }
            if (r <= 0) return false;
            c.have = r;
        }
        const char ch = c.buffer[c.pos++];
        if (ch == '\n') break;
        if (ch == '\r') continue;
        if (n + 1 >= size) return false;   // a line longer than any command: not our client
        line[n++] = ch;
    }
    line[n] = '\0';
    return true;
}

bool SendAll(int s, const void* data, int size) {
    const char* p = static_cast<const char*>(data);
    int idle = 0;
    while (size > 0) {
        const int r = sceNetSend(s, p, static_cast<unsigned>(size), 0);
        if (r > 0) {
            p += r;
            size -= r;
            idle = 0;
            continue;
        }
        if (r < 0 && IsTimeout(r) && ++idle < 20) continue;   // 20 x the 1 s send timeout
        return false;
    }
    return true;
}

bool Reply(Conn& c, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(text, sizeof(text) - 1, format, args);
    va_end(args);
    if (n < 0) n = 0;
    if (n > static_cast<int>(sizeof(text)) - 2) n = sizeof(text) - 2;
    text[n++] = '\n';
    return SendAll(c.s, text, n);
}

enum class Action { None, Close, Run, Play, Exec, Exit, Reboot };
struct Pending {
    unsigned seconds = kDefaultMaxSeconds;
    char file[64] = "";
    bool execRun = false;
};

// The server's log, while a client reads or replaces it.
struct LogPause {
    bool active;
    explicit LogPause(const char* path) : active(!std::strcmp(path, kLabLog)) {
        if (active) VitaLog::Pause();
    }
    ~LogPause() {
        if (active) VitaLog::Resume();
    }
};

bool PlainFileName(const char* name) {
    if (!*name || std::strlen(name) > 48 || name[0] == '.') return false;
    for (const char* p = name; *p; ++p) {
        const char ch = *p;
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

// The owner looking at the console during a long transfer: one line, once a second.
void ShowProgress(const char* what, const char* path, unsigned long done, unsigned long total) {
    static uint64_t lastUs = 0;
    const uint64_t now = NowUs();
    if (now - lastUs < 1000000u) return;
    lastUs = now;
    const char* name = std::strrchr(path, '/');
    VitaConsole::SetLine(5, 0xE0E0A0, "%s %.40s: %lu of %lu KiB", what, name ? name + 1 : path, done / 1024u, total / 1024u);
    VitaConsole::Refresh();
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
}

// PUT <size> <path>                 the content replaces <path> when all of it has arrived;
//                                   an interrupted transfer leaves nothing.
// PUTR <size> <crc32 hex> <path>    the same, resumable: what an interrupted transfer brought
//                                   stays in <path>.part; the answer "OK send <size> from <n>
//                                   <crc of those n bytes>" tells the client where to go on
//                                   (it compares the sum with its own file and removes the
//                                   .part if it is not the beginning of it). The whole file's
//                                   sum is checked BEFORE it replaces <path>.
Action CmdPut(Conn& c, char* arg, bool resumable) {
    char* end = nullptr;
    const unsigned long size = std::strtoul(arg, &end, 10);
    bool usage = end == arg || *end != ' ' || size > 512ul * 1024ul * 1024ul;
    uint32_t wantCrc = 0;
    if (!usage && resumable) {
        char* afterCrc = nullptr;
        wantCrc = static_cast<uint32_t>(std::strtoul(end + 1, &afterCrc, 16));
        usage = afterCrc != end + 9 || *afterCrc != ' ';
        end = afterCrc;
    }
    if (usage) {
        Reply(c, resumable ? "ERR usage: PUTR <size> <crc32 as 8 hex digits> <path>   (size at most 512 MiB)" : "ERR usage: PUT <size> <path>   (size at most 512 MiB)");
        return Action::None;
    }
    char path[280], part[288];
    const char* why = "";
    const char* rest = nullptr;
    if (!CheckPath(end + 1, Access::Write, path, sizeof(path), &why, &rest) || !*rest) {
        Reply(c, "ERR %s", *why ? why : "PUT needs a file below one of the two folders");
        return Action::None;
    }
    MakeParents(path, rest);
    std::snprintf(part, sizeof(part), "%s.part", path);
    LogPause pause(path);
    unsigned long have = 0;
    uint32_t crc = 0;
    SceIoStat st;
    if (resumable && Exists(part, &st) && !SCE_S_ISDIR(st.st_mode) && st.st_size > 0 && static_cast<unsigned long long>(st.st_size) <= size) {
        // What an interrupted upload left: its length and sum.
        const SceUID in = sceIoOpen(part, SCE_O_RDONLY, 0);
        if (in >= 0) {
            for (;;) {
                const int n = sceIoRead(in, g_io, kIoMemory);
                if (n <= 0) break;
                crc = Crc32(crc, g_io, static_cast<size_t>(n));
                have += static_cast<unsigned long>(n);
                ShowProgress("checking the partial", path, have, size);
            }
            sceIoClose(in);
        }
    }
    if (have == 0) sceIoRemove(part);   // (not left to SCE_O_TRUNC: see WriteWholeFile)
    const SceUID fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | (have ? 0 : SCE_O_TRUNC), 0666);
    if (fd < 0 || (have && sceIoLseek(fd, static_cast<SceOff>(have), SCE_SEEK_SET) != static_cast<SceOff>(have))) {
        if (fd >= 0) sceIoClose(fd);
        Reply(c, "ERR cannot create %s: 0x%08X", part, static_cast<unsigned>(fd));
        return Action::None;
    }
    const bool told = resumable ? Reply(c, "OK send %lu from %lu %08X", size, have, static_cast<unsigned>(crc)) : Reply(c, "OK send %lu", size);
    if (!told) {
        sceIoClose(fd);
        if (!resumable) sceIoRemove(part);
        return Action::Close;
    }
    unsigned long left = size - have;
    int writeError = 0;
    const uint64_t t0 = NowUs();
    while (left != 0) {
        // Fill the buffer before writing: large writes are what the memory card likes.
        int filled = 0;
        int r = 1;
        while (filled < static_cast<int>(kIoMemory) && static_cast<unsigned long>(filled) < left) {
            unsigned long want = left - static_cast<unsigned long>(filled);
            if (want > kIoMemory - static_cast<unsigned>(filled)) want = kIoMemory - static_cast<unsigned>(filled);
            r = RecvSome(c, g_io + filled, static_cast<int>(want), 20);
            if (r <= 0) break;
            filled += r;
        }
        if (filled != 0 && writeError == 0 && (r > 0 || resumable)) {
            crc = Crc32(crc, g_io, static_cast<size_t>(filled));
            const int w = sceIoWrite(fd, g_io, static_cast<SceSize>(filled));
            if (w != filled) writeError = w < 0 ? w : -1;
        }
        left -= static_cast<unsigned long>(filled);
        if (r <= 0) {
            sceIoClose(fd);
            if (resumable && writeError == 0) {
                std::fprintf(stderr, "[lab] PUTR %s: the transfer stopped at %lu of %lu bytes (recv 0x%08X); they are kept in %s\n", path, size - left, size,
                             static_cast<unsigned>(r), part);
            } else {
                sceIoRemove(part);
                std::fprintf(stderr, "[lab] PUT %s: the transfer stopped at %lu of %lu bytes (recv 0x%08X, write 0x%08X); nothing was changed\n", path,
                             size - left, size, static_cast<unsigned>(r), static_cast<unsigned>(writeError));
            }
            return Action::Close;
        }
        ShowProgress("receiving", path, size - left, size);
    }
    sceIoClose(fd);
    if (writeError != 0) {
        sceIoRemove(part);
        Reply(c, "ERR write to %s failed: 0x%08X (card full?); nothing was changed", part, static_cast<unsigned>(writeError));
        return Action::None;
    }
    if (resumable && crc != wantCrc) {
        sceIoRemove(part);
        Reply(c, "ERR checksum of the received file is %08X, expected %08X; nothing was changed", static_cast<unsigned>(crc), static_cast<unsigned>(wantCrc));
        return Action::None;
    }
    if (Exists(path)) {
        const int r = sceIoRemove(path);
        if (r < 0) {
            Reply(c, "ERR cannot replace %s (remove: 0x%08X); the new content is in %s", path, static_cast<unsigned>(r), part);
            return Action::None;
        }
    }
    const int r = sceIoRename(part, path);
    if (r < 0) {
        Reply(c, "ERR cannot rename %s to its name: 0x%08X", part, static_cast<unsigned>(r));
        return Action::None;
    }
    const unsigned ms = static_cast<unsigned>((NowUs() - t0) / 1000u);
    std::fprintf(stderr, "[lab] PUT %s: %lu bytes (%lu of them from an earlier attempt) in %u ms, crc32 %08X\n", path, size, have, ms, static_cast<unsigned>(crc));
    Reply(c, "OK %lu %08X", size, static_cast<unsigned>(crc));
    return Action::None;
}

Action CmdGet(Conn& c, char* arg) {
    char path[280];
    const char* why = "";
    if (!CheckPath(arg, Access::Read, path, sizeof(path), &why)) {
        Reply(c, "ERR %s", why);
        return Action::None;
    }
    LogPause pause(path);
    SceIoStat st;
    if (!Exists(path, &st) || SCE_S_ISDIR(st.st_mode)) {
        Reply(c, "ERR no such file: %s", path);
        return Action::None;
    }
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        Reply(c, "ERR cannot open %s: 0x%08X", path, static_cast<unsigned>(fd));
        return Action::None;
    }
    const unsigned long long size = static_cast<unsigned long long>(st.st_size);
    if (!Reply(c, "OK %llu", size)) {
        sceIoClose(fd);
        return Action::Close;
    }
    unsigned long long left = size;
    uint32_t crc = 0;
    while (left != 0) {
        const SceSize want = left > kIoMemory ? kIoMemory : static_cast<SceSize>(left);
        const int n = sceIoRead(fd, g_io, want);
        if (n <= 0 || !SendAll(c.s, g_io, n)) {
            sceIoClose(fd);
            return Action::Close;   // the client sees a short transfer
        }
        crc = Crc32(crc, g_io, static_cast<size_t>(n));
        left -= static_cast<unsigned long long>(n);
        ShowProgress("sending", path, static_cast<unsigned long>(size - left), static_cast<unsigned long>(size));
    }
    sceIoClose(fd);
    Reply(c, "OK crc32 %08X", static_cast<unsigned>(crc));
    return Action::None;
}

Action CmdLs(Conn& c, char* arg) {
    char path[280];
    const char* why = "";
    if (!CheckPath(arg, Access::Read, path, sizeof(path), &why)) {
        Reply(c, "ERR %s", why);
        return Action::None;
    }
    const SceUID dir = sceIoDopen(path);
    if (dir < 0) {
        Reply(c, "ERR cannot list %s: 0x%08X", path, static_cast<unsigned>(dir));
        return Action::None;
    }
    size_t used = 0;
    bool cut = false;
    SceIoDirent entry;
    for (;;) {
        std::memset(&entry, 0, sizeof(entry));
        if (sceIoDread(dir, &entry) <= 0) break;
        if (!std::strcmp(entry.d_name, ".") || !std::strcmp(entry.d_name, "..")) continue;
        if (used + 320 > kIoMemory) {
            cut = true;
            break;
        }
        used += static_cast<size_t>(std::snprintf(g_io + used, 320, "%c %llu %s\n", SCE_S_ISDIR(entry.d_stat.st_mode) ? 'd' : 'f',
                                                  static_cast<unsigned long long>(entry.d_stat.st_size), entry.d_name));
    }
    sceIoDclose(dir);
    if (!Reply(c, "OK %u%s", static_cast<unsigned>(used), cut ? " truncated" : "") || !SendAll(c.s, g_io, static_cast<int>(used))) return Action::Close;
    return Action::None;
}

Action CmdStat(Conn& c, char* arg) {
    char path[280];
    const char* why = "";
    if (!CheckPath(arg, Access::Read, path, sizeof(path), &why)) {
        Reply(c, "ERR %s", why);
        return Action::None;
    }
    LogPause pause(path);
    SceIoStat st;
    if (!Exists(path, &st)) Reply(c, "ERR no such file: %s", path);
    else Reply(c, "OK %c %llu", SCE_S_ISDIR(st.st_mode) ? 'd' : 'f', static_cast<unsigned long long>(st.st_size));
    return Action::None;
}

Action CmdRm(Conn& c, char* arg) {
    char path[280];
    const char* why = "";
    const char* rest = nullptr;
    if (!CheckPath(arg, Access::Write, path, sizeof(path), &why, &rest) || !*rest) {
        Reply(c, "ERR %s", *why ? why : "RM needs a file or an empty folder below one of the two folders");
        return Action::None;
    }
    LogPause pause(path);
    SceIoStat st;
    if (!Exists(path, &st)) {
        Reply(c, "ERR no such file: %s", path);
        return Action::None;
    }
    const int r = SCE_S_ISDIR(st.st_mode) ? sceIoRmdir(path) : sceIoRemove(path);
    if (r < 0) Reply(c, "ERR cannot remove %s: 0x%08X%s", path, static_cast<unsigned>(r), SCE_S_ISDIR(st.st_mode) ? " (folders must be empty)" : "");
    else Reply(c, "OK removed %s", path);
    return Action::None;
}

Action CmdMkdir(Conn& c, char* arg) {
    char path[280];
    const char* why = "";
    const char* rest = nullptr;
    if (!CheckPath(arg, Access::Write, path, sizeof(path), &why, &rest) || !*rest) {
        Reply(c, "ERR %s", *why ? why : "MKDIR needs a folder below one of the two folders");
        return Action::None;
    }
    MakeParents(path, rest);
    const int r = sceIoMkdir(path, 0777);
    SceIoStat st;
    if (Exists(path, &st) && SCE_S_ISDIR(st.st_mode)) Reply(c, "OK %s", path);
    else Reply(c, "ERR cannot create %s: 0x%08X", path, static_cast<unsigned>(r));
    return Action::None;
}

Action CmdMv(Conn& c, char* arg) {
    char* bar = std::strchr(arg, '|');
    if (!bar) {
        Reply(c, "ERR usage: MV <from>|<to>");
        return Action::None;
    }
    *bar = '\0';
    char from[280], to[280];
    const char* why = "";
    const char* restFrom = nullptr;
    const char* restTo = nullptr;
    if (!CheckPath(arg, Access::Write, from, sizeof(from), &why, &restFrom) || !CheckPath(bar + 1, Access::Write, to, sizeof(to), &why, &restTo) ||
        !*restFrom || !*restTo) {
        Reply(c, "ERR %s", *why ? why : "MV needs two names below the two folders");
        return Action::None;
    }
    LogPause pauseFrom(from);
    LogPause pauseTo(to);
    SceIoStat st;
    if (!Exists(from)) {
        Reply(c, "ERR no such file: %s", from);
        return Action::None;
    }
    MakeParents(to, restTo);
    if (Exists(to, &st)) {
        const int r = SCE_S_ISDIR(st.st_mode) ? -1 : sceIoRemove(to);
        if (r < 0) {
            Reply(c, "ERR %s exists and cannot be replaced: 0x%08X", to, static_cast<unsigned>(r));
            return Action::None;
        }
    }
    const int r = sceIoRename(from, to);
    if (r < 0) Reply(c, "ERR rename failed: 0x%08X", static_cast<unsigned>(r));
    else Reply(c, "OK %s -> %s", from, to);
    return Action::None;
}

// UNINSTALL <title id>: the system's own uninstall (the promoter utility, as VitaShell does it)
// of OUR old test titles, on the owner's request (9 Oct): the probes MSCP00001/MSCP00002, the
// text-console build MSCH00001 and the never-finished Codex install CDXMS0002. Nothing else can be
// named: the list is fixed here, and the running title is refused. Saves of these titles go with
// them (the game's own saves live in ux0:data/mscharged/NAND, which is untouched).
Action CmdUninstall(Conn& c, char* arg) {
    static const char* const kAllowed[] = {"MSCP00001", "MSCP00002", "MSCH00001", "CDXMS0002", "MSCN00001", "MSCN00002", "MSCN00003"};
    bool allowed = false;
    for (const char* t : kAllowed) allowed = allowed || !std::strcmp(arg, t);
    if (!allowed || !std::strcmp(arg, g_title)) {
        Reply(c, "ERR UNINSTALL <id>: only MSCP00001 MSCP00002 MSCH00001 CDXMS0002 MSCN00001 MSCN00002 MSCN00003 (never the running title)");
        return Action::None;
    }
    char sfo[64];
    std::snprintf(sfo, sizeof(sfo), "ux0:app/%s/sce_sys/param.sfo", arg);
    if (!Exists(sfo)) {
        Reply(c, "OK %s is not installed (no %s)", arg, sfo);
        return Action::None;
    }
    // PAF first (the promoter utility needs it), with the argument block VitaShell passes.
    unsigned pafArgs[] = {0x180000, 0xffffffff, 0xffffffff, 1, 0xffffffff, 0xffffffff};
    int pafResult = -1;
    SceSysmoduleOpt opt = {sizeof(SceSysmoduleOpt), &pafResult, {-1, -1}};
    int paf = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(pafArgs), pafArgs, &opt);
    int mod = paf >= 0 ? sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL) : -1;
    int init = mod >= 0 ? scePromoterUtilityInit() : -1;
    int del = init >= 0 ? scePromoterUtilityDeletePkg(arg) : -1;
    if (init >= 0) scePromoterUtilityExit();
    if (mod >= 0) sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (paf >= 0) {
        SceSysmoduleOpt unloadOpt{};
        sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, nullptr, &unloadOpt);
    }
    std::fprintf(stderr, "[lab] uninstall %s: paf 0x%08X promoter 0x%08X init 0x%08X delete 0x%08X; still there: %d\n", arg,
                 static_cast<unsigned>(paf), static_cast<unsigned>(mod), static_cast<unsigned>(init), static_cast<unsigned>(del),
                 Exists(sfo) ? 1 : 0);
    if (del < 0) Reply(c, "ERR uninstall %s failed: paf 0x%08X promoter 0x%08X init 0x%08X delete 0x%08X", arg, static_cast<unsigned>(paf),
                       static_cast<unsigned>(mod), static_cast<unsigned>(init), static_cast<unsigned>(del));
    else Reply(c, "OK uninstalled %s (still there: %s)", arg, Exists(sfo) ? "yes" : "no");
    return Action::None;
}

Action HandleCommand(Conn& c, char* line, Pending& pending) {
    char* arg = std::strchr(line, ' ');
    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ') ++arg;
    } else {
        arg = line + std::strlen(line);
    }
    ++g_commands;
    std::snprintf(g_lastCommand, sizeof(g_lastCommand), "%s %s", line, arg);
    const bool quiet = !std::strcmp(line, "PING") || !std::strcmp(line, "STAT");
    if (!quiet) {
        std::fprintf(stderr, "[lab] command: %s %s\n", line, arg);
        ShowScreen();   // the screen says what the server is doing while it does it
    }

    if (!std::strcmp(line, "PING")) {
        QueryAddress();
        Reply(c, "OK msclab proto=2 build=%s variant=%s title=%s ip=%s port=%d self=%s state=idle last=%s", MSC_LAB_BUILD_ID, kMscVitaVariantTag,
              g_title, g_ip[0] ? g_ip : "?", kPort, g_self, g_lastRun);
        return Action::None;
    }
    if (!std::strcmp(line, "PUT")) return CmdPut(c, arg, false);
    if (!std::strcmp(line, "PUTR")) return CmdPut(c, arg, true);
    if (!std::strcmp(line, "GET")) return CmdGet(c, arg);
    if (!std::strcmp(line, "LS")) return CmdLs(c, arg);
    if (!std::strcmp(line, "STAT")) return CmdStat(c, arg);
    if (!std::strcmp(line, "RM")) return CmdRm(c, arg);
    if (!std::strcmp(line, "MKDIR")) return CmdMkdir(c, arg);
    if (!std::strcmp(line, "MV")) return CmdMv(c, arg);
    if (!std::strcmp(line, "UNINSTALL")) return CmdUninstall(c, arg);
    if (!std::strcmp(line, "RUN")) {
        unsigned seconds = kDefaultMaxSeconds;
        if (*arg) seconds = static_cast<unsigned>(std::strtoul(arg, nullptr, 10));
        if (seconds < 5 || seconds > 24u * 3600u) {
            Reply(c, "ERR RUN [max_seconds]: 5 to 86400");
            return Action::None;
        }
        pending.seconds = seconds;
        Reply(c, "OK run max=%u (the server is closed until the run has ended)", seconds);
        return Action::Run;
    }
    if (!std::strcmp(line, "EXEC")) {
        char* option = std::strchr(arg, ' ');
        if (option) {
            *option++ = '\0';
            while (*option == ' ') ++option;
        }
        if (!std::strncmp(arg, "app0:", 5)) arg += 5;
        if (*arg == '/') ++arg;
        char path[120];
        std::snprintf(path, sizeof(path), "%s/%s", g_appDir, arg);
        SceIoStat st;
        if (!PlainFileName(arg) || !Exists(path, &st) || SCE_S_ISDIR(st.st_mode)) {
            Reply(c, "ERR EXEC <file> [RUN [max_seconds]]: a file directly in the application folder (%s)", g_appDir);
            return Action::None;
        }
        // Only something that at least begins like an executable is handed to the system
        // (what it does with anything else is not worth finding out on the owner's console).
        unsigned char magic[4] = {0, 0, 0, 0};
        const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd >= 0) {
            sceIoRead(fd, magic, sizeof(magic));
            sceIoClose(fd);
        }
        if (std::memcmp(magic, "SCE\0", 4) != 0 || st.st_size < 4096) {
            Reply(c, "ERR %s is not an executable (no SELF header)", arg);
            return Action::None;
        }
        pending.execRun = false;
        pending.seconds = kDefaultMaxSeconds;
        if (option && *option) {
            if (std::strncmp(option, "RUN", 3) != 0 || (option[3] != '\0' && option[3] != ' ')) {
                Reply(c, "ERR EXEC <file> [RUN [max_seconds]]");
                return Action::None;
            }
            pending.execRun = true;
            if (option[3]) pending.seconds = static_cast<unsigned>(std::strtoul(option + 4, nullptr, 10));
            if (pending.seconds < 5 || pending.seconds > 24u * 3600u) {
                Reply(c, "ERR RUN max_seconds: 5 to 86400");
                return Action::None;
            }
        }
        std::snprintf(pending.file, sizeof(pending.file), "%s", arg);
        Reply(c, "OK exec app0:%s (%llu bytes)%s", arg, static_cast<unsigned long long>(st.st_size), pending.execRun ? ", run at once" : "");
        return Action::Exec;
    }
    if (!std::strcmp(line, "PLAY")) {
        Reply(c, "OK leaving lab mode: the game starts as for a player (no watchdog, no reload)");
        return Action::Play;
    }
    if (!std::strcmp(line, "EXIT")) {
        Reply(c, "OK exiting to the home screen");
        return Action::Exit;
    }
    if (!std::strcmp(line, "REBOOT")) {
        // A cold reset ends everything that is not installed permanently: on a console whose
        // custom firmware is loaded after each power-on, the owner has to redo that by hand.
        if (std::strcmp(arg, "YES") != 0) {
            Reply(c, "ERR REBOOT YES   (cold reset of the console; nothing of ours restarts by itself afterwards)");
            return Action::None;
        }
        Reply(c, "OK cold reset requested");
        return Action::Reboot;
    }
    Reply(c, "ERR unknown command %s (PING PUT PUTR GET LS STAT RM MKDIR MV UNINSTALL RUN EXEC PLAY EXIT REBOOT)", line);
    return Action::None;
}

// ---- run supervision -------------------------------------------------------------------------

void ReadEnvSeconds(const char* name, unsigned* value) {
    const char* text = std::getenv(name);
    if (text && *text) {
        const unsigned long v = std::strtoul(text, nullptr, 10);
        if (v >= 2 && v <= 24ul * 3600ul) *value = static_cast<unsigned>(v);
    }
}

int WatchdogThread(SceSize, void*) {
    uint64_t lastFrames = VitaPlatform::FrameCount();
    uint64_t lastChangeUs = NowUs();
    uint64_t lastPassUs = lastChangeUs;
    for (unsigned pass = 1;;) {
        if (g_wdPollMs != 0) {
            sceKernelDelayThread(g_wdPollMs * 1000);
        } else {
            SceUInt timeout = 1000 * 1000;
            sceKernelWaitSema(g_wdSema, 1, &timeout);
        }
        if (g_ending.load()) {
            // Another thread is ending the run; nothing left to watch.
            for (;;) sceKernelDelayThread(1000 * 1000);
        }
        if (g_crashPending.load()) {
            // The crash handler has written its report and waits for this thread (a clean
            // context) to do the reload.
            EndRun(1);
        }
        const uint64_t now = NowUs();
        if (g_wdPollMs != 0 && now - lastPassUs < 1000000u) continue;   // what follows: once a second
        lastPassUs = now;
        ++pass;
        const uint64_t frames = VitaPlatform::FrameCount();
        if (frames != lastFrames) {
            lastFrames = frames;
            lastChangeUs = now;
        }
        if (pass % 10 == 0) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        if (now - g_runStartUs >= static_cast<uint64_t>(g_maxSeconds) * 1000000u) {
            EndReason("time limit: the run was still going after %u s (frame %llu)", g_maxSeconds, static_cast<unsigned long long>(frames));
            // A run that still presents frames ends where a script's exit ends it: on the game
            // thread, between two frames, every other thread in an orderly state. (Vita3K
            // never finishes closing the application when the reload is asked for from this
            // thread in the middle of a frame; a console's kernel should not care, but the
            // orderly way costs nothing.) Fifteen seconds for the game thread to get there
            // (a loading screen can sit on one frame for several seconds).
            g_endRequest.store(124);
            for (int i = 0; i < 150 && !g_ending.load(); ++i) sceKernelDelayThread(100 * 1000);
            if (g_ending.load())
                for (;;) sceKernelDelayThread(1000 * 1000);
            VitaLog::EmergencyLine("[lab] the game thread did not come to a frame end within 15 s of the time limit; the watchdog thread ends the run\n");
            EndRun(124);
        }
        const unsigned limit = frames == 0 ? g_stallBootSeconds : g_stallSeconds;
        if (now - lastChangeUs >= static_cast<uint64_t>(limit) * 1000000u) {
            if (frames == 0) EndReason("stall: no frame presented in the first %u s", limit);
            else EndReason("stall: no frame presented for %u s after frame %llu", limit, static_cast<unsigned long long>(frames));
            EndRun(125);
        }
    }
    return 0;
}

void BeginRun(unsigned seconds, void (*reloadEnv)(), const char* how) {
    // log.txt from here on, starting with what this process has logged so far.
    const bool switched = VitaLog::SwitchFile(kRunLog, kPrevLog);
    if (!switched) std::fprintf(stderr, "[lab] WARNING: log.txt could not be created; the run's log continues in %s\n", VitaLog::CurrentPath());
    if (reloadEnv) reloadEnv();
    g_maxSeconds = seconds;
    ReadEnvSeconds("MSC_LAB_STALL_S", &g_stallSeconds);
    ReadEnvSeconds("MSC_LAB_STALL_BOOT_S", &g_stallBootSeconds);
    int core = 2;
    if (const char* text = std::getenv("MSC_LAB_WD_CORE"))
        if (*text >= '0' && *text <= '2') core = *text - '0';
    // Two switches for finding out whether the watchdog thread disturbs a run:
    // MSC_LAB_WD=0 runs without it (no time limit, no stall detection; a crash is still
    // followed by a reload, from the exception handler after 5 s; a run that hangs stays hung),
    // MSC_LAB_WD_POLL_MS=<n> makes it sleep n ms per pass instead of waiting on a semaphore.
    bool wanted = true;
    if (const char* text = std::getenv("MSC_LAB_WD")) wanted = *text != '0';
    if (const char* text = std::getenv("MSC_LAB_WD_POLL_MS")) g_wdPollMs = static_cast<unsigned>(std::strtoul(text, nullptr, 10)) % 1001u;
    VitaConsole::SetFatalWaitMs(0);
    for (int row = 1; row <= 5; ++row) VitaConsole::SetLine(row, 0xC8C8C8, "");
    g_runStartUs = NowUs();
    WriteState("running since=%lld max=%u build=%s self=%s how=%s", static_cast<long long>(std::time(nullptr)), seconds, MSC_LAB_BUILD_ID, g_self, how);
    int started = -1;
    if (wanted) {
        g_wdSema = sceKernelCreateSema("msc_lab_wd", 0, 0, 1, nullptr);
        g_wdThread = sceKernelCreateThread("msc_lab_watchdog", WatchdogThread, 64, 64 * 1024, 0, 0x10000 << core, nullptr);
        started = g_wdThread >= 0 ? sceKernelStartThread(g_wdThread, 0, nullptr) : g_wdThread;
    }
    g_runActive.store(true);
    std::fprintf(stderr, "[lab] RUN (%s): time limit %u s, stall limits %u s / %u s before the first frame; watchdog thread %s (priority 64, core %d, 0x%X%s)\n",
                 how, seconds, g_stallSeconds, g_stallBootSeconds, !wanted ? "SWITCHED OFF (MSC_LAB_WD=0)" : started >= 0 ? "started" : "NOT STARTED", core,
                 static_cast<unsigned>(started), g_wdPollMs ? ", polling" : "");
}

} // namespace

// ---- public ----------------------------------------------------------------------------------

const char* EarlyLogName() {
    if (g_decided == 0) g_decided = Exists(kNoLabFile) ? -1 : 1;
    return g_decided > 0 ? "lab.txt" : VitaPlatform::kLogFileName;
}

bool RunActive() { return g_runActive.load(std::memory_order_relaxed); }

void RealExit(int code) {
    // The log is written in batches after the first frame: whatever the last second said (the harness's "run ended" lines, a final
    // report) is still in memory here, and sceKernelExitProcess does not run atexit handlers. (Found in the first Vita3K run of
    // the full game: log.txt ended at the last [watch] line, the exit reason was lost.)
    VitaLog::Flush();
    __real_sceKernelExitProcess(code);
    for (;;) sceKernelDelayThread(1000 * 1000);
}

void EndReason(const char* format, ...) {
    if (g_haveReason.exchange(true)) return;
    va_list args;
    va_start(args, format);
    std::vsnprintf(g_reason, sizeof(g_reason), format, args);
    va_end(args);
    for (char* p = g_reason; *p; ++p)
        if (*p == '\n' || *p == '\r') *p = ' ';
}

void EndRun(int code) {
    if (!g_runActive.load()) RealExit(code);
    if (g_ending.exchange(true)) {
        // Two threads end the run at once (say the watchdog and the game thread's own exit):
        // the first one does it. If it has not replaced the process in 20 s, leave plainly.
        for (int i = 0; i < 200; ++i) sceKernelDelayThread(100 * 1000);
        RealExit(code);
    }
    if (!g_haveReason.load()) EndReason("exit code %d", code);
    const unsigned elapsedMs = static_cast<unsigned>((NowUs() - g_runStartUs) / 1000u);
    const unsigned long long frames = VitaPlatform::FrameCount();
    static char line[420];
    std::snprintf(line, sizeof(line), "[lab] RUN ENDED: %s (exit code %d); %u.%u s, %llu frames presented; reloading %s\n", g_reason, code,
                  elapsedMs / 1000u, (elapsedMs % 1000u) / 100u, frames, kMonitor);
    VitaLog::EmergencyLine(line);
    WriteState("ended: %s; code=%d seconds=%u frames=%llu at=%lld build=%s self=%s", g_reason, code, elapsedMs / 1000u, frames,
               static_cast<long long>(std::time(nullptr)), MSC_LAB_BUILD_ID, g_self);
    sceIoRemove(kExecFile);
    const int r = sceAppMgrLoadExec(kMonitor, nullptr, nullptr);
    // A successful call does not return on a console; an emulator may return 0 and replace
    // the process a moment later. Give it ten seconds either way.
    if (r >= 0)
        for (int i = 0; i < 100; ++i) sceKernelDelayThread(100 * 1000);
    std::snprintf(line, sizeof(line), "[lab] RELOAD FAILED: sceAppMgrLoadExec(\"%s\") returned 0x%08X and the process is still here; exiting to the home screen\n",
                  kMonitor, static_cast<unsigned>(r));
    VitaLog::EmergencyLine(line);
    WriteState("ended: %s; code=%d seconds=%u frames=%llu at=%lld build=%s self=%s RELOAD-FAILED=0x%08X", g_reason, code, elapsedMs / 1000u, frames,
               static_cast<long long>(std::time(nullptr)), MSC_LAB_BUILD_ID, g_self, static_cast<unsigned>(r));
    RealExit(code);
}

void CrashEnd() {
    if (!g_runActive.load()) return;
    g_crashPending.store(true);
    if (g_wdSema >= 0) sceKernelSignalSema(g_wdSema, 1);
    // The watchdog thread reloads from a clean context. Should that not happen (the thread
    // is gone, or the system does not schedule it while this handler runs), try from here.
    for (int i = 0; i < 50; ++i) sceKernelDelayThread(100 * 1000);
    VitaLog::EmergencyLine("[lab] the watchdog thread did not take the reload within 5 s; trying from the exception handler\n");
    g_ending.store(false);
    EndRun(1);
}

void OnFramePresented() {
    if (!g_runActive.load(std::memory_order_relaxed)) return;
    // The watchdog's time limit, taken here (see WatchdogThread).
    const int request = g_endRequest.load(std::memory_order_relaxed);
    if (request >= 0) {
        if (NpHarnessFinish) NpHarnessFinish(request, "time limit of the lab run");
        EndRun(request);
    }
    // Deliberate failures, for testing the lab itself: MSC_LAB_CRASH_AT=<frame> and
    // MSC_LAB_CRASH_KIND=null (default: read through a null pointer) | prot (read a page made
    // inaccessible through kubridge: the one fault Vita3K delivers to our handler) | undef |
    // hang (busy loop) | sleep (blocks for ever) | exit | abort.
    static const long crashAt = [] {
        const char* text = std::getenv("MSC_LAB_CRASH_AT");
        return text && *text ? std::strtol(text, nullptr, 10) : -1L;
    }();
    if (crashAt < 0 || static_cast<long>(VitaPlatform::FrameCount()) != crashAt) return;
    const char* kind = std::getenv("MSC_LAB_CRASH_KIND");
    if (!kind || !*kind) kind = "null";
    std::fprintf(stderr, "[lab] MSC_LAB_CRASH_AT=%ld reached: deliberate failure of kind '%s'\n", crashAt, kind);
    std::fflush(stderr);
    VitaLog::Flush();
    if (!std::strcmp(kind, "prot")) {
        const SceUID block = sceKernelAllocMemBlock("msc_lab_crash", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4096, nullptr);
        void* base = nullptr;
        sceKernelGetMemBlockBase(block, &base);
        const int r = kuKernelMemProtect(base, 4096, KU_KERNEL_PROT_NONE);
        std::fprintf(stderr, "[lab] page %p protected (0x%X); reading it now\n", base, static_cast<unsigned>(r));
        VitaLog::Flush();
        volatile int* p = static_cast<volatile int*>(base);
        const int value = *p;
        std::fprintf(stderr, "[lab] the read of the protected page did NOT fault (value %d); the run continues\n", value);
    } else if (!std::strcmp(kind, "undef")) {
        __builtin_trap();
    } else if (!std::strcmp(kind, "hang")) {
        for (volatile unsigned spin = 0;; ++spin) {}
    } else if (!std::strcmp(kind, "sleep")) {
        for (;;) sceKernelDelayThread(1000 * 1000);
    } else if (!std::strcmp(kind, "exit")) {
        std::exit(7);
    } else if (!std::strcmp(kind, "abort")) {
        std::abort();
    } else {
        volatile int* volatile p = nullptr;
        const int value = *p;
        std::fprintf(stderr, "[lab] the read through a null pointer did NOT fault (value %d; an emulator only logs it); the run continues\n", value);
    }
}

void ServeBeforeRun(void (*reloadEnv)()) {
    EarlyLogName();
    if (g_decided < 0) {
        std::fprintf(stderr, "[lab] this is a lab build, but %s exists: starting as an ordinary build\n", kNoLabFile);
        return;
    }
    // Where we are: the title id names the application folder.
    if (sceAppMgrAppParamGetString(0, 12, g_title, sizeof(g_title)) < 0 || std::strlen(g_title) != 9)
        std::snprintf(g_title, sizeof(g_title), "%s", kMscVitaTitleId);
    std::snprintf(g_appDir, sizeof(g_appDir), "ux0:app/%s", g_title);
    sceIoMkdir(kLabDir, 0777);

    // How the previous run ended.
    char text[240];
    if (ReadWholeFile(kStateFile, text, sizeof(text)) > 0) {
        if (!std::strncmp(text, "running", 7)) std::snprintf(g_lastRun, sizeof(g_lastRun), "DIED WITHOUT REPORT (state file still says: %.150s)", text);
        else std::snprintf(g_lastRun, sizeof(g_lastRun), "%s", text);
    }
    // Started by another executable's EXEC? "<file> <unix time> <serve|run> <seconds>". Valid for
    // 45 s only: an executable that is not a lab build does not consume the file, and if the
    // owner starts the application by hand afterwards the note is not meant for this process.
    bool autoRun = false;
    unsigned autoSeconds = kDefaultMaxSeconds;
    if (ReadWholeFile(kExecFile, text, sizeof(text)) > 0) {
        sceIoRemove(kExecFile);
        char file[64] = "", mode[16] = "";
        long long when = 0;
        unsigned seconds = kDefaultMaxSeconds;
        const long long now = static_cast<long long>(std::time(nullptr));
        if (std::sscanf(text, "%63s %lld %15s %u", file, &when, mode, &seconds) >= 3 && now - when >= -5 && now - when <= 45) {
            std::snprintf(g_self, sizeof(g_self), "%s", file);
            autoRun = !std::strcmp(mode, "run");
            autoSeconds = seconds;
        } else {
            std::fprintf(stderr, "[lab] stale or unreadable %s ignored: %s\n", kExecFile, text);
        }
    }
    std::fprintf(stderr, "[lab] lab mode: build %s (%s), title %s, started from %s; previous run: %s\n", MSC_LAB_BUILD_ID, kMscVitaVariantTag, g_title,
                 g_self, g_lastRun);
    if (autoRun) {
        BeginRun(autoSeconds, nullptr, "new process, no server phase");
        return;
    }

    g_ioBlock = sceKernelAllocMemBlock("msc_lab_io", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, kIoMemory, nullptr);
    void* base = nullptr;
    if (g_ioBlock < 0 || sceKernelGetMemBlockBase(g_ioBlock, &base) < 0) {
        std::fprintf(stderr, "[lab] no memory for the server (0x%08X): starting as an ordinary build\n", static_cast<unsigned>(g_ioBlock));
        g_decided = -1;
        return;
    }
    g_io = static_cast<char*>(base);
    VitaPlatform::BootStage("LAB MODE: command server (START = play normally)");

    Pending pending;
    Action action = Action::None;
    uint64_t lastTryUs = 0, lastTickUs = 0;
    bool announced = false;
    char shownIp[24] = "?";
    // The network can go away under a waiting server: Wi-Fi lost, or the console suspended
    // (power button, lid of a dock) and woken again, after which the old sockets are dead
    // while everything still looks in order. Three signs, each of which makes the server
    // tear the network down and set it up again as soon as there is a connection:
    //   - the connection state is not "connected";
    //   - the wall clock ran ahead of this process's own clock (which stands still while the
    //     console sleeps) by more than five seconds between two ticks;
    //   - accept() fails with anything but "nothing there yet", 20 times in a row.
    bool restartWanted = false;
    int acceptErrors = 0;
    long long lastWall = static_cast<long long>(std::time(nullptr));
    unsigned restarts = 0;
    while (action == Action::None) {
        const uint64_t now = NowUs();
        if (g_listen < 0 && (lastTryUs == 0 || now - lastTryUs > 2000000u)) {
            lastTryUs = now;
            if (!NetUp() && !announced) std::fprintf(stderr, "[lab] network not up yet (%s); retrying every 2 s\n", g_netError);
        }
        if (now - lastTickUs > 1000000u) {
            const long long wall = static_cast<long long>(std::time(nullptr));
            const long long ownSeconds = lastTickUs ? static_cast<long long>((now - lastTickUs) / 1000000u) : 0;
            if (lastTickUs && wall - lastWall > ownSeconds + 5) {
                std::fprintf(stderr, "[lab] the wall clock moved %lld s in %lld s of this process: the console slept; the server's sockets are renewed\n",
                             wall - lastWall, ownSeconds);
                restartWanted = true;
            }
            lastWall = wall;
            lastTickUs = now;
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
            if (g_listen >= 0) {
                int state = SCE_NETCTL_STATE_CONNECTED;
                const bool connected = sceNetCtlInetGetState(&state) < 0 || state == SCE_NETCTL_STATE_CONNECTED;
                if (!connected && !restartWanted) {
                    std::fprintf(stderr, "[lab] the network connection is gone (state %d); the server's sockets are renewed when it is back\n", state);
                    restartWanted = true;
                }
                if (acceptErrors >= 20) restartWanted = true;
                if (restartWanted && connected) {
                    NetDown();
                    const bool up = NetUp();
                    ++restarts;
                    std::fprintf(stderr, "[lab] network set up again (%u. time): %s\n", restarts, up ? "listening" : g_netError);
                    restartWanted = false;
                    acceptErrors = 0;
                    lastTryUs = now;
                }
            }
            if (g_listen >= 0) QueryAddress();
            if (std::strcmp(shownIp, g_ip) != 0) {
                std::snprintf(shownIp, sizeof(shownIp), "%s", g_ip);
                if (g_listen >= 0) {
                    std::fprintf(stderr, "[lab] command server listening: address %s, port %d\n", g_ip[0] ? g_ip : "(none yet)", kPort);
                    announced = true;
                }
            }
            ShowScreen();
        }
        if (StartPressed()) {
            std::fprintf(stderr, "[lab] START pressed on the waiting screen\n");
            action = Action::Play;
            break;
        }
        if (g_listen < 0) {
            sceKernelDelayThread(50 * 1000);
            continue;
        }
        SceNetSockaddrIn peer;
        unsigned peerLength = sizeof(peer);
        std::memset(&peer, 0, sizeof(peer));
        Conn c;
        c.s = sceNetAccept(g_listen, reinterpret_cast<SceNetSockaddr*>(&peer), &peerLength);
        if (c.s < 0) {
            if (IsTimeout(c.s)) acceptErrors = 0;
            else if (++acceptErrors == 20) std::fprintf(stderr, "[lab] accept keeps failing (0x%08X)\n", static_cast<unsigned>(c.s));
            sceKernelDelayThread(50 * 1000);
            continue;
        }
        acceptErrors = 0;
        int off = 0, second = 1000 * 1000;
        sceNetSetsockopt(c.s, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &off, sizeof(off));
        sceNetSetsockopt(c.s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &second, sizeof(second));
        sceNetSetsockopt(c.s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &second, sizeof(second));
        // One client at a time; it may send several commands. Ten idle seconds end the visit.
        static char line[512];
        while (action == Action::None && ReadLine(c, line, sizeof(line), 10)) {
            action = HandleCommand(c, line, pending);
            ShowScreen();
            if (action == Action::Close) {
                action = Action::None;
                break;
            }
        }
        sceNetShutdown(c.s, SCE_NET_SHUT_RDWR);
        sceNetSocketClose(c.s);
    }

    // Leaving the server for good (or trying to): the network goes away completely first.
    NetDown();
    switch (action) {
    case Action::Run:
        sceKernelFreeMemBlock(g_ioBlock);
        g_ioBlock = -1;
        BeginRun(pending.seconds, reloadEnv, "same process");
        return;
    case Action::Exec: {
        char target[80], note[160];
        std::snprintf(target, sizeof(target), "app0:%s", pending.file);
        std::snprintf(note, sizeof(note), "%s %lld %s %u\n", pending.file, static_cast<long long>(std::time(nullptr)), pending.execRun ? "run" : "serve",
                      pending.seconds);
        WriteWholeFile(kExecFile, note);
        std::fprintf(stderr, "[lab] EXEC: sceAppMgrLoadExec(\"%s\")\n", target);
        VitaLog::Flush();
        const int r = sceAppMgrLoadExec(target, nullptr, nullptr);
        if (r >= 0)
            for (int i = 0; i < 100; ++i) sceKernelDelayThread(100 * 1000);
        sceIoRemove(kExecFile);
        if (pending.execRun && !std::strcmp(pending.file, g_self)) {
            // "Run in a fresh process of this executable" is not to be had: run in this one.
            std::fprintf(stderr, "[lab] EXEC FAILED: sceAppMgrLoadExec(\"%s\") returned 0x%08X; the run starts in THIS process instead "
                                 "(switches that static initialisers read keep the previous env.txt's values)\n", target, static_cast<unsigned>(r));
            sceKernelFreeMemBlock(g_ioBlock);
            g_ioBlock = -1;
            BeginRun(pending.seconds, reloadEnv, "same process, because the reload was refused");
            return;
        }
        std::fprintf(stderr, "[lab] EXEC FAILED: sceAppMgrLoadExec(\"%s\") returned 0x%08X; reloading %s instead\n", target, static_cast<unsigned>(r), kMonitor);
        WriteState("ended: EXEC of %s failed (sceAppMgrLoadExec 0x%08X); at=%lld build=%s self=%s", pending.file, static_cast<unsigned>(r),
                   static_cast<long long>(std::time(nullptr)), MSC_LAB_BUILD_ID, g_self);
        VitaLog::Flush();
        // Back into a server the simple way: a fresh start of the monitor. If even that is
        // refused, the home screen.
        sceAppMgrLoadExec(kMonitor, nullptr, nullptr);
        for (int i = 0; i < 100; ++i) sceKernelDelayThread(100 * 1000);
        RealExit(1);
    }
    case Action::Reboot:
        std::fprintf(stderr, "[lab] REBOOT: cold reset\n");
        VitaLog::Flush();
        scePowerRequestColdReset();
        for (int i = 0; i < 100; ++i) sceKernelDelayThread(100 * 1000);
        RealExit(0);
    case Action::Exit:
        std::fprintf(stderr, "[lab] EXIT: back to the home screen\n");
        VitaLog::Flush();
        RealExit(0);
    default:
        break;
    }
    // START / PLAY: an ordinary start from here on. The log moves to log.txt as for a run.
    sceKernelFreeMemBlock(g_ioBlock);
    g_ioBlock = -1;
    VitaLog::SwitchFile(kRunLog, kPrevLog);
    if (reloadEnv) reloadEnv();
    for (int row = 1; row <= 5; ++row) VitaConsole::SetLine(row, 0xC8C8C8, "");
    WriteState("ended: lab mode left on request (START or PLAY): the game was started as for a player; at=%lld build=%s self=%s",
               static_cast<long long>(std::time(nullptr)), MSC_LAB_BUILD_ID, g_self);
    std::fprintf(stderr, "[lab] lab mode left: ordinary start (no watchdog, no reload at the end)\n");
}

} // namespace VitaLab

// Every exit call of the process (the link redirects both symbols here in lab builds).
extern "C" int __wrap_sceKernelExitProcess(int code) {
    if (VitaLab::RunActive()) VitaLab::EndRun(code);
    // An ordinary start (ux0:data/mscharged/nolab: the emulator, or a player starting a lab build) has no run to end: the log is written in batches
    // and sceKernelExitProcess runs no atexit handler, so the harness's last lines ("run ended") would be lost (found in the first NPV3 Vita3K run).
    VitaLog::Flush();
    return __real_sceKernelExitProcess(code);
}
// exit() and a return from main() arrive here after the exit handlers, BEFORE newlib frees
// the C heap (which the log's mutex and much else lives in).
extern "C" void __wrap__exit(int code) {
    if (VitaLab::RunActive()) VitaLab::EndRun(code);
    VitaLog::Flush();
    __real__exit(code);
    for (;;) {}
}

#endif // __vita__ && MSC_LAB
