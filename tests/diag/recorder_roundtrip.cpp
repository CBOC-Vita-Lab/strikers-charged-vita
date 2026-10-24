// Host round trip of the performance recorder (native/platform/compat/np_touchdiag.cpp) through
// tools/diag/perf-report.py: records a short Mega Strike bank with known frame timings, closes it at a
// safe point (written to MSC_DIAG_DIR) and leaves the decoding to tests/test_diag_tools.py.
#include "np/touch_diag.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <chrono>

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main() {
    if (!NpDiagEnabled()) { std::fprintf(stderr, "set MSC_TOUCH_DIAG=1\n"); return 2; }
    NpDiagContext(2, 5, 2, 0, 1);                 // Mega Strike in play: opens bank 2
    if (!NpDiagActive()) { std::fprintf(stderr, "bank did not open\n"); return 1; }
    for (int f = 0; f < 20; ++f) {
        NpDiagStep();
        const uint64_t tag = NpDiagLatch();       // game thread hands the frame over
        const uint64_t t0 = NpDiagNow();
        NpDiagEvent(NPD_BACKEND, 0, tag, f, 40, 2, 0, 0);
        sleep_ms(2);
        NpDiagEvent(NPD_BACKEND_END, 0, tag, f, 0, 0, 0, 0);
        NpDiagEvent(NPD_GPU, 3, tag, t0, NpDiagNow() + 5000000ull, 0, 0, 1);   // display queue call, ~5+ ms
        sleep_ms(8);
        NpDiagEvent(NPD_GATE, 2, tag, 0, f, 0, 0, 0);                           // shown
        NpDiagTake();
    }
    NpDiagContext(0, 1, 4, 1, 0);                 // paused at a menu: safe point, the bank is written
    std::printf("recorded\n");
    return 0;
}
