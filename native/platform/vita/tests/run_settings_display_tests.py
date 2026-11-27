#!/usr/bin/env python3
"""Run the real settings renderer and console against an asynchronous display boundary."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    vita = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="settings-display-") as temporary:
        build = Path(temporary)
        headers = {
            "psp2/kernel/sysmem.h": """#pragma once
using SceUID = int;
using SceSize = unsigned int;
constexpr int SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW = 1;
SceUID sceKernelAllocMemBlock(const char*, int, SceSize, const void*);
int sceKernelGetMemBlockBase(SceUID, void**);
int sceKernelFreeMemBlock(SceUID);
""",
            "psp2/kernel/processmgr.h": """#pragma once
#include <cstdint>
uint64_t sceKernelGetProcessTimeWide();
""",
            "psp2/kernel/threadmgr.h": """#pragma once
#include <psp2/kernel/sysmem.h>
SceUID sceKernelGetThreadId();
int sceKernelDelayThread(unsigned int);
""",
            "psp2/ctrl.h": """#pragma once
struct SceCtrlData { unsigned int buttons; };
constexpr unsigned int SCE_CTRL_START = 1;
int sceCtrlPeekBufferPositive(int, SceCtrlData*, int);
""",
            "psp2/display.h": """#pragma once
#include <cstdint>
struct SceDisplayFrameBuf {
    unsigned int size;
    void* base;
    unsigned int pitch, pixelformat, width, height;
};
constexpr int SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 = 0;
constexpr int SCE_DISPLAY_SETBUF_NEXTFRAME = 1;
int sceDisplaySetFrameBuf(const SceDisplayFrameBuf*, int);
int sceDisplayWaitVblankStart();
int sceDisplayWaitSetFrameBuf();
""",
        }
        for relative, source in headers.items():
            target = build / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(source)
        command = shlex.split(os.environ.get("CXX", "c++"))
        command += ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-D__vita__",
                    "-I" + str(build), "-I" + str(vita / "src"),
                    "-I" + str(vita.parent / "harness/include")]
        if os.environ.get("SANITIZE") == "1":
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        binary = build / "test_settings_display"
        command += [str(vita / "tests/test_settings_display.cpp"),
                    str(vita / "src/vita_console.cpp"), str(vita / "src/np_startup_check.cpp"), str(vita / "src/np_settings.cpp"),
                    "-o", str(binary)]
        subprocess.run(command, check=True)
        for case in ("plain", "art", "boot-resume", "allocation-failure", "base-failure", "queue-failure", "wait-failure"):
            subprocess.run([str(binary), case], check=True)


if __name__ == "__main__":
    main()
