// 1.3 MSC_VGX_WORKLOAD_PROBE / MSC_VGX_FE_SPLIT (vgx_workload.h): the [lane] line from fake clocks, on the threads the game has.
//   workload <thread|same|off> <log file>
//   -DNP_LOGGING=0 build: everything compiles to nothing, and no line appears.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "vgx_workload.h"
static thread_local double fakeCpuMs = 0;
extern "C" double NpFpsPlatformThreadClockMs(void) { return fakeCpuMs; }
static double coreCpu[4], coreTime[4];
extern "C" int NpFpsPlatformProbe(void* out) {
    auto* p = static_cast<double*>(out);
    std::memcpy(p, coreCpu, sizeof coreCpu);
    std::memcpy(p + 4, coreTime, sizeof coreTime);
    *reinterpret_cast<int*>(p + 8) = 0;
    return 1;
}
static uint32_t gpuSeq = 0;
extern "C" uint32_t vglRtLastGpuFrame(void* out) {
    const uint32_t f[4] = {20000, 0, 0, 0x00010001u};
    std::memcpy(out, f, sizeof f);
    return ++gpuSeq;
}
namespace vgx { bool LogEnabled() { return false; } }

namespace wl = vgx::workload;
static unsigned failures = 0;
static void Require(bool ok, const char* m) {
    if (!ok) { std::fprintf(stdout, "FAIL: %s\n", m); ++failures; }
}
static void Has(const std::string& s, const char* what) {
    if (s.find(what) == std::string::npos) { std::fprintf(stdout, "FAIL: no \"%s\" in: %s\n", what, s.c_str()); ++failures; }
}

// Two windows of 300 pictures with the given costs. `sameThread`: the back end runs inside the front end's thread.
static void Run(bool sameThread) {
    constexpr double fePerPicture = 10.0, bePerPicture = 6.0;
    for (uint32_t i = 0; i < 2 * wl::kWindow + 1; ++i) {
        wl::ProducerTick();
        fakeCpuMs += fePerPicture;
        coreCpu[0] += 12.0; coreTime[0] += 2.0; coreCpu[1] += 4.0;
        { wl::FeScope scope(wl::FeEmit); }
        wl::MergeNote(wl::MergeBlocks);
        wl::Picture(500, 9000, 200);
        const auto be = [&] { wl::BackendScope s; fakeCpuMs += bePerPicture; };
        if (sameThread) be();
        else std::thread(be).join();
        wl::NextPicture();
    }
}

int main(int argc, char** argv) {
    // The formatter alone.
    {
        wl::Line l;
        l.pictures = 300; l.seconds = 6.0; l.fe = 12.4; l.game = 14.25; l.send = 3.1; l.beCpu = 7.3; l.beWall = 9.8; l.gpu = 18.2;
        l.core[0] = 71; l.core[1] = 63; l.core[2] = 58; l.core[3] = 4; l.drawCalls = 612; l.softVerts = 9120; l.staticDraws = 210; l.threads = 3;
        char b[256];
        wl::FormatLine(l, b, sizeof b);
        Require(!std::strcmp(b, "[lane] 300 pics 6.00s 50.0/s game 14.2 send 3.1 fe 12.4 be 7.3/9.8 gpu 18.2 cores 71/63/58/4 dc 612 soft 9120 static 210 th 3"), b);
        wl::Line none;
        wl::FormatLine(none, b, sizeof b);
        Has(b, "game - send - fe - be - gpu - cores - dc 0");
        char tinyBuf[16]; // (cut, never past the buffer)
        Require(wl::FormatLine(l, tinyBuf, sizeof tinyBuf) == sizeof tinyBuf - 1, "a short buffer cuts the line");
        Require(wl::FeOnly(13.0, 6.0, true) == 7.0 && wl::FeOnly(13.0, 6.0, false) == 13.0 && wl::FeOnly(5.0, 6.0, true) == 0.0 && wl::FeOnly(5.0, -1, true) == 5.0, "FeOnly");
    }
    const char* path = argc > 2 ? argv[2] : "workload.log";
    if (!std::freopen(path, "w", stderr)) return 2;
    if (argc > 1 && !std::strcmp(argv[1], "off")) {
        setenv("MSC_VGX_WORKLOAD_PROBE", "0", 1);
    } else {
        setenv("MSC_VGX_WORKLOAD_PROBE", "1", 1);
        setenv("MSC_VGX_FE_SPLIT", "1", 1);
    }
    const bool off = argc > 1 && !std::strcmp(argv[1], "off"), same = argc > 1 && !std::strcmp(argv[1], "same");
    (void)off; (void)same;
    Run(same);
    std::fflush(stderr);
    FILE* f = std::fopen(path, "r");
    std::string all;
    char line[1024];
    while (f && std::fgets(line, sizeof line, f)) all += line;
    if (f) std::fclose(f);
#if !VGX_WORKLOAD_BUILD
    Require(all.empty(), "NP_LOGGING=0: no line at all");
#else
    if (off) {
        Require(all.empty(), "probe off: no line at all");
    } else {
        const size_t second = all.find("[lane] ", all.find("[lane] ") + 1);
        Require(second != std::string::npos, "two windows, two lines");
        const std::string two = all.substr(second);
        Has(two, " fe 10.0 ");       // alone on its thread, or 16 ms of a thread with 6 of them the back end's
        Has(two, " be 6.0/");
        Has(two, same ? " th 2" : " th 3");
        Has(two, " dc 500 soft 9000 static 200");
        Has(two, " gpu 20.0");
        Has(two, " game 10.0 ");     // 12 ms of the core, 2 of them time-driven threads
        Has(two, " send ");
        Has(all, "[lane-fe]");
        Has(all, " emit=");
        Has(all, "[lane-dl]");
        Has(all, "uniform-blocks=1.0");
    }
#endif
    std::printf("workload %s: %u failures\n", argc > 1 ? argv[1] : "thread", failures);
    return failures != 0;
}
