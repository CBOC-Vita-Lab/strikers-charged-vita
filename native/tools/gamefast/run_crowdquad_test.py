#!/usr/bin/env python3
"""MSC_CROWD_QUADFAST: builds tools/gamefast/crowdquad_test.cpp with the real sources for armhf and runs it under qemu-arm (NP_ARMHF_CXX, NP_QEMU_ARM). Usage: run_crowdquad_test.py [scratch dir]"""
import os, shlex, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE = os.path.abspath(os.path.join(HERE, "..", ".."))
FLAGS = "-include stddef.h -include np/compat.h -O2 -w -fpermissive -fno-math-errno -fno-strict-aliasing -mthumb -DNP_NATIVE=1 -DNP_TARGET_ARMHF=1 -DNP_DISC_HOST_ORDER=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -Iplatform/gx/include -I. -Iinclude -Ilibs/RVL_SDK/include -Iref/RVL_SDK -Ilibs/Runtime/include -Iplatform/sdk/include -idirafter libs/MSL_C/include".split()
SRC = ["tools/gamefast/crowdquad_test.cpp", "src/NL/math.cpp", "src/NL/nlMath.cpp", "src/NL/plat/platqmath.cpp"]

def main():
    cxx, qemu = os.environ.get("NP_ARMHF_CXX"), os.environ.get("NP_QEMU_ARM")
    if not (cxx and qemu):
        print("skip: NP_ARMHF_CXX / NP_QEMU_ARM not set")
        return 0
    out = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
    os.makedirs(out, exist_ok=True)
    objs = []
    for f in SRC + ["platform/sdk/src/mtx_native.c"]:
        o = os.path.join(out, os.path.basename(f) + ".o")
        c = shlex.split(cxx)
        fl = FLAGS if f.endswith(".cpp") else [x for x in FLAGS if x != "-fpermissive"]
        subprocess.run(c + (["-x", "c++"] if f.endswith(".c") else []) + fl + ["-c", f, "-o", o], cwd=NATIVE, check=True)
        objs.append(o)
    exe = os.path.join(out, "test")
    subprocess.run(shlex.split(cxx) + objs + ["-Wl,--unresolved-symbols=ignore-all", "-o", exe], cwd=NATIVE, check=True)
    return subprocess.run([qemu, "-cpu", "cortex-a9", exe]).returncode

if __name__ == "__main__":
    sys.exit(main())
