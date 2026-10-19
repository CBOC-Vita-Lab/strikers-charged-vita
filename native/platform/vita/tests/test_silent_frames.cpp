#include "np_vita_internal.h"
#include <cassert>
#include <ctime>

static unsigned bootFrames, radioPumps, diagnostics, clocks;
extern "C" void NpVitaOnFramePresented();
extern "C" void NpPlatformBootMark(const char*);
extern "C" void np_vita_radio_pump() { ++radioPumps; }
extern "C" int __real_clock_gettime(clockid_t, timespec*);
extern "C" int __wrap_clock_gettime(clockid_t id, timespec* time) {
    ++clocks;
    return __real_clock_gettime(id, time);
}
namespace VitaBootScreen {
void Frame(uint64_t frame) { assert(frame == ++bootFrames); }
}
namespace VitaLog {
void FlushIfDue() { ++diagnostics; }
void SetSynchronous(bool) { ++diagnostics; }
}
namespace VitaAudio {
Stats GetStats() { ++diagnostics; return {}; }
void LogReport(const char*) { ++diagnostics; }
}
namespace VitaInput { void LastState(uint16_t*, int*, int*) { ++diagnostics; } }
namespace VitaConsole {
void SetLine(int, uint32_t, const char*, ...) { ++diagnostics; }
void Refresh() { ++diagnostics; }
}
extern "C" void NpJobsLogStats(const char*) { ++diagnostics; }

int main() {
    for (int i = 0; i < 1200; ++i) NpVitaOnFramePresented();
    assert(VitaPlatform::FrameCount() == 1200 && bootFrames == 1200 && radioPumps == 1200);
    assert(diagnostics == 0 && clocks == 0);
}
