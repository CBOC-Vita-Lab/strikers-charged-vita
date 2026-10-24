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
#include <initializer_list>

#include <atomic>
#include <chrono>
#include <cstdio>
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
char g_buffer[kBufferSize];
size_t g_buffered = 0;
int g_file = -1;
char g_path[256];              // the file g_file is (lab builds switch and pause it)
long long g_startOffset = 0;   // where this process's output begins in it
bool g_paused = false;
uint64_t g_lastFlushMs = 0;
bool g_synchronous = false;
#if defined(__vita__)
SceUID g_wake = -1;            // semaphore the writer thread waits on (-1: no writer, writes happen inline)
char g_out[kBufferSize];       // the writer's copy of the buffer
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
        std::memcpy(g_out, g_buffer, n);
        g_buffered = 0;
        g_lastFlushMs = NowMs();
        lock.unlock();
        FileWrite(g_out, n);
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

void Write(const char* data, size_t size) {
    if (!data || size == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
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
        if (g_synchronous || NowMs() - g_lastFlushMs >= kFlushIntervalMs) FlushLocked();
    }
}

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
// newlib's FILE keeps its write function in the object; stdout and stderr are
// the process-wide __sf[1] and __sf[2], shared by every thread and by
// libstdc++'s cout/cerr (which write through these FILE objects). Replacing
// the function captures all of it without pipes or a pump thread.
_READ_WRITE_RETURN_TYPE HookedWrite(struct _reent*, void*, const char* data, _READ_WRITE_BUFSIZE_TYPE size) {
    Write(data, static_cast<size_t>(size));
    if (NP_LOGGING) VitaConsole::RefreshThrottled(200);
    return static_cast<_READ_WRITE_RETURN_TYPE>(size);
}
} // namespace

void InstallStdioHooks() {
    for (FILE* stream : {stdout, stderr}) {
        std::fflush(stream);                           // also forces newlib's lazy stream setup
        std::setvbuf(stream, nullptr, _IOLBF, 1024);   // one hook call per line
        stream->_write = HookedWrite;
    }
}
#else
// The check build keeps the desktop transcript (main.cpp's pipes); the sink
// and the ring are exercised through VitaPlatform::SelfTest and BootStage.
void InstallStdioHooks() {}
#endif

} // namespace VitaLog
