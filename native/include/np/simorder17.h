// mapi17 (wave 17): the draw window of the fork-join plan (w8/plan-simthread17.md 1.4: world NPC draw + crowd block) and the order test around it.
// MSC_SIMT_ORDER=A (default, today's order: the window is drawn inside RenderFrame, after the step), B (the window is drawn before the step, from FixedUpdateTask::Run) or
// I (test only: each window stage starts at a seeded phase mark inside the step, MSC_SIMT_SCHED=<seed>, needs MSC_GSPLIT=1). MSC_SIMT_ORDER_SEED=1 gives the window
// its own random streams (so the pictures of A and B can be compared). Host/lab use; the Vita plan never sets any of these.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int NpSimOrder17Mode(void);                                   // 0 = A (default), 1 = B, 2 = I
void NpSimOrder17Early(void);                                 // FixedUpdateTask::Run, before the step loop: starts the window in B / I
int NpSimOrder17RunPending(void);                            // MSC_SIMT=2 (overlap18): runs the stages the armed picture planned, on the calling thread (returns 1 when it ran any) (NpSimtRun between kick and join; FixedUpdateTask::Run for a picture without a step)
void NpSimOrder17Flush(int group, int pip, int captain);      // GameRenderTask::RenderFrame: group 0 = stage 0 (world NPCs), group 1 = stages 1..6 (crowd): runs what has not run yet
void NpWin17Stage(int stage, int pip, int captain);           // GameRenderTask.cpp: the stage bodies (the old inline code)
int NpWin17Wanted(int stage, int pip, int captain);           // GameRenderTask.cpp: may the stage start before the step
#ifdef __cplusplus
}
#endif
