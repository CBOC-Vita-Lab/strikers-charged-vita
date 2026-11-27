// Wave 12 (ai27): exact cuts in the AI part of the 50 Hz step. Every switch changes only the instructions run, never a decision, an RNG draw, or
// the order or value of a float operation. Each is an int read from the environment on first use:
//   MSC_AI27_SORT    1  cGame::UpdateCachedGameData: a closest-players list that is already in order (the comparator says no neighbour pair is out of
//                       order) is not sent to qsort (an insertion or merge sort leaves such a list as it is). 2 verify: qsort runs on a copy as well, a
//                       difference is counted "wrong" (must be 0).
//   MSC_AI27_VCOPY   1  FuzzyVariantCollection::operator= tests the slot pointers directly instead of calling IsSet twice per slot (no verify mode: the code is the old loop with the call removed).
//   MSC_AI27_AVOID   1  AvoidController::Update: IsAvoiding(kind) is worked out once per kind for the whole update (it reads only the fielder's state
//                       and m_ThingsToAvoid, which the loop does not change). 2 verify: the plain call runs as well, a difference is counted "wrong".
//   MSC_AI27_CTRL    1  UserControlledT: stops at the first pad assigned to the side instead of counting all 16. 2 verify.
// MSC_AI27_BREAK=<n> (proof mutants, each spoils exactly one switch so that the simulation changes): 1 SORT (a list is left alone when only its first
// pair is in order), 2 VCOPY (a slot that both collections hold keeps its old value), 3 AVOID (the kind memo is never refreshed: the first answer is reused for
// every fielder), 4 CTRL (pads 0 and 1 are not looked at).
// Counters print as [ai27] lines every 200000 events and at exit.
#pragma once
#ifdef NP_NATIVE
enum { NPAI27_SORT, NPAI27_VCOPY, NPAI27_AVOID, NPAI27_CTRL, NPAI27_N };
extern int gNpAi27Ready;
extern int gNpAi27Val[NPAI27_N];
extern int gNpAi27Break;
extern unsigned long long gNpAi27Cnt[NPAI27_N][4];
void NpAi27Init(void);
void NpAi27Tick(void);
static inline int NpAi27(int s)
{
    if (__builtin_expect(!gNpAi27Ready, 0)) NpAi27Init();
    return gNpAi27Val[s];
}
static inline int NpAi27Break(void) { return gNpAi27Break; }
#define NPAI27_COUNT(s, k, n) (gNpAi27Cnt[(s)][(k)] += (unsigned long long)(n))
#else
#define NPAI27_COUNT(s, k, n) ((void)0)
static inline int NpAi27(int) { return 0; }
static inline int NpAi27Break(void) { return 0; }
#endif
