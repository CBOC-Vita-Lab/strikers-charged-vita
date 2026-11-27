#ifndef NP_SNAP_ONECOPY_H
#define NP_SNAP_ONECOPY_H
// MSC_SNAP_ONECOPY (wave 11, gt16a): the snapshot slot of a character takes its pose from the simulation's accumulator once and only the parts something reads.
// DrawableCharacter::Grab copied all seven arrays of cPoseAccumulator, among them m_PrevNodeMatrices (28 % of the bytes, about 14 KiB a character): the previous
// picture's node matrices, which only the simulation's own accumulator reads (Character.cpp GetJointVelocity, PhysicsPose.cpp). Nothing reads that array of a snapshot:
// the blend builds its own pose from m_rot / m_trans / m_scale, the recorder writes m_pQuaternions / m_trans / m_scale, and cull and prop code read m_NodeMatrices (still copied).
//   0  the old copy (default in the code)      1  the copy without m_PrevNodeMatrices
//   2  proof: the previous matrices of the slot are filled with 0xFF (a NaN) instead of copied: any reader of them would change a picture
//   3  proof mutant: as 1, then one recorded quaternion is nudged: the replay bytes and the pictures must change
// np_det: NP_REPLAY_HASH=1 prints a hash of the whole recorded replay every 300 steps ([replayhash]), for the proofs.
extern int gNpSnapOneCopy;
int NpSnapOneCopyInit();
inline int NpSnapOneCopy() { return gNpSnapOneCopy < 0 ? NpSnapOneCopyInit() : gNpSnapOneCopy; }
#endif
