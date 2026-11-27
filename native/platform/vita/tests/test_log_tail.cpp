// The end of a run that leaves without exit handlers (sceKernelExitProcess, _Exit): NpVitaLogFlush() must put every buffered byte, and what stdio still holds, into the file,
// also while another thread is inside the file write (Flush() would skip). Usage: test_log_tail <dir>
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
namespace VitaPlatform { uint64_t FrameCount() { return 0; } }

static std::string Slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string path = std::string(argv[1]) + "/tail.log";
    std::remove(path.c_str());
    VitaLog::Open(argv[1], "tail.log");
    // buffered lines (the sink holds them back for a quarter of a second)
    const char* a = "[script] f=1 first\n";
    VitaLog::Write(a, std::strlen(a));
    assert(Slurp(path).empty());          // proves the premise: without a flush nothing is in the file yet
    // the tail of a run: more lines, and the stream's own buffer
    const char* b = "[plat] script exit at frame 1, exiting\n";
    VitaLog::Write(b, std::strlen(b));
    NpVitaLogFlush();
    assert(Slurp(path) == std::string(a) + b);
    // nothing buffered: no write, no hang
    assert(!VitaLog::FlushBlocking());
    // a second call after more output
    VitaLog::Write("late\n", 5);
    assert(VitaLog::FlushBlocking());
    assert(Slurp(path) == std::string(a) + b + "late\n");
    // many threads writing while the end of the run flushes: nothing is lost or duplicated
    std::atomic<bool> go{true};
    std::atomic<int> lines{0};
    std::thread t([&] { while (go) { VitaLog::Write("x\n", 2); ++lines; } });
    for (int i = 0; i < 20; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); NpVitaLogFlush(); }
    go = false; t.join();
    NpVitaLogFlush();
    const std::string all = Slurp(path);
    int xs = 0;
    for (size_t i = 0; i + 1 < all.size(); ++i) if (all[i] == 'x' && all[i + 1] == '\n') ++xs;
    assert(xs == lines.load());
    std::puts("test_log_tail: ok");
    return 0;
}
