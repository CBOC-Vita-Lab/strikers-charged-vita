// The defaults and the values of the np/overlap.h switches.
#include "np/overlap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main(int, char** argv)
{
    const char* mode = argv[1];
    if (!std::strcmp(mode, "host"))
    {
        CHECK(NpFsSnapOnce() == 0 && NpPoseCullLevel() == 0 && NpCrowdPoseOnce() == 0 && NpNpcLazy() == 0);
        CHECK(NpCrowdRefresh(0) == 1 && NpCrowdRefresh(1) == 1);
    }
    else if (!std::strcmp(mode, "vita"))
    {
        CHECK(NpFsSnapOnce() == 1 && NpPoseCullLevel() == 1 && NpCrowdPoseOnce() == 1 && NpNpcLazy() == 1);
        CHECK(NpCrowdRefresh(0) == 2 && NpCrowdRefresh(1) == 4);
    }
    else if (!std::strcmp(mode, "pose2"))
    {
        CHECK(NpPoseCullLevel() == 2);
        CHECK(NpCrowdRefresh(0) == 3 && NpCrowdRefresh(1) == 4);
    }
    else if (!std::strcmp(mode, "MSC_FS_SNAPONCE=0")) CHECK(NpFsSnapOnce() == 0 && NpPoseCullLevel() == 1);
    else if (!std::strcmp(mode, "MSC_POSE_CULL=0")) CHECK(NpPoseCullLevel() == 0 && NpFsSnapOnce() == 1);
    else if (!std::strcmp(mode, "MSC_CROWD_POSEONCE=0")) CHECK(NpCrowdPoseOnce() == 0);
    else if (!std::strcmp(mode, "MSC_CROWD_REFRESH=1")) CHECK(NpCrowdRefresh(0) == 1 && NpCrowdRefresh(1) == 1);
    else if (!std::strcmp(mode, "MSC_NPC_LAZY=0")) CHECK(NpNpcLazy() == 0);
    else CHECK(!"unknown mode");
    // the counters and the log take any order of calls
    for (int i = 0; i < 700; ++i)
    {
        NpOverlapSnapSkipped(100);
        NpOverlapPose(i & 1);
        NpOverlapCrowdPose(i & 1);
        NpOverlapFrame();
    }
    std::printf(fails ? "switches %s: %d FAILED\n" : "switches %s: ok\n", mode, fails);
    return fails != 0;
}
