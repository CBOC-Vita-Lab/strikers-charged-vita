// The game-thread split's markers (np/gsplit.h NpGsScope, used by np_fps.cpp since the gsplit waves) over no split: the units include the real np_fps.cpp and
// link nothing else, so the three symbols its scopes name are defined here (off: g_npGsOn is 0, NpGsEnter is never called).
#pragma once
int g_npGsOn = 0;
int NpGsEnter(int) { return -1; }
void NpGsLeave(int) {}
