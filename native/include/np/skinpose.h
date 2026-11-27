// MSC_SKIN_POSE_PAR (console sampler 23 Nov, The Dump match play: ShaderSkinMesh::Pose and its callees are 8-10 % of the game thread).
// RenderSnapshot::Render drew the ten characters one after the other, each one first posing its skin meshes (cCharacter::PoseSkinMesh ->
// ShaderSkinMesh::Pose: per named bone nlMultMatrices(pose, bind, node), then the morph weights) on the game thread. With the switch on, the part of
// DrawableCharacter::Render that DECIDES (visible, shock model, pose cull, LOD / low-poly choice: it names which skin-mesh types the draw will use) runs for all
// ten first, in character order; the poses it named run on the job pool (one item per character, each item writes only that character's skin meshes and its
// four "posed" flags); then the draws run in character order, each finding its flags set, so its PoseSkinMesh calls do nothing. Render only: the skin meshes
// and the flags are read by nothing else (the simulation never touches them); inputs (the blended pose accumulators, the bind matrices) are read-only.
// A character whose Render returns before the poses (hidden, not the one to render, culled) has no pose listed and stays as it was (the same reset call).
// MSC_SKIN_POSE_PAR: 0 (code default) = the old code path, 1 on, 2 on plus verify: every listed pose is also recomputed on the game thread and the skin mesh's
//   outputs compared bit for bit, and every character's decision is made again at its draw time and compared with the one the poses were made for
//   (a [skinpose] line every 300 pictures: both mismatch counts must be 0). Flippable in a run (MSC_AB): read every picture, nothing is carried between pictures.
// MSC_SKIN_POSE_PAR_BREAK (proof mutants, must be caught): 1 character 3's mesh outputs are perturbed on the lane every 8th picture; 2 character 5's pose is left out
//   but its flags are set (stale mesh); 3 character 7's pose is made on mesh 0 instead of the type named and its flags are set; 4 character 2's job writes character 6's
//   shock mesh (a foreign write: the ownership audit of mode 2 must count it).
// Mode 2 also audits ownership: every mesh no item lists and every blended pose accumulator (the inputs) is hashed before and after the lanes; the line's
//   foreign_or_input_writes must stay 0.
// MSC_SKIN_POSE_DIGEST=1 (host proof, any mode): a rolling hash of every character's skin-mesh outputs after each picture's character draws, printed every 300 pictures
//   as [skindigest] and at exit; equal for 0, 1, 2 over a run.
#pragma once
#include "Game/Drawable/DrawableCharacter.h"
class cCharacter;
#ifdef NP_NATIVE
int NpSkinPoseMode(void);   // this picture: 0 / 1 / 2 (honours MSC_AB; 0 where the job pool has fewer than two lanes)
// d: the ten DrawableCharacters of the snapshot, src: g_pCharacters, plans / ok: what RenderDecide returned for each (defer set)
void NpSkinPoseRun(DrawableCharacter* d, cCharacter* const* src, const NpRenderPlan* plans, const bool* ok, int n);
// mode 2: the decision of character i made again at its draw time (plan2 / ok2) against the one the poses were made for
void NpSkinPoseCheckDecision(bool ok, const NpRenderPlan& plan, bool ok2, const NpRenderPlan& plan2);
int NpSkinPoseDigestOn(void);
void NpSkinPoseDigest(cCharacter* const* src, int n);
#else
static inline int NpSkinPoseMode(void) { return 0; }
#endif
