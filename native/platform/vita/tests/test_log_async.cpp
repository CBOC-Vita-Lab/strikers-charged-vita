// nxr6b: the harness's mid-run [script] flush is asynchronous (VitaLog::FlushAsync / NpVitaLogFlushAsync) and the fatal paths still flush.
// Host: there is no writer thread, so the async flush is the try-lock Flush(): it must put the buffer in the file, and it must NOT wait while another thread is inside the file
// write (the Vita's case: the writer inside sceIoWrite). The blocking flushes (end of run, crash) must still deliver every byte even then. Usage: test_log_async <dir>
#include "np_vita_internal.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

extern "C" void NpVitaLogFlush(void);
extern "C" void NpVitaLogFlushAsync(void);
namespace VitaPlatform { uint64_t FrameCount() { return 0; } }

static std::string Slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string path = std::string(argv[1]) + "/async.log";
    std::remove(path.c_str());
    VitaLog::Open(argv[1], "async.log");
    // 1. the async flush delivers the buffered line (no writer thread here: Flush())
    const char* a = "[script] f=1 first\n";
    VitaLog::Write(a, std::strlen(a));
    assert(Slurp(path).empty());
    NpVitaLogFlushAsync();
    assert(Slurp(path) == a);
    // 2. nothing buffered: false, and no hang
    assert(!VitaLog::FlushAsync());
    // 3. async flush does not wait: another thread sits in a flush of its own for 300 ms (a stand-in for the writer inside sceIoWrite) -- here a thread that takes the sink's
    //    lock through a stream of writes while this one asks; the call must come back at once with the data kept for the next flush.
    VitaLog::Write("held\n", 5);
    std::atomic<bool> stop{false};
    std::thread hog([&] { while (!stop.load()) { VitaLog::Write("x", 1); } });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) VitaLog::FlushAsync();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    stop.store(true);
    hog.join();
    assert(ms < 1500.0);   // 50 calls under contention: bounded (a blocking flush polls with 10 ms sleeps)
    // 4. the fatal paths: the crash handler's Flush() and the end of the run's FlushBlocking() / NpVitaLogFlush() still write every byte
    VitaLog::Write("crash-line\n", 11);
    VitaLog::Flush();
    assert(Slurp(path).find("crash-line\n") != std::string::npos);
    VitaLog::Write("exit-line\n", 10);
    NpVitaLogFlush();
    assert(Slurp(path).find("exit-line\n") != std::string::npos);
    VitaLog::Write("tail-line\n", 10);
    assert(VitaLog::FlushBlocking());
    assert(Slurp(path).find("tail-line\n") != std::string::npos);
    std::printf("test_log_async: ok (%.1f ms for 50 contended async flushes)\n", ms);
    return 0;
}
