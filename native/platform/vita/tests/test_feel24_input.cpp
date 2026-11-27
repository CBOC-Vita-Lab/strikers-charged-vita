// wave 24 (feel24): the left-stick tuning (MSC_STICK_DEAD / MSC_STICK_CURVE, default OFF) and the rear panel's lost-touch pick (np_controls.h PickLostContact, MSC_REAR_LATCH).
//   g++ -std=c++17 -O1 -I../src -I../include -I../../harness/include -I../../compat -I../../../include test_feel24_input.cpp -o /tmp/t24 && /tmp/t24 [stick|rear]
// Run 1 with the environment as it is (defaults: the stick must equal the old mapping bit for bit), run 2 with MSC_STICK_DEAD=12 MSC_STICK_CURVE=1.2 (`stick tuned`).
#include "../src/np_vita_input.cpp"
#include "../../sdk/input/np_controls.h"
#include <cstdio>
#include <cstring>
namespace VitaInput { void TapCarryHold(bool) {} void TapCarryClear() {} }   // (defined in the Vita-only part of the file)
extern "C" {
int NpDiagActive(void) { return 0; }
uint64_t NpDiagPollNext(void) { return 0; }
}
static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

// the mapping before wave 24, as it was
static void Legacy(uint8_t rx, uint8_t ry, int8_t& ox, int8_t& oy) {
    const float x = (float)rx - 128.0f, y = 128.0f - (float)ry, m = std::sqrt(x * x + y * y);
    if (m <= 16.0f) { ox = oy = 0; return; }
    float s = (m - 16.0f) / (102.0f - 16.0f); if (s > 1.0f) s = 1.0f;
    const float k = s * 72.0f / m;
    ox = (int8_t)std::lround(x * k); oy = (int8_t)std::lround(y * k);
}

int main(int argc, char** argv) {
    const bool tuned = argc > 1 && !std::strcmp(argv[1], "tuned");
    // stick: every raw position
    unsigned diff = 0, total = 0; int maxd = 0;
    for (int rx = 0; rx < 256; ++rx) for (int ry = 0; ry < 256; ++ry) {
        NpVitaInput in{}; in.lx = (uint8_t)rx; in.ly = (uint8_t)ry; in.rx = (uint8_t)rx; in.ry = (uint8_t)ry;
        NpPadState pad; NpVitaMapGcPad(&in, &pad);
        int8_t lx, ly; Legacy((uint8_t)rx, (uint8_t)ry, lx, ly);
        ++total;
        if (pad.stickX != lx || pad.stickY != ly) { ++diff; int d = std::abs(pad.stickX - lx); if (d > maxd) maxd = d; }
        // the right stick is never tuned
        CHECK(pad.substickX == lx && pad.substickY == ly);
    }
    if (!tuned) {
        CHECK(diff == 0);
        std::printf("stick defaults: %u of %u raw positions differ from the old mapping (must be 0)\n", diff, total);
    } else {
        std::printf("stick tuned: %u of %u raw positions differ from the old mapping, largest step %d (full 72)\n", diff, total, maxd);
        NpVitaInput in{}; in.lx = 128 + 102; in.ly = 128; NpPadState p; NpVitaMapGcPad(&in, &p);
        CHECK(p.stickX == 72 && p.stickY == 0);            // outer throw: the full 72
        in.lx = 128 + 15; NpVitaMapGcPad(&in, &p); CHECK(p.stickX >= 0);   // (inside the old zone 16; just outside the new 12: a unit or two)
        in.lx = 128 + 20; NpVitaMapGcPad(&in, &p); CHECK(p.stickX > 0);
        in.lx = 128 + 11; NpVitaMapGcPad(&in, &p); CHECK(p.stickX == 0);
        in.lx = 128 + 60; NpVitaMapGcPad(&in, &p); int8_t lx, ly; Legacy(128 + 60, 128, lx, ly); CHECK(p.stickX < lx || tuned);   // the curve is below linear at mid throw
    }
    // rear: a finger that came and went between two reads
    using namespace NpControls;
    auto S = [](uint64_t t, int n, int a = -1, int b = -1) { TouchSample s{}; s.stamp = t; s.count = n; s.ids[0] = a; s.ids[1] = b; return s; };
    { TouchSample v[3] = {S(1000, 0), S(1010, 1, 7), S(1020, 0)}; int prev[1]; CHECK(PickLostContact(v, 3, 990, prev, 0, 250000) == 1); }   // new finger down and up between reads: reported once
    { TouchSample v[3] = {S(1000, 1, 7), S(1010, 1, 7), S(1020, 0)}; int prev[1] = {7}; CHECK(PickLostContact(v, 3, 990, prev, 1, 250000) == -1); }   // the finger the last read reported lifts: not shown again (no delayed tap)
    { TouchSample v[2] = {S(1000, 1, 7), S(1020, 1, 7)}; int prev[1]; CHECK(PickLostContact(v, 2, 990, prev, 0, 250000) == -1); }   // newest has a finger: as it is
    { TouchSample v[3] = {S(1000, 0), S(1010, 1, 7), S(1020, 0)}; int prev[1]; CHECK(PickLostContact(v, 3, 0, prev, 0, 250000) == -1); }   // first read: nothing older
    { TouchSample v[3] = {S(1000, 0), S(1010, 1, 7), S(400000, 0)}; int prev[1]; CHECK(PickLostContact(v, 3, 990, prev, 0, 250000) == -1); }  // a stall: old touches are not taps
    { TouchSample v[3] = {S(900, 1, 5), S(1010, 1, 7), S(1020, 0)}; int prev[1]; CHECK(PickLostContact(v, 3, 990, prev, 0, 250000) == 1); }   // older than the last read: ignored
    { TouchSample v[3] = {S(1020, 0), S(1010, 1, 7), S(1000, 1, 9)}; int prev[1]; CHECK(PickLostContact(v, 3, 990, prev, 0, 250000) == 1); }  // any order; the newest finger wins
    { TouchSample v[3] = {S(1000, 0), S(1010, 3, 1, 2), S(1020, 0)}; v[1].count = 9; int prev[1]; CHECK(PickLostContact(v, 3, 990, prev, 0, 250000) == -1); }  // more than 8 reports: invalid
    // the old read of one sample would have lost it; the pick + RearPad turns it into one tap
    {
        RearPad pad; RearPoint p{7, 700.0f, 300.0f};
        unsigned taps = pad.Update(&p, 1, 1010);   // the picked (older) sample, reported once
        taps |= pad.Update(nullptr, 0, 1043);      // the next read: lifted
        CHECK(taps & 2);                           // a right-zone tap
        RearPad pad2; CHECK(pad2.Update(nullptr, 0, 1043) == 0);   // without the pick: nothing
    }
    std::printf("test_feel24_input: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
