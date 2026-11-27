// Unit-test stub of the MSC_GSPLIT markers (np/gsplit.h, gt27): the pacing tests compile np_fps.cpp, whose markers need these symbols; off and inert here.
extern "C" {
int g_npGsOn = 0;
unsigned int g_npGsCnt[32];
int NpGsEnter(int) { return -1; }
void NpGsLeave(int) {}
int NpGsTaskId(const char*) { return -1; }
void NpGsPassBegin(void) {}
void NpGsPassEnd(void) {}
void NpGsWaitBegin(void) {}
void NpGsWaitEnd(void) {}
int NpGsPairId(int, int, int) { return -1; }
}
