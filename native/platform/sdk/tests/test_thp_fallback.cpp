// Seed completed lanes directly so fallback precedence is independent of thread timing or disc assets.
#include "../src/thp_native.cpp"

#include <vector>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void Segment(std::vector<uint8_t>& out, uint8_t marker, const std::vector<uint8_t>& payload) {
    const unsigned size = payload.size() + 2;
    out.insert(out.end(), {0xff, marker, uint8_t(size >> 8), uint8_t(size)});
    out.insert(out.end(), payload.begin(), payload.end());
}

static std::vector<uint8_t> ConstantFrame() {
    std::vector<uint8_t> frame = {0xff, 0xd8};
    std::vector<uint8_t> quant(65, 1);
    quant[0] = 0;
    Segment(frame, 0xdb, quant);
    Segment(frame, 0xc0, {8, 0, 16, 0, 16, 3, 1, 0x22, 0, 2, 0x11, 0, 3, 0x11, 0});
    // One-bit DC-zero and AC-end codes: all six blocks produce constant 128 planes.
    std::vector<uint8_t> huffman;
    for (uint8_t table : {0x00, 0x10}) {
        huffman.push_back(table);
        huffman.push_back(1);
        huffman.insert(huffman.end(), 15, 0);
        huffman.push_back(0);
    }
    Segment(frame, 0xc4, huffman);
    Segment(frame, 0xda, {3, 1, 0, 2, 0, 3, 0, 0, 63, 0});
    frame.insert(frame.end(), {0, 0x0f, 0xff, 0xd9});
    return frame;
}

static void CheckPrecedence(unsigned size, bool refused, bool firstFix, bool expectCurrent) {
    std::vector<uint8_t> frame = ConstantFrame();
    frame.resize(size, 0);
    std::vector<uint8_t> expected[3] = {std::vector<uint8_t>(256), std::vector<uint8_t>(64), std::vector<uint8_t>(64)};
    CHECK(THPVideoDecode(frame.data(), expected[0].data(), expected[1].data(), expected[2].data(), nullptr) == 0);
    CHECK(expected[0] == std::vector<uint8_t>(256, 128));

    g_lanes = Lanes();
    g_lanes.enabled = g_lanes.nowait = 1;
    g_lanes.firstFix = firstFix;
    g_lanes.depth = 1;
    g_lanes.lastShown = 0;
    Slot& earlier = g_lanes.slot[0];
    earlier.state = kDone;
    earlier.frame = 1;
    earlier.gen = g_lanes.gen;
    earlier.w = earlier.h = 16;
    std::vector<uint8_t> old[3] = {std::vector<uint8_t>(256, 34), std::vector<uint8_t>(64, 34), std::vector<uint8_t>(64, 34)};
    std::vector<uint8_t> actual[3] = {std::vector<uint8_t>(256, 99), std::vector<uint8_t>(64, 99), std::vector<uint8_t>(64, 99)};
    for (int p = 0; p < 3; ++p) {
        earlier.plane[p] = old[p].data();
        earlier.planeBytes[p] = old[p].size();
    }
    if (refused) NoteRejected(frame.data(), 2);
    CHECK(NpThpVideoDecode(frame.data(), actual[0].data(), actual[1].data(), actual[2].data(), nullptr, 2, 16, 16, size) == 0);
    for (int p = 0; p < 3; ++p) CHECK(actual[p] == (expectCurrent ? expected[p] : old[p]));
    CHECK(g_lanes.lastShown == (expectCurrent ? 2 : 1));
    CHECK(g_lanes.st.misses == unsigned(expectCurrent));
    CHECK(g_lanes.st.lateShown == unsigned(!expectCurrent));
    CHECK(SlotState(earlier) == kIdle);
}

static void CheckExplicitBoundary(unsigned size, bool olderLane, bool enabled, bool nowait, bool firstFix) {
    std::vector<uint8_t> frame = ConstantFrame();
    frame.resize(512, 0);
    std::vector<uint8_t> legacy[3] = {std::vector<uint8_t>(256), std::vector<uint8_t>(64), std::vector<uint8_t>(64)};
    CHECK(THPVideoDecode(frame.data(), legacy[0].data(), legacy[1].data(), legacy[2].data(), nullptr) == 0);
    CHECK(legacy[0] == std::vector<uint8_t>(256, 128));

    g_lanes = Lanes();
    g_lanes.enabled = enabled;
    g_lanes.nowait = nowait;
    g_lanes.firstFix = firstFix;
    g_lanes.depth = 1;
    g_lanes.lastShown = 0;
    std::vector<uint8_t> old[3] = {std::vector<uint8_t>(256, 34), std::vector<uint8_t>(64, 34), std::vector<uint8_t>(64, 34)};
    std::vector<uint8_t> actual[3] = {std::vector<uint8_t>(256, 99), std::vector<uint8_t>(64, 99), std::vector<uint8_t>(64, 99)};
    if (olderLane) {
        Slot& earlier = g_lanes.slot[0];
        earlier.state = kDone;
        earlier.frame = 1;
        earlier.gen = g_lanes.gen;
        earlier.w = earlier.h = 16;
        for (int p = 0; p < 3; ++p) {
            earlier.plane[p] = old[p].data();
            earlier.planeBytes[p] = old[p].size();
        }
        if (size) NoteRejected(frame.data(), 2);
    }
    // Valid bytes remain in memory: only the explicit component boundary may govern this call.
    CHECK(NpThpVideoDecode(frame.data(), actual[0].data(), actual[1].data(), actual[2].data(), nullptr, 2, 16, 16, size) == np_thp::kBadSyntax);
    for (int p = 0; p < 3; ++p) CHECK(actual[p] == std::vector<uint8_t>(actual[p].size(), 99));
    CHECK(g_lanes.lastShown == 0);
    CHECK(g_lanes.st.errors == 1);
    if (!size) {
        CHECK(g_lanes.st.misses == 0);
        CHECK(g_lanes.st.waits == 0);
        if (olderLane) CHECK(SlotState(g_lanes.slot[0]) == kDone);
    }
}

static void CheckInvalidArgumentPrecedence() {
    uint8_t byte = 0;
    g_thp_init = 0;
    CHECK(NpThpVideoDecode(nullptr, &byte, &byte, &byte, nullptr, 2, 16, 16, 0) == 1);
    CHECK(NpThpVideoDecode(&byte, nullptr, &byte, &byte, nullptr, 2, 16, 16, 0) == 2);
    CHECK(NpThpVideoDecode(&byte, &byte, &byte, &byte, nullptr, 2, 16, 16, 0) == 7);
    THPInit();
}

static void CheckAcceptedPrefetchClearsOnlyItsRejection() {
    // This is last: the real worker and its semaphores stay alive until process exit.
    g_lanes = Lanes();
    g_lanes.enabled = g_lanes.nowait = g_lanes.firstFix = 1;
    g_lanes.depth = 1;
    g_lanes.lastShown = 0;
    std::vector<uint8_t> frame = ConstantFrame();
    frame.resize(512, 0);
    std::vector<uint8_t> actual[3] = {std::vector<uint8_t>(256, 99), std::vector<uint8_t>(64, 99), std::vector<uint8_t>(64, 99)};

    // A transient earlier failure must not override this successful retry's finished picture.
    NoteRejected(frame.data(), 2);
    NpThpPrefetch(frame.data(), frame.size(), 2, 16, 16);
    Slot& lane = g_lanes.slot[0];
    CHECK(lane.thread != nullptr);
    if (!lane.thread) return;
    SlotWait(lane);
    CHECK(SlotState(lane) == kDone);
    CHECK(NpThpVideoDecode(frame.data(), actual[0].data(), actual[1].data(), actual[2].data(), nullptr, 2, 16, 16, frame.size()) == 0);
    for (int p = 0; p < 3; ++p) CHECK(actual[p] == std::vector<uint8_t>(actual[p].size(), 128));
    CHECK(g_lanes.st.hits == 1);
    CHECK(g_lanes.st.misses == 0);

    // Accepting frame 4 must leave frame 3's rejection effective: no matching lane exists for 3.
    NoteRejected(frame.data(), 3);
    NpThpPrefetch(frame.data(), frame.size(), 4, 16, 16);
    SlotWait(lane);
    CHECK(SlotState(lane) == kDone);
    for (int p = 0; p < 3; ++p) actual[p].assign(actual[p].size(), 99);
    CHECK(NpThpVideoDecode(frame.data(), actual[0].data(), actual[1].data(), actual[2].data(), nullptr, 3, 16, 16, frame.size()) == 0);
    for (int p = 0; p < 3; ++p) CHECK(actual[p] == std::vector<uint8_t>(actual[p].size(), 128));
    CHECK(g_lanes.st.misses == 1);
    CHECK(NpThpVideoDecode(frame.data(), actual[0].data(), actual[1].data(), actual[2].data(), nullptr, 4, 16, 16, frame.size()) == 0);
    CHECK(g_lanes.st.hits == 2);

    // An already matching prefetch is accepted too, without queuing a second job.
    NpThpPrefetch(frame.data(), frame.size(), 5, 16, 16);
    NoteRejected(frame.data(), 5);
    NpThpPrefetch(frame.data(), frame.size(), 5, 16, 16);
    SlotWait(lane);
    CHECK(SlotState(lane) == kDone);
    CHECK(g_lanes.st.prefetched == 3);
    for (int p = 0; p < 3; ++p) actual[p].assign(actual[p].size(), 99);
    CHECK(NpThpVideoDecode(frame.data(), actual[0].data(), actual[1].data(), actual[2].data(), nullptr, 5, 16, 16, frame.size()) == 0);
    for (int p = 0; p < 3; ++p) CHECK(actual[p] == std::vector<uint8_t>(actual[p].size(), 128));
    CHECK(g_lanes.st.hits == 3);
    CHECK(g_lanes.st.misses == 1);
}

int main() {
    THPInit();
    CheckPrecedence(kMaxFrameBytes + 1, false, true, true);
    CheckPrecedence(512, true, true, true);
    CheckPrecedence(512, false, true, false);
    CheckPrecedence(kMaxFrameBytes, false, true, false);
    CheckPrecedence(kMaxFrameBytes + 1, false, false, false);
    CheckPrecedence(512, true, false, false);
    CheckInvalidArgumentPrecedence();
    NpThpNoteName("fallback-boundary");
    CheckExplicitBoundary(0, true, true, true, true);
    CheckExplicitBoundary(0, true, true, true, false);
    CheckExplicitBoundary(0, false, true, true, true);
    CheckExplicitBoundary(0, false, true, true, false);
    CheckExplicitBoundary(0, false, true, false, true);
    CheckExplicitBoundary(0, false, false, false, true);
    for (unsigned size : {1u, 31u, 90u}) {
        CheckExplicitBoundary(size, true, true, true, true);
        CheckExplicitBoundary(size, false, true, true, true);
        CheckExplicitBoundary(size, false, false, false, true);
    }
    CheckAcceptedPrefetchClearsOnlyItsRejection();
    printf("test_thp_fallback: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
