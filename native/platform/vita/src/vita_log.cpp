// Log sink of the Vita platform layer.
//
// The runtime logs through std::cerr / fprintf(stderr) and the guest's
// OSReport through stdout. On the desktop a pair of pipes and pump threads
// copies both into a transcript file; the Vita has neither a terminal nor
// pipes worth using, so here the two stdio streams are pointed at Write(),
// which
//   - appends to ux0:data/mscharged/log.txt (buffered: about a quarter of a
//     second of output is held back, and every fatal path flushes), and
//   - keeps the last lines, wrapped to the console width, for the on-screen
//     console.
#include "plat_realclock.h"
#include "np_vita_internal.h"
#include "np_log_stdio.h"
#include "../../../include/np/gsplit.h"   // gt27 (MSC_GSPLIT): the log phase (no native/include on this file's path)
#include <initializer_list>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(__vita__)
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <sys/reent.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace VitaLog {
#if NP_LOGGING
namespace {

// 23 Oct: on the Vita the file is written by a writer thread, not by the thread that logs. A memory-card write takes
// milliseconds (a line written under the old 250 ms flush, from the present path, was a visible stall in logging builds, and
// it changed the very timings the log was there to show). The logging thread copies into the buffer and returns; the writer
// drains it every 250 ms, or at once when 8 KiB are waiting. Fixed buffers, no heap and no condition variable on this path.
// Fatal paths, Flush(), synchronous mode and the lab's file switches still write on the caller's thread, under the I/O lock,
// so the order in the file is the order of the writes.
constexpr size_t kBufferSize = 64 * 1024;
constexpr size_t kWakeAt = 8192;
constexpr int kRingLines = 64;
constexpr unsigned kFlushIntervalMs = 250;
constexpr long kRotateAtBytes = 2 * 1024 * 1024;

std::mutex g_mutex;            // the buffer and the display ring
std::mutex g_io;               // the file (taken after g_mutex, never before it)
char g_bufferA[kBufferSize];
char* g_buffer = g_bufferA;   // the buffer lines are appended to (the writer swaps it with g_out: see WriterThread)
size_t g_buffered = 0;
int g_file = -1;
char g_path[256];              // the file g_file is (lab builds switch and pause it)
long long g_startOffset = 0;   // where this process's output begins in it
bool g_paused = false;
uint64_t g_lastFlushMs = 0;
bool g_synchronous = false;
#if defined(__vita__)
SceUID g_wake = -1;            // semaphore the writer thread waits on (-1: no writer, writes happen inline)
char g_outB[kBufferSize];      // the writer's buffer: swapped with g_buffer under g_mutex, written without it
char* g_out = g_outB;
#endif

// Ring of wrapped display lines.
char g_ring[kRingLines][kTailColumns + 1];
int g_ringNext = 0;            // slot the line being assembled lives in
int g_ringCount = 0;           // completed lines available (<= kRingLines - 1)
int g_column = 0;              // write position in the line being assembled
std::atomic<uint32_t> g_generation{0};

uint64_t NowMs() {
#if defined(__vita__)
    return sceKernelGetProcessTimeWide() / 1000u;
#else
    return PlatRealMonotonicUs() / 1000u;   // not the harness's virtual clock: plat_realclock.h
#endif
}

void WaitMs(unsigned ms) {
#if defined(__vita__)
    sceKernelDelayThread(ms * 1000u);
#else
    usleep(ms * 1000u);
#endif
}

void FileWrite(const char* data, size_t size) {
    if (g_file < 0 || size == 0) return;
#if defined(__vita__)
    sceIoWrite(g_file, data, static_cast<SceSize>(size));
#else
    ssize_t ignored = ::write(g_file, data, size);
    (void)ignored;
#endif
}

// Writes the buffer now, on this thread (g_mutex held).
void FlushLocked() {
    if (g_paused) return;   // (the file is closed for a moment; the buffer keeps what it can)
    if (g_buffered != 0) {
        std::lock_guard<std::mutex> io(g_io);
        FileWrite(g_buffer, g_buffered);
        g_buffered = 0;
    }
    g_lastFlushMs = NowMs();
}

#if defined(__vita__)
int WriterThread(SceSize, void*) {
    for (;;) {
        SceUInt timeout = kFlushIntervalMs * 1000u;
        sceKernelWaitSema(g_wake, 1, &timeout);
        std::unique_lock<std::mutex> lock(g_mutex);
        if (g_paused || g_file < 0 || g_buffered == 0) continue;
        std::lock_guard<std::mutex> io(g_io);   // before g_mutex is released: no Flush() can overtake this chunk
        const size_t n = g_buffered;
        // smooth (11 Nov): the two buffers trade places instead of a 64 KiB copy under g_mutex. The writer runs at the lowest priority; preempted inside the copy it kept every
        // thread that logs (the game thread, the render thread) waiting on g_mutex for as long as the busiest thread of its core ran. The swap holds the lock for a few instructions.
        char* const full = g_buffer;
        g_buffer = g_out;
        g_out = full;
        g_buffered = 0;
        g_lastFlushMs = NowMs();
        lock.unlock();
        FileWrite(full, n);
    }
    return 0;
}

void StartWriter() {
    if (g_wake >= 0) return;
    const SceUID sema = sceKernelCreateSema("np_log_wake", 0, 0, 1, nullptr);
    if (sema < 0) return;
    // The lowest user priority (191) on any core: it only ever waits for the card.
    const SceUID thread = sceKernelCreateThread("np_log_writer", WriterThread, 0x10000100 + 31, 16 * 1024, 0, 0, nullptr);
    if (thread < 0 || sceKernelStartThread(thread, 0, nullptr) < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        sceKernelDeleteSema(sema);
        return;
    }
    g_wake = sema;
}

void WakeWriter() {
    if (g_wake >= 0) sceKernelSignalSema(g_wake, 1);
}
#endif

void EndLineLocked() {
    g_ring[g_ringNext][g_column] = '\0';
    g_ringNext = (g_ringNext + 1) % kRingLines;
    if (g_ringCount < kRingLines - 1) ++g_ringCount;
    g_column = 0;
    g_ring[g_ringNext][0] = '\0';
    g_generation.fetch_add(1, std::memory_order_relaxed);
}

// Feeds the display ring: printable ASCII only, tabs to spaces, long lines
// wrapped with a two-space continuation indent.
void RingAppendLocked(const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        char c = data[i];
        if (c == '\n') { EndLineLocked(); continue; }
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) c = '?';
        if (g_column >= kTailColumns) {
            EndLineLocked();
            g_ring[g_ringNext][0] = ' ';
            g_ring[g_ringNext][1] = ' ';
            g_column = 2;
        }
        g_ring[g_ringNext][g_column++] = c;
    }
}

} // namespace

void Open(const char* devicePath, const char* fileName) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file >= 0) return;
    char path[256], old[256];
    std::snprintf(path, sizeof(path), "%s/%s", devicePath, fileName);
    std::snprintf(old, sizeof(old), "%s/%s.old", devicePath, fileName);
#if defined(__vita__)
    SceIoStat st;
    if (sceIoGetstat(path, &st) >= 0 && st.st_size > kRotateAtBytes) {
        sceIoRemove(old);
        sceIoRename(path, old);
    }
    g_file = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    std::snprintf(g_path, sizeof(g_path), "%s", path);
    g_startOffset = (sceIoGetstat(path, &st) >= 0) ? static_cast<long long>(st.st_size) : 0;
    if (g_file >= 0) StartWriter();
#else
    struct stat st;
    if (::stat(path, &st) == 0 && st.st_size > kRotateAtBytes) {
        ::unlink(old);
        ::rename(path, old);
    }
    g_file = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
#endif
    g_lastFlushMs = NowMs();
}

#if defined(__vita__)
// (wave 14, from the Galaxy port's fable31fix S1, log_sink.h DisplayDropRule) The display-queue callback thread (4 KiB system stack) must never do a card write: a full buffer on
// that thread used to make Write() flush to the memory card from there (milliseconds, and a deep call chain on a stack with ~1.3 KiB of margin). The renderer's DisplayCallback
// names its thread once (NpLogMarkNoCardWrite); on that thread a full buffer DROPS the line and counts it, and the next line of any other thread reports the count once.
// Every other thread is unchanged. MSC_LOG_DISPLAY_DROP=0 gives back the old behaviour (read once).
std::atomic<int> g_noCardTid{0};
unsigned g_displayDropped = 0;   // under g_mutex
inline bool OnNoCardThread() {
    const int tid = g_noCardTid.load(std::memory_order_relaxed);
    return tid != 0 && tid == static_cast<int>(sceKernelGetThreadId());
}
#else
inline bool OnNoCardThread() { return false; }
unsigned g_displayDropped = 0;
#endif

void Write(const char* data, size_t size) {
    if (!data || size == 0) return;
    const bool noCard = OnNoCardThread();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (noCard && g_file >= 0 && !g_paused && (size >= kBufferSize || g_buffered + size > kBufferSize)) {   // (full: this thread must not write the card)
        ++g_displayDropped;
        return;
    }
    if (g_displayDropped != 0 && !noCard && !g_paused && g_file >= 0) {   // (once, by an ordinary thread)
        char note[96];
        const int n = std::snprintf(note, sizeof note, "[log] %u display-thread line(s) dropped (buffer full)\n", g_displayDropped);
        if (n > 0 && g_buffered + static_cast<size_t>(n) <= kBufferSize) {
            std::memcpy(g_buffer + g_buffered, note, static_cast<size_t>(n));
            g_buffered += static_cast<size_t>(n);
            g_displayDropped = 0;
        }
    }
    RingAppendLocked(data, size);
    if (g_paused) {
        // Pause(): keep what fits; the lab server, the only user, logs nothing meanwhile.
        if (g_buffered + size <= kBufferSize) {
            std::memcpy(g_buffer + g_buffered, data, size);
            g_buffered += size;
        }
        return;
    }
    if (g_file >= 0) {
        if (size >= kBufferSize) {
            FlushLocked();
            std::lock_guard<std::mutex> io(g_io);
            FileWrite(data, size);
        } else {
            if (g_buffered + size > kBufferSize) FlushLocked();   // (the writer is behind: write here rather than drop)
            std::memcpy(g_buffer + g_buffered, data, size);
            g_buffered += size;
        }
#if defined(__vita__)
        if (g_wake >= 0 && !g_synchronous) {
            if (g_buffered >= kWakeAt) WakeWriter();
            return;
        }
#endif
        if (!noCard && (g_synchronous || NowMs() - g_lastFlushMs >= kFlushIntervalMs)) FlushLocked();
    }
}

#if defined(__vita__)
extern "C" void NpLogMarkNoCardWrite(void) {
    static const bool on = [] { const char* e = std::getenv("MSC_LOG_DISPLAY_DROP"); return !(e && e[0] == '0'); }();
    if (on) g_noCardTid.store(static_cast<int>(sceKernelGetThreadId()), std::memory_order_relaxed);
}
#endif

void SetSynchronous(bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_synchronous = on;
    if (on) FlushLocked();
}

void Flush() noexcept {
    // Called from fatal paths too: if the lock is held by this very thread
    // (a crash inside Write), give up rather than deadlock.
    if (!g_mutex.try_lock()) return;
    if (!g_paused && g_buffered != 0 && g_io.try_lock()) {   // (a crash during the writer's own write: give up)
        FileWrite(g_buffer, g_buffered);
        g_buffered = 0;
        g_io.unlock();
    }
    g_mutex.unlock();
}

// The end of a run that leaves through sceKernelExitProcess / _Exit (no atexit handlers, the writer thread is simply killed): everything buffered goes to the file now.
// Unlike Flush() this waits for the locks (the writer thread may be inside its own write, which Flush() would take for "a crash in the writer" and skip), for at most
// 2 s, and then writes without them: a lost line is worse than a torn one.
bool FlushBlocking() noexcept {
    bool lockedMutex = false, lockedIo = false;
    for (int i = 0; i < 200 && !lockedMutex; ++i) {
        lockedMutex = g_mutex.try_lock();
        if (!lockedMutex) WaitMs(10);
    }
    for (int i = 0; i < 200 && !lockedIo; ++i) {
        lockedIo = g_io.try_lock();
        if (!lockedIo) WaitMs(10);
    }
    bool wrote = false;
    if (!g_paused && g_buffered != 0) {
        FileWrite(g_buffer, g_buffered);
        g_buffered = 0;
        wrote = true;
    }
    if (lockedIo) g_io.unlock();
    if (lockedMutex) g_mutex.unlock();
    return wrote;
}

bool FlushAsync() noexcept {
#if defined(__vita__)
    if (g_wake >= 0 && !g_synchronous) {
        bool pending;
        { std::lock_guard<std::mutex> lock(g_mutex); pending = g_buffered != 0 && !g_paused; }   // (g_mutex is held by the writer only for the buffer swap, a few instructions)
        if (pending) WakeWriter();
        return pending;
    }
#endif
    const bool had = g_buffered != 0;   // (a racy peek: only for the return value)
    Flush();
    return had;
}

void FlushIfDue() {
#if defined(__vita__)
    if (g_wake >= 0) return;   // the writer thread does it
#endif
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_buffered != 0 && NowMs() - g_lastFlushMs >= kFlushIntervalMs) FlushLocked();
}

int Tail(char lines[][kTailColumns + 1], int maxLines, uint32_t* generation) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (generation) *generation = g_generation.load(std::memory_order_relaxed);
    // The line still being assembled counts as the newest one if it has text.
    const bool partial = g_column > 0;
    int available = g_ringCount + (partial ? 1 : 0);
    int count = available < maxLines ? available : maxLines;
    int slot = g_ringNext - (partial ? 0 : 1) - (count - 1);
    slot = ((slot % kRingLines) + kRingLines) % kRingLines;
    for (int i = 0; i < count; ++i) {
        const bool isPartial = partial && i == count - 1;
        const int length = isPartial ? g_column : static_cast<int>(std::strlen(g_ring[slot]));
        std::memcpy(lines[i], g_ring[slot], static_cast<size_t>(length));
        lines[i][length] = '\0';
        slot = (slot + 1) % kRingLines;
    }
    return count;
}

#if defined(__vita__)
// ---- lab builds (vita_lab.cpp) --------------------------------------------
// The process starts its log in one file (the command server's) and, when a run
// begins, moves to log.txt, taking along what it has written so far: log.txt is
// then the complete log of exactly one run, and the process that comes back
// after the run does not touch it.
bool SwitchFile(const char* newPath, const char* keepOldAs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_paused = false;
    FlushLocked();
    std::lock_guard<std::mutex> io(g_io);
    if (g_file >= 0) sceIoClose(g_file);
    g_file = -1;
    if (keepOldAs) {
        sceIoRemove(keepOldAs);
        sceIoRename(newPath, keepOldAs);
    }
    const SceUID out = sceIoOpen(newPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out < 0) {
        // Keep logging where we were rather than nowhere.
        g_file = sceIoOpen(g_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
        return false;
    }
    const SceUID in = sceIoOpen(g_path, SCE_O_RDONLY, 0);
    if (in >= 0) {
        static char copy[4096];
        if (sceIoLseek(in, g_startOffset, SCE_SEEK_SET) >= 0) {
            for (;;) {
                const int n = sceIoRead(in, copy, sizeof(copy));
                if (n <= 0) break;
                sceIoWrite(out, copy, static_cast<SceSize>(n));
            }
        }
        sceIoClose(in);
    }
    g_file = out;
    std::snprintf(g_path, sizeof(g_path), "%s", newPath);
    g_startOffset = 0;
    g_lastFlushMs = NowMs();
    return true;
}

const char* CurrentPath() { return g_path; }

// Closes the file so that it can be read or replaced (flushes first); Resume() appends again.
void Pause() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_paused) return;
    FlushLocked();
    std::lock_guard<std::mutex> io(g_io);
    if (g_file >= 0) sceIoClose(g_file);
    g_file = -1;
    g_paused = true;
}
void Resume() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_paused) return;
    {
        std::lock_guard<std::mutex> io(g_io);
        g_file = sceIoOpen(g_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    }
    g_paused = false;
    FlushLocked();
}

// One line straight into the file from a thread that must not wait for the log's lock for
// ever (the lab watchdog ending a run while the game thread may be stuck anywhere, even in
// Write): half a second of trying, then the write happens without the lock.
void EmergencyLine(const char* text) noexcept {
    bool locked = false;
    for (int i = 0; i < 50 && !locked; ++i) {
        locked = g_mutex.try_lock();
        if (!locked) sceKernelDelayThread(10 * 1000);
    }
    if (g_file >= 0) {
        if (g_buffered != 0) {
            sceIoWrite(g_file, g_buffer, static_cast<SceSize>(g_buffered));
            g_buffered = 0;
        }
        sceIoWrite(g_file, text, static_cast<SceSize>(std::strlen(text)));
    }
    if (locked) {
        RingAppendLocked(text, std::strlen(text));
        g_mutex.unlock();
    }
}
#endif
#else
void Open(const char*, const char*) {}
void Write(const char*, size_t) {}
void SetSynchronous(bool) {}
void Flush() noexcept {}
bool FlushBlocking() noexcept { return false; }
bool FlushAsync() noexcept { return false; }
void FlushIfDue() {}
int Tail(char[][kTailColumns + 1], int, uint32_t* generation) {
    if (generation) *generation = 0;
    return 0;
}
bool SwitchFile(const char*, const char*) { return false; }
const char* CurrentPath() { return ""; }
void Pause() {}
void Resume() {}
void EmergencyLine(const char*) noexcept {}
#endif

#if defined(__vita__)
namespace {
std::atomic<unsigned> g_hookCalls{0};
std::atomic<unsigned long long> g_hookBytes{0};
bool g_lockOn = true;   // MSC_STDIO_LOCK (ApplyStdioMode): the wrapped printf family goes through np_log_stdio.h, not through the FILE
// newlib's FILE keeps its write function in the object; stdout and stderr are
// the process-wide __sf[1] and __sf[2], shared by every thread and by
// libstdc++'s cout/cerr (which write through these FILE objects). Replacing
// the function captures all of it without pipes or a pump thread.
_READ_WRITE_RETURN_TYPE HookedWrite(struct _reent*, void*, const char* data, _READ_WRITE_BUFSIZE_TYPE size) {
    g_hookCalls.fetch_add(1, std::memory_order_relaxed);
    g_hookBytes.fetch_add(static_cast<unsigned long long>(size), std::memory_order_relaxed);
    NpGsSeqW npGsLog(NPGS_LOG);   // gt27 (MSC_GSPLIT): the game thread's share of logging (the formatting before this call stays in the caller's phase)
    if (npGsLog.On()) { NpGsCountIf(NPGC_LOG_LINES, 1); NpGsCountIf(NPGC_LOG_BYTES, (unsigned)size); }
    Write(data, static_cast<size_t>(size));
    if (NP_LOGGING) VitaConsole::RefreshThrottled(200);
    return static_cast<_READ_WRITE_RETURN_TYPE>(size);
}

// crash26b: the sink and the after-call of np_log_stdio.h (the wrapped printf family of logging builds, vita_silent_stdio.cpp).
void StdioSink(const char* data, size_t size) { Write(data, size); }   // (under the format lock: Write prints nothing)
void StdioAfter() { if (NP_LOGGING) VitaConsole::RefreshThrottled(200); }
} // namespace

// crash26 / crash26b (18 Nov): the SDK's newlib is built WITHOUT _RETARGETABLE_LOCKING, so a FILE has no lock; two threads printing to the shared
// line-buffered stdout/stderr at once lost updates of _p / _w and __sfvwrite_r copied (size_t)-1 bytes over the heap (both console dumps). crash26's
// unbuffered streams stalled the console (each printf put a 1 KiB buffer on the caller's stack; the display-queue thread that prints has 4 KiB).
// crash26b: the streams stay line-buffered as in 1.2 (the same stack depth), and every wrapped printf / fputs / fwrite / puts / putc / fputc / putchar
// (vita.mk --wrap in logging builds) on stdout / stderr bypasses the FILE: np_log_stdio.h formats under its own bounded lock into one buffer and hands
// the text to Write(). Only calls the link cannot wrap (none found in the image) still reach the FILE and this hook.
void InstallStdioHooks() {
    NpLogStdio::Configure(StdioSink, StdioAfter);
    for (FILE* stream : {stdout, stderr}) {
        std::fflush(stream);                           // also forces newlib's lazy stream setup
        std::setvbuf(stream, nullptr, _IOLBF, 1024);   // one hook call per line
        stream->_write = HookedWrite;
    }
}

void ApplyStdioMode() {
    const char* e = std::getenv("MSC_STDIO_LOCK");
    g_lockOn = !(e && std::strcmp(e, "0") == 0);
    std::fprintf(stderr, "[stdio] %s (MSC_STDIO_LOCK=%d)\n",
                 g_lockOn ? "log printf / fputs / fwrite / putc on stdout and stderr bypass the FILE: formatted under one bounded lock, whole texts to the log"
                          : "log printing through newlib's FILE (1.2): two threads printing at once can corrupt the heap (crash26)",
                 g_lockOn ? 1 : 0);
}

bool StdioLockOn() { return g_lockOn; }

void StdioReport(const char* when) {
    const NpLogStdio::Stats& s = NpLogStdio::G().stats;
    std::fprintf(stderr, "[stdio] %s: lock %s; wrapped %llu calls %llu bytes, format-lock waits %u timeouts %u nested %u long %u errors %u; through the FILE %u writes %llu bytes\n",
                 when ? when : "now", g_lockOn ? "on" : "off (MSC_STDIO_LOCK=0)", s.calls.load(), s.bytes.load(), s.waits.load(), s.timeouts.load(), s.nested.load(),
                 s.longTexts.load(), s.errors.load(), g_hookCalls.load(std::memory_order_relaxed), g_hookBytes.load(std::memory_order_relaxed));
}
#else
// The check build keeps the desktop transcript (main.cpp's pipes); the sink
// and the ring are exercised through VitaPlatform::SelfTest and BootStage.
void InstallStdioHooks() {}
void ApplyStdioMode() {}
void StdioReport(const char*) {}
bool StdioLockOn() { return false; }
#endif

// ---- the basic record (np_vita_internal.h: Event) ------------------------------------------------------------------------------
namespace {
constexpr int kEventMaxLines = 64;
constexpr long kEventRotateAtBytes = 96 * 1024;
std::atomic<int> g_eventLines{0};
std::atomic_flag g_eventBusy = ATOMIC_FLAG_INIT;
bool g_eventHeader = false;
}

namespace {
std::atomic<int> g_noteLines{0};
constexpr int kNoteMaxLines = 24;   // (their own count: a flood of notes must not use up the lines the fatal paths have)
void EventLine(const char* label, const char* text) noexcept {
#if defined(__vita__)
    const unsigned long ms = static_cast<unsigned long>(sceKernelGetProcessTimeWide() / 1000u);
#else
    const unsigned long ms = static_cast<unsigned long>(PlatRealMonotonicUs() / 1000u);
#endif
    char line[700];
    std::snprintf(line, sizeof(line), "[%s +%lu ms, frame %llu] %s\n", label, ms, static_cast<unsigned long long>(VitaPlatform::FrameCount()), text);
#if NP_LOGGING
    // A logging build has its log open: the same sink as everything else, flushed now (the process is about to end).
    std::fputs(line, stderr);
    std::fflush(stderr);
    Flush();
#elif defined(__vita__)
    // A quiet build has no log open and no stdio sink: one open / append / close on the calling thread. The spin is bounded (a thread
    // that crashed inside this function must not stop the next one from writing).
    for (int i = 0; i < 20000 && g_eventBusy.test_and_set(std::memory_order_acquire); ++i) {}
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", VitaPlatform::kDataDevicePath, VitaPlatform::kLogFileName);
    if (!g_eventHeader) {
        g_eventHeader = true;
        SceIoStat st;
        if (sceIoGetstat(path, &st) >= 0 && st.st_size > kEventRotateAtBytes) {
            char old[100];
            std::snprintf(old, sizeof(old), "%s.old", path);
            sceIoRemove(old);
            sceIoRename(path, old);
        }
    }
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, line, static_cast<SceSize>(std::strlen(line)));
        sceIoClose(fd);
    }
    g_eventBusy.clear(std::memory_order_release);
#else
    std::fputs(line, stderr);
#endif
}
} // namespace

void Event(const char* text) noexcept {
    if (!text || !*text) return;
    if (g_eventLines.fetch_add(1, std::memory_order_relaxed) >= kEventMaxLines) return;
    EventLine("fatal", text);
}

void EventUncapped(const char* text) noexcept {
    if (!text || !*text) return;
    EventLine("fatal", text);
}

// 1.2: a failure that was survived (a render target or texture the driver had no memory for, a shader program that could not be made, a timer ring that
// overflowed): one short line for the basic record. At most 24 per run, written like Event's lines but labelled "note".
void Note(const char* text) noexcept {
    if (!text || !*text) return;
    if (g_noteLines.fetch_add(1, std::memory_order_relaxed) >= kNoteMaxLines) return;
    EventLine("note", text);
}

// 1.2: the memory line (see np_vita_internal.h). Its own count: it must not use up the lines of the fatal paths or the notes.
namespace {
std::atomic<int> g_memLines{0};
constexpr int kMemMaxLines = 240;
}
void Memory(const char* text) noexcept {
    if (!text || !*text) return;
    if (g_memLines.fetch_add(1, std::memory_order_relaxed) >= kMemMaxLines) return;
    EventLine("mem", text);
}

void EventF(const char* format, ...) noexcept {
    char text[600];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Event(text);
}

} // namespace VitaLog

// For the harness (np_harness.cpp, weak reference): its end of a run and its [script] lines. Everything the stdio streams still hold goes into the sink first.
// crash26: with MSC_STDIO_LOCK on only stdout and stderr are flushed. fflush(nullptr) walks every FILE of the process, and newlib's flush of a
// read stream seeks and resets its _p / _r without a lock: a [script] line during loading raced the dvd-io thread's fread on the same FILE (the same lost
// updates, a negative _r there). The streams the harness writes its traces to are flushed by the harness at the end of the run.
// nxr6b: the harness's mid-run [script] lines. The stdio streams go into the sink (a memcpy under the sink's lock), the sink's buffer is handed to the writer thread: no wait for
// the card. The end of a run (HarnessExit) and every fatal path keep the blocking flush above.
extern "C" void NpVitaLogFlushAsync(void) {
    if (VitaLog::StdioLockOn()) {
        std::fflush(stdout);
        std::fflush(stderr);
    } else {
        std::fflush(nullptr);
    }
    VitaLog::FlushAsync();
}

extern "C" void NpVitaLogFlush(void) {
    if (VitaLog::StdioLockOn()) {
        std::fflush(stdout);
        std::fflush(stderr);
    } else {
        std::fflush(nullptr);
    }
    VitaLog::FlushBlocking();
}

// exit17: one line straight into the file, past the sink's buffer and its 250 ms writer (the harness's "[exit] step N" marks and its watchdog's "EXIT STUCK" line: they exist for the case
// that the process stops moving, and a line that waits in the buffer would be lost with it). On the Vita EmergencyLine (a bounded wait for the lock, then the write without it);
// elsewhere the sink and a blocking flush.
extern "C" void NpVitaLogDirect(const char* text) {
    if (!text || !*text) return;
#if defined(__vita__)
    VitaLog::EmergencyLine(text);
#else
    VitaLog::Write(text, std::strlen(text));
    VitaLog::FlushBlocking();
#endif
}
