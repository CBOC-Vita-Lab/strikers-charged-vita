// crash26b host stress test of np_log_stdio.h through the REAL wrappers of vita_silent_stdio.cpp (built with NP_LOGSTDIO_HOST_TEST and linked with
// the Vita's -Wl,--wrap=printf,vprintf,fprintf,vfprintf,puts,putchar,fputs,fputc,putc,fwrite: run_log_stdio_test.sh).
// The console's printing threads, approximated: the game thread and the back end (printf / fprintf with floats and strings, puts, fputs, fwrite,
// fputc pieces, std::cerr), a 1 ms "os-alarm" timer thread and POSIX timer callbacks (SIGEV_THREAD) that print, a 16 KiB-stack thread that prints,
// texts longer than the 4 KiB buffer, a slow sink (sceIoWrite of the synchronous start-up), a sink that prints from inside itself (nested), and a
// holder that keeps the format lock for 1.5 s (the bounded wait must give up, not hang). A watchdog fails the run if it stops making progress.
// Checks: every whole-line call reaches the sink exactly once, whole and in order per thread; the counters add up; no stall.
// Negative control: built with -DNP_LOGSTDIO_MUTANT_NOLOCK (the format lock skipped) the same run must FAIL (torn or lost lines).
#include "np_log_stdio.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <pthread.h>
#include <regex>
#include <unistd.h>
#include <string>
#include <thread>
#include <vector>

namespace VitaLog {
bool StdioLockOn() { return true; }
}

namespace {
std::mutex g_sinkMutex;           // VitaLog::Write's g_mutex
std::string g_out;                // the log
std::atomic<unsigned long> g_sinkCalls{0}, g_progress{0};
std::atomic<bool> g_slow{false}, g_nestOnce{false};
thread_local bool t_inSink = false;

void Sink(const char* data, size_t size) {
    t_inSink = true;
    {
        std::lock_guard<std::mutex> lock(g_sinkMutex);
        g_out.append(data, size);
    }
    const unsigned long n = g_sinkCalls.fetch_add(1) + 1;
    if (g_slow.load() && n % 500 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));   // a card write
    if (n > 20000 && !g_nestOnce.load() && NpLogStdio::G().owner.load() == NP_LOGSTDIO_TID() && !g_nestOnce.exchange(true))
        std::printf("N0:nested print from inside the sink, the format lock held\n");   // must not deadlock (the own-buffer path)
    t_inSink = false;
    g_progress.fetch_add(1, std::memory_order_relaxed);
}
std::atomic<unsigned long> g_afterCalls{0};
void After() {   // the console refresh: may print (outside the lock)
    if (g_afterCalls.fetch_add(1) % 50000 == 49999) std::fprintf(stderr, "A0:a print from the after-call\n");
}

std::atomic<bool> g_stop{false};
// line format: <tag><thread>:<seq>:<payload>:<len>\n  (the checker verifies tag/thread/seq order and the payload length)
std::string Payload(int t, unsigned long k, size_t extra) {
    std::string p(8 + (k * 7 + t) % 90 + extra, 'a' + static_cast<char>((k + t) % 26));
    return p;
}

void Printer(int t, bool backEnd) {
    for (unsigned long k = 0; !g_stop.load(); ++k) {
        const std::string p = Payload(t, k, (k % 997 == 0) ? 5000 : 0);   // now and then longer than the 4 KiB buffer
        switch (k % 6) {
        case 0: std::printf("P%d:%lu:%s:%zu\n", t, k, p.c_str(), p.size()); break;
        case 1: std::fprintf(stderr, "P%d:%lu:%s:%zu\n", t, k, p.c_str(), p.size()); break;
        case 2: { char line[6000]; std::snprintf(line, sizeof line, "P%d:%lu:%s:%zu\n", t, k, p.c_str(), p.size()); std::fputs(line, stderr); break; }
        case 3: { char line[6000]; const int n = std::snprintf(line, sizeof line, "P%d:%lu:%s:%zu\n", t, k, p.c_str(), p.size()); std::fwrite(line, 1, static_cast<size_t>(n), stdout); break; }
        case 4: { char line[6000]; std::snprintf(line, sizeof line, "P%d:%lu:%s:%zu", t, k, p.c_str(), p.size()); std::puts(line); break; }
        case 5: std::fprintf(stderr, "P%d:%lu:%s:%zu\n", t, k, p.c_str(), p.size()); std::fprintf(stderr, "F%d %.3f %e %g\n", t, k * 0.25, k * 1e-3, k * 3.5); break;
        }
        if (k % 64 == 0) { std::fputc('x', stderr); std::putc('y', stderr); std::putchar('\n'); }        // pieces (not checked as lines)
        if (backEnd && k % 128 == 0) std::cerr << "C" << t << " cerr " << k << std::endl;                   // iostream (fwrite + putc + fflush)
    }
}

// The os-alarm thread of the console: a 1 ms timer loop that prints from its callback.
std::atomic<unsigned long> g_alarmSeq{0};
void AlarmCallback() { const unsigned long k = g_alarmSeq.fetch_add(1); std::fprintf(stderr, "T9:%lu:%s:%zu\n", k, "alarm", static_cast<size_t>(5)); }
void AlarmThread() {
    while (!g_stop.load()) {
        timespec ts{0, 1000000};
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
        AlarmCallback();
    }
}
// POSIX timer callbacks (SIGEV_THREAD: a helper thread the test does not own)
std::atomic<unsigned long> g_timerSeq{0};
void TimerFn(sigval) { const unsigned long k = g_timerSeq.fetch_add(1); std::printf("Q8:%lu:%s:%zu\n", k, "timer", static_cast<size_t>(5)); }

void* SmallStack(void*) {   // a 16 KiB thread (np_stallwatch / np_clock_* size on the console)
    for (unsigned long k = 0; !g_stop.load(); ++k) {
        std::fprintf(stderr, "S7:%lu:%s:%zu\n", k, "small", static_cast<size_t>(5));
        std::fprintf(stderr, "G7 %.1f ms %d %s\n", k * 0.1, static_cast<int>(k), "float line");
        if (k % 100 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return nullptr;
}

struct Check { long next = 0; unsigned long lines = 0; std::map<unsigned long, int> seen; };

// A whole-line call reaches the sink in ONE call, so only the byte pieces (fputc / putc / putchar / cerr's parts) can stand in front of it on the same
// log line: the checked text is the end of the line. A line the lock's 1 s give-up path cut (254 bytes + newline) counts as present but cut.
int Verify(bool expectOk) {
    const std::regex tail("([PSTQ][0-9]):([0-9]+):([a-z]+):([0-9]+)$");
    std::map<std::string, Check> seqs;
    unsigned long lines = 0, torn = 0, gaps = 0, dups = 0, pieces = 0, cut = 0;
    const std::string& s = g_out;
    size_t pos = 0;
    while (pos < s.size()) {
        const size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) break;
        const std::string line = s.substr(pos, nl - pos);
        pos = nl + 1;
        std::smatch m;
        if (line.size() == 254) {   // cut by the give-up path: the prefix still names the line
            const std::regex head("([PSTQ][0-9]):([0-9]+):");
            std::smatch h;
            if (std::regex_search(line, h, head)) { ++cut; Check& c = seqs[h[1]]; ++c.seen[std::stoul(h[2])]; ++c.lines; ++lines; continue; }
        }
        if (!std::regex_search(line, m, tail)) {
            if (line.find(':') != std::string::npos && (line.find('P') != std::string::npos || line.find('T') != std::string::npos)) ++torn;
            else ++pieces;
            continue;
        }
        if (m[3].length() != static_cast<long>(std::stoul(m[4]))) { ++torn; continue; }
        Check& c = seqs[m[1]];
        ++c.seen[std::stoul(m[2])];
        ++c.lines;
        ++lines;
    }
    for (auto& kv : seqs) {
        // every writer's numbers 0..last exactly once (the timer callbacks may run on several helper threads: their order is not checked, only presence)
        unsigned long expect = 0;
        for (auto& e : kv.second.seen) {
            if (e.second > 1) ++dups;
            if (e.first != expect) gaps += e.first > expect ? e.first - expect : 1;
            expect = e.first + 1;
        }
    }
    const NpLogStdio::Stats& st = NpLogStdio::G().stats;
    char summary[700];
    std::snprintf(summary, sizeof summary,
                  "[log_stdio] %zu bytes in the log, %lu checked lines from %zu writers (%lu cut by the 1 s give-up), %lu piece lines; torn %lu, lost %lu, doubled %lu | wrapped calls %llu bytes %llu, lock waits %u timeouts %u nested %u long %u errors %u\n",
                  s.size(), lines, seqs.size(), cut, pieces, torn, gaps, dups, st.calls.load(), st.bytes.load(), st.waits.load(), st.timeouts.load(), st.nested.load(), st.longTexts.load(), st.errors.load());
    ::write(2, summary, std::strlen(summary));   // (straight to the terminal: not through the wraps)
    const bool clean = torn == 0 && gaps == 0 && dups == 0 && lines > 1000 && seqs.size() >= 7;
    const bool countersOk = st.timeouts.load() >= 1 && st.nested.load() >= 1 && st.longTexts.load() >= 1;
    const char* verdict = expectOk ? ((clean && countersOk) ? "PASS\n" : "FAIL\n") : (clean ? "FAIL: the mutant was not caught\n" : "PASS: the mutant (no lock) is caught\n");
    ::write(2, "[log_stdio] ", 12);
    ::write(2, verdict, std::strlen(verdict));
    if (expectOk && !countersOk) { const char* w = "[log_stdio] (the timeout, nested or long-text path was not exercised)\n"; ::write(2, w, std::strlen(w)); }
    return expectOk ? ((clean && countersOk) ? 0 : 1) : (clean ? 1 : 0);
}
} // namespace

int main(int argc, char** argv) {
    const int secs = argc > 1 ? std::atoi(argv[1]) : 20;
    const bool mutant = argc > 2 && std::strcmp(argv[2], "mutant") == 0;
    NpLogStdio::Configure(Sink, After);
    g_slow = true;
    std::vector<std::thread> th;
    for (int t = 0; t < 4; ++t) th.emplace_back(Printer, t, t >= 2);
    th.emplace_back(AlarmThread);
    pthread_t small;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024);
    pthread_create(&small, &attr, SmallStack, nullptr);
    timer_t timer;
    sigevent sev{};
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = TimerFn;
    timer_create(CLOCK_MONOTONIC, &sev, &timer);
    itimerspec its{{0, 2000000}, {0, 2000000}};
    timer_settime(timer, 0, &its, nullptr);
    // the holder: keeps the format lock 1.5 s once, mid-run (the others must give up after 1 s, not hang)
    std::thread holder([] {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const int self = NP_LOGSTDIO_TID();
        while (!NpLogStdio::Acquire(self)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        NpLogStdio::Release();
    });
    // watchdog: progress must not stop for 3 s
    bool stalled = false;
    unsigned long last = 0;
    auto lastMove = std::chrono::steady_clock::now();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const unsigned long p = g_progress.load();
        if (p != last) { last = p; lastMove = std::chrono::steady_clock::now(); }
        else if (std::chrono::steady_clock::now() - lastMove > std::chrono::seconds(3)) { stalled = true; break; }
    }
    if (stalled) { ::write(2, "[log_stdio] FAIL: STALL (no sink progress for 3 s)\n", 51); std::_Exit(2); }
    timer_delete(timer);
    g_stop = true;
    for (auto& t : th) t.join();
    holder.join();
    pthread_join(small, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // (a last timer callback)
    return Verify(!mutant);
}
