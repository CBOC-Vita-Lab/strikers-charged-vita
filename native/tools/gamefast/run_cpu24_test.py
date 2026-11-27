#!/usr/bin/env python3
"""MSC_CPU24_PART unit test: tools/gamefast/cpu24_test.cpp. Usage: run_cpu24_test.py host|arm [scratch dir]
host: g++ -m32 -msse2 -mfpmath=sse -O2 (like the host build); arm: freestanding armhf with the VitaSDK g++ under qemu-arm -cpu cortex-a9 (-O2, no -ffp-contract flag: the game's own float flags).
CPU24_BREAK=1 in the environment compiles the proof mutant, which must make the test fail. Skips (0) when the arm tools are missing."""
import os, shlex, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "host"
    out = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp()
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, "cpu24_test_" + which)
    brk = ["-DCPU24_BREAK=1"] if os.environ.get("CPU24_BREAK") == "1" else []
    if which == "host":
        subprocess.run(["g++", "-m32", "-msse2", "-mfpmath=sse", "-O2", "-fwrapv"] + brk + ["-o", exe, "cpu24_test.cpp"], cwd=HERE, check=True)
        return subprocess.run([exe]).returncode
    cxx = os.environ.get("NP_ARMHF_CXX", "/opt/vitasdk/bin/arm-vita-eabi-g++")
    qemu = shutil.which("qemu-arm")
    if not (os.path.exists(shlex.split(cxx)[0]) and qemu):
        print("skip: no armhf compiler or qemu-arm")
        return 0
    subprocess.run(shlex.split(cxx) + "-std=c++17 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O2 -fwrapv -ffreestanding -fno-exceptions -fno-rtti -nostdlib -static -DNP_CPU24_FREESTANDING=1".split()
                   + brk + ["-o", exe, "cpu24_test.cpp"], cwd=HERE, check=True)
    return subprocess.run([qemu, "-cpu", "cortex-a9", exe]).returncode
if __name__ == "__main__":
    sys.exit(main())
