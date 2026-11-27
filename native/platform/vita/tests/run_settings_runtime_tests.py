#!/usr/bin/env python3
"""Build actual startup settings blocks and VitaPlan with host I/O at the OS boundary."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def main():
    vita = Path(__file__).resolve().parents[1]
    source = (vita / "src/np_vita_main.cpp").read_text()
    settings = (vita / "src/np_vita_settings.cpp").read_text()
    with tempfile.TemporaryDirectory(prefix="settings-runtime-") as temporary:
        build = Path(temporary)
        loader = between(source, "char g_envNames[", "// Runs before every other")
        fallback = between(source, "    if (NpSettingsScreen::Requested(argc, argv))", "#else")
        restart = between(settings, "    if (result == Result::kStart) {", "\n// The running game starts again as a fresh process")
        restart = restart[:restart.rfind("}")]
        relaunch = between(settings, "bool RelaunchRunningGame() {", "\nvoid Pump()")   # model18: the one function both restarts use
        (build / "settings_runtime_relaunch.inc").write_text(relaunch)
        (build / "settings_runtime_loader.inc").write_text(loader)
        (build / "settings_runtime_fallback.inc").write_text(fallback)
        (build / "settings_runtime_restart.inc").write_text(restart)
        stub = build / "psp2/io/fcntl.h"
        stub.parent.mkdir(parents=True)
        stub.write_text("""#pragma once
using SceUID = int;
using SceSize = unsigned int;
constexpr int SCE_O_RDONLY = 0;
constexpr int SCE_O_WRONLY = 1;
constexpr int SCE_O_CREAT = 2;
constexpr int SCE_O_TRUNC = 4;
int sceIoOpen(const char*, int, int);
int sceIoRead(int, void*, unsigned int);
int sceIoWrite(int, const void*, unsigned int);
int sceIoClose(int);
""")
        sysmem = build / "psp2/kernel/sysmem.h"
        sysmem.parent.mkdir(parents=True)
        sysmem.write_text("""#pragma once
int sceKernelIsPSVitaTV(void);
""")
        procmgr = build / "psp2/kernel/processmgr.h"
        procmgr.write_text("""#pragma once
unsigned long long sceKernelGetProcessTimeWide(void);
""")
        command = shlex.split(os.environ.get("CXX", "c++"))
        command += ["-std=c++17", "-O1", "-g", "-pthread", "-D__vita__", "-DNP_PLAYER", "-I" + str(build), "-I" + str(vita / "src")]
        if os.environ.get("SANITIZE") == "1":
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        binary = build / "test_settings_runtime"
        command += [str(vita / "tests/test_settings_runtime.cpp"), str(vita / "src/np_settings.cpp"), str(vita / "src/vita_plan.cpp"), "-o", str(binary)]
        subprocess.run(command, check=True)
        tests = ["fallback-named", "fallback-auto", "full-named", "full-cn", "full-picture", "file-cap", "duplicate-cap", "stock-sim-core3", "timers-reread", "timers-explicit-0", "timers-explicit-3", "full-tracking", "performance-group", "quality-defaults", "guard-auto", "guard-manual", "guard-return-auto"]
        tests += ["guard-boot-" + str(n) for n in range(5)]
        tests += ["concurrent-cancel-heartbeat", "concurrent-restore-failure", "concurrent-cancel-failure"]
        environment = {key: value for key, value in os.environ.items() if not key.startswith(("MSC_", "NP_"))}
        failed = []
        completed = []

        def run_test(test, *arguments, label=None):
            result = subprocess.run([str(binary), test, *map(str, arguments)], capture_output=True, text=True, env=environment)
            label = label or test
            completed.append(label)
            print((label + ": " if label != test else "") + (result.stdout or "process exited\n"), end="")
            if result.returncode:
                print(result.stderr, end="")
                failed.append(label)

        for test in tests:
            run_test(test)
        for initial in range(4):
            for transition in ("leave-named", "leave-manual", "return-named", "return-manual", "failed-leave", "cleared-leave"):
                guard = build / f"guard-{initial}-{transition}.txt"
                guard.write_text(f"{initial}\n")
                run_test("guard-" + transition, guard, label=f"{transition}-{initial}")
                expected = 0 if transition == "cleared-leave" else initial + 1 if transition.startswith(("return", "failed")) else initial
                run_test("guard-fresh", guard, expected, label=f"fresh-after-{transition}-{initial}")
            for transition in ("restart-success-startup", "restart-success-running", "settings-exit", "restart-failed-running", "restart-failed-startup", "restart-return-heartbeat", "restart-return-failure"):
                guard = build / f"exec-{initial}-{transition}.txt"
                guard.write_text(f"{initial}\n")
                run_test(transition, guard, label=f"{transition}-{initial}")
                if transition.startswith("restart-success"):
                    run_test("guard-fresh-named", guard, initial, label=f"named-after-{transition}-{initial}")
                expected = 0 if transition == "restart-return-heartbeat" else initial + 1 if transition.startswith(("restart-failed", "restart-return-failure")) else initial
                run_test("guard-fresh", guard, expected, label=f"fresh-after-{transition}-{initial}")
        print(f"settings runtime: {len(completed)} cases, {len(failed)} failed")
        return bool(failed)


if __name__ == "__main__":
    raise SystemExit(main())
