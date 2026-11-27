/* np/overlap.h: 1.3 game-thread work that was done for nothing (platform/compat/np_overlap.cpp has the switches).
 *
 * Strikers already has what the Galaxy port called "logic overlap": with core plan C (MSC_FS_PIPE=1) the game thread builds frame N+1 while the send of frame N
 * and its decode run beside it, and it joins only at the next fork. What was left is work the frame does not use. Everything here runs on the render path, which
 * runs a different number of times per simulation tick on each peer, so none of it can feed the simulation; none of it draws random numbers.
 *
 *   MSC_FS_SNAPONCE=1     the send's copy of a packet (parameters, stream list, skin matrices) is made once, at the fork; the copy made at the attach was overwritten there
 *   MSC_POSE_CULL=1       a player the frame will not draw (SendToGl's own frustum test) is not posed for the renderer; =2 (open play only) not blended either
 *   MSC_CROWD_POSEONCE=1  the angle sprites of one crowd texture share a pose: its node and skin matrices are built for the first, not again for each
 *   MSC_CROWD_REFRESH=n   the crowd sprites refresh at least n times less often under the gameplay camera (1 to 4); a crowd that is not drawn refreshes 4 times less often
 *   MSC_CROWD_QUADFAST=1  the crowd sprites' quads are built with the per-sprite-independent arithmetic done once per call (np/crowdquad.h): the same floats, the same vertex data
 *   MSC_POWERUP_HIDDEN=1  a power-up slot that is hidden and whose stadium object is already hidden is not given a new world matrix nobody reads
 *   MSC_SKIN_POSE_NEEDED=1 a skin mesh computes the pose matrices of the bones its packets name, not of every node of the skeleton (nothing reads the others)
 *   MSC_NPC_LAZY=1        the decorative NPCs are not posed when they are not drawn (Stadium Crowd: HIDE)
 *
 * MSC_OVERLAP_LOG=1 prints the counters and the frame-send worker's [fs] lines every 600 frames.
 * Read once, at first use, on the game thread. The Vita build has them on, the host harness off unless the name is set (its picture comparisons keep their meaning).
 */
#ifndef NP_OVERLAP_H
#define NP_OVERLAP_H

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif
int NpFsSnapOnce(void);
int NpPoseCullLevel(void);       /* 0 off, 1 no skin pose, 2 no blend either */
int NpCrowdPoseOnce(void);
int NpCrowdRefresh(int hidden);  /* the floor of the crowd throttle, 1..4; hidden: the crowd is not drawn this frame */
int NpNpcLazy(void);
int NpCrowdQuadFast(void);
int NpPowerupHidden(void);
int NpSkinPoseNeeded(void);
int NpReplayCamCfg(void);        /* 0 off, 1 slot cache, 2 slot cache and compare with a fresh lookup */
int NpAllocMask(void);
void NpReplayCfgChecked(int differs);

void NpOverlapSnapSkipped(unsigned long bytes);
void NpOverlapPose(int culled);        /* one player, one frame */
void NpOverlapCrowdPose(int reused);   /* one crowd sprite captured */
void NpOverlapFrame(void);             /* one line every 600 frames, MSC_OVERLAP_LOG=1 */
#ifdef __cplusplus
}
#endif
#else
static inline int NpFsSnapOnce(void) { return 0; }
static inline int NpPoseCullLevel(void) { return 0; }
static inline int NpCrowdPoseOnce(void) { return 0; }
static inline int NpCrowdRefresh(int) { return 1; }
static inline int NpNpcLazy(void) { return 0; }
static inline int NpCrowdQuadFast(void) { return 0; }
static inline int NpPowerupHidden(void) { return 0; }
static inline int NpSkinPoseNeeded(void) { return 0; }
static inline int NpReplayCamCfg(void) { return 0; }
static inline int NpAllocMask(void) { return 0; }
static inline void NpReplayCfgChecked(int) {}
static inline void NpOverlapSnapSkipped(unsigned long) {}
static inline void NpOverlapPose(int) {}
static inline void NpOverlapCrowdPose(int) {}
static inline void NpOverlapFrame(void) {}
#endif

#endif // NP_OVERLAP_H
