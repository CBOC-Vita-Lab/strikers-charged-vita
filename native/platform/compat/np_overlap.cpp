// The 1.3 game-thread work skips: see include/np/overlap.h.
#include "np/overlap.h"

#include <stdio.h>
#include <stdlib.h>

// (tests/overlap_switches.cpp builds this file both ways)
#if defined(__vita__) || defined(NP_OVERLAP_VITA_DEFAULTS)
#define NP_OVERLAP_ON(onVita) (onVita)
#else
#define NP_OVERLAP_ON(onVita) 0
#endif

namespace {

// The number after NAME=, kept within lo..hi ("off" reads as 0), or `def` when the name is not set or empty.
int Number(const char* name, int def, int lo, int hi)
{
    const char* e = getenv(name);
    int v = (e == 0 || *e == 0) ? def : atoi(e);
    return v < lo ? lo : (v > hi ? hi : v);
}

// (the frame-send worker's block of [fs] lines: joins that waited and for how long, the gates by name)
extern "C" void NpFsLogStats(const char* when) __attribute__((weak));

unsigned long g_snapSkipped, g_snapBytes, g_poses, g_posesCulled, g_crowdPoses, g_crowdReused, g_frames;

} // namespace

extern "C" {

int NpFsSnapOnce(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = Number("MSC_FS_SNAPONCE", NP_OVERLAP_ON(1), 0, 1);
    }
    return v;
}

int NpPoseCullLevel(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = Number("MSC_POSE_CULL", NP_OVERLAP_ON(1), 0, 2);
    }
    return v;
}

int NpCrowdPoseOnce(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = Number("MSC_CROWD_POSEONCE", NP_OVERLAP_ON(1), 0, 1);
    }
    return v;
}

int NpCrowdRefresh(int hidden)
{
    static int shown = -1;
    if (shown < 0)
    {
        // (the host's default: NP_OVERLAP_ON gives 0 there, and 0 is below the limit)
        shown = Number("MSC_CROWD_REFRESH", NP_OVERLAP_ON(2), 1, 4);
    }
    // =1 is "as the game", a hidden crowd included
    return hidden && shown > 1 ? 4 : shown;
}

int NpNpcLazy(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = Number("MSC_NPC_LAZY", NP_OVERLAP_ON(1), 0, 1);
    }
    return v;
}

int NpCrowdQuadFast(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = Number("MSC_CROWD_QUADFAST", NP_OVERLAP_ON(1), 0, 1);
    }
    return v;
}

int NpPowerupHidden(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = Number("MSC_POWERUP_HIDDEN", NP_OVERLAP_ON(1), 0, 1);
    }
    return v;
}

int NpSkinPoseNeeded(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = Number("MSC_SKIN_POSE_NEEDED", NP_OVERLAP_ON(1), 0, 1);
    }
    return v;
}

int NpReplayCamCfg(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = Number("MSC_REPLAYCAM_CFG", NP_OVERLAP_ON(1), 0, 2);
    }
    return v;
}

int NpAllocMask(void)
{
    static int v = -1;
    if (v < 0)
    {
        v = Number("MSC_ALLOC_MASK", NP_OVERLAP_ON(1), 0, 1);
    }
    return v;
}

// (MSC_REPLAYCAM_CFG=2: one call a lookup compared with the fresh one; a line for the first difference and one every 4096 checks)
void NpReplayCfgChecked(int differs)
{
    static unsigned long checks, diffs;
    ++checks;
    diffs += differs ? 1 : 0;
    if ((differs && diffs == 1) || (checks & 4095) == 0)
    {
        fprintf(stderr, "[replaycfg] checks %lu differences %lu\n", checks, diffs);
    }
}

void NpOverlapSnapSkipped(unsigned long bytes)
{
    ++g_snapSkipped;
    g_snapBytes += bytes;
}

void NpOverlapPose(int culled)
{
    ++g_poses;
    g_posesCulled += culled ? 1 : 0;
}

void NpOverlapCrowdPose(int reused)
{
    ++g_crowdPoses;
    g_crowdReused += reused ? 1 : 0;
}

void NpOverlapFrame(void)
{
    static int log = -1;
    if (log < 0)
    {
        log = Number("MSC_OVERLAP_LOG", 0, 0, 1);
        fprintf(stderr, "[overlap] MSC_FS_SNAPONCE=%d MSC_POSE_CULL=%d MSC_CROWD_POSEONCE=%d MSC_CROWD_REFRESH=%d (a hidden crowd %d) MSC_NPC_LAZY=%d\n", NpFsSnapOnce(), NpPoseCullLevel(),
                NpCrowdPoseOnce(), NpCrowdRefresh(0), NpCrowdRefresh(1), NpNpcLazy());
    }
    if (log == 0 || ++g_frames < 600)
    {
        return;
    }
    fprintf(stderr, "[overlap] last 600 frames: packet copies not made at the attach %lu a frame (%lu KiB a frame), players not posed %lu of %lu, crowd sprites on a shared pose %lu of %lu\n",
            g_snapSkipped / g_frames, (g_snapBytes / g_frames) >> 10, g_posesCulled, g_poses, g_crowdReused, g_crowdPoses);
    if (&NpFsLogStats != 0)
    {
        NpFsLogStats("overlap");
    }
    g_snapSkipped = g_snapBytes = g_poses = g_posesCulled = g_crowdPoses = g_crowdReused = g_frames = 0;
}

} // extern "C"
