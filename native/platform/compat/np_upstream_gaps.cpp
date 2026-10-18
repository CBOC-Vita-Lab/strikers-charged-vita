// Definitions that the decomp (7a6bf16) still lacks: virtuals it declares and lists in a vtable but does not define because
// retail has no code for them (the original linker dropped / shared them). Nothing calls them in the shipped game.
// NP (DEC): each of these used to be a generated stub (platform/stubs/sdk_stubs.c) or hand-written in a game file.
#include "Game/AI/StatsGatherer.h"

// StatsGatherer is the AI test harness's task ("Partial interface for the test harness; only its shared task and string
// implementations survive linking", include/Game/AI/StatsGatherer.h). It is never constructed by the game; the vtable that the
// constructor/destructor in StatsGatherer.cpp emit refers to these three.
void StatsGatherer::Run(float)
{
}

const char* StatsGatherer::GetName()
{
    return "StatsGatherer";
}

void StatsGatherer::DoFunctionCall(unsigned int)
{
}
