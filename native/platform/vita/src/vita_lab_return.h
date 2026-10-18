// For executables of ours that are NOT lab builds (benchmarks, probes) but are started on the
// console by the lab server's EXEC (tools/vitalab.py exec <file>): how to come back.
//
// The lab server (vita_lab.cpp, the application's eboot.bin = "the monitor") writes
// ux0:data/mscharged/lab/exec.txt ("<file> <unix time> <serve|run> <seconds>") and replaces
// itself with the named executable of the application folder. A lab build consumes that file
// and reloads the monitor when its run ends. Anything else leaves through
// sceKernelExitProcess, i.e. to the home screen, and the owner has to start the application
// again by hand. Call MscLabReturnOrExit(code, "what happened") instead: if the lab started
// this executable it leaves the one-line status the monitor reports (PING's "last=") and
// reloads the monitor; otherwise it exits as usual. Header only, C and C++, no other file
// of the platform layer needed; link SceAppMgr_stub and SceIofilemgr (always there).
//
//   vitalab.py put eboot-bench.bin app0:bench.bin
//   vitalab.py exec bench.bin ; vitalab.py wait-back 300 ; vitalab.py get bench-log.txt bench-log.txt
#pragma once
#if defined(__vita__)
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

static inline int MscLabStartedByLab(void) {
    SceIoStat st;
    return sceIoGetstat("ux0:data/mscharged/lab/exec.txt", &st) >= 0;
}

static inline void MscLabReturnOrExit(int code, const char* status) {
    if (MscLabStartedByLab()) {
        // (Removed first: should this executable itself be an "eboot.bin", the reload below
        // starts it again, and it must not come here a second time.)
        sceIoRemove("ux0:data/mscharged/lab/exec.txt");
        sceIoRemove("ux0:data/mscharged/lab/state.txt");
        SceUID fd = sceIoOpen("ux0:data/mscharged/lab/state.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (fd >= 0) {
            static const char head[] = "ended: ";
            sceIoWrite(fd, head, sizeof(head) - 1);
            if (status) sceIoWrite(fd, status, (SceSize)strlen(status));
            sceIoWrite(fd, "\n", 1);
            sceIoClose(fd);
        }
        if (sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL) >= 0) sceKernelDelayThread(10 * 1000 * 1000);
    }
    sceKernelExitProcess(code);
}
#endif
