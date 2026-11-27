// Wave 9 (MSC_PHYS_POSE): the pose of the ten characters inside PhysicsWorld::PostUpdate on the job lanes.
// The pose (cPoseAccumulator::Pose: the pose tree's Evaluate and BuildNodeMatrices) was 54-74 percent of the "physics" of the console's game thread
// (w8/gsplit.md v2). It reads only its own character (tree, accumulator, world matrix, head track) and shared read-only data, so the ten run beside
// each other. Order is kept: pass 1 (game thread, body order) runs each character's PostUpdate up to the pose, pass 2 (lanes) the poses, pass 3 is the
// ordinary body loop in body order, in which each character finds its pose done and goes on with what followed it (bone volumes, ball joint, goalie post).
// MSC_PHYS_POSE: 0 off (default code path, unchanged), 1 on, 2 on plus verify: every pose is also recomputed serially on the game thread from the saved state
// and compared bit for bit (a [physpose] line every 300 steps: mismatches must be 0). MSC_PHYS_POSE_BREAK=1 (test mutant): nudges one character's
// position in pass 1 (must change the determinism trace); =2: changes a lane's result of one character (must be counted by the verify).
#pragma once
class cCharacter;
class PhysicsWorld;
#ifdef NP_NATIVE
int NpPhysPoseMode(void);                     // 0 / 1 / 2
void NpPhysPoseRun(PhysicsWorld* world);      // passes 1 and 2; PhysicsWorld::PostUpdate calls it before its loop
bool NpPhysPosePending(const cCharacter* c);  // pass 1 and 2 are done for c this step
bool NpPhysPoseTake(const cCharacter* c);     // the same, and clears it (cCharacter::PostPhysicsUpdate)
bool NpSim26BonesOn(void);                    // MSC_SIM26_BONES != 0 for this step's lane poses (read on the game thread in NpPhysPoseRun)
#else
static inline bool NpSim26BonesOn() { return false; }
static inline bool NpPhysPosePending(const cCharacter*) { return false; }
static inline bool NpPhysPoseTake(const cCharacter*) { return false; }
#endif
