#!/usr/bin/env python3
"""MSC_GT18_QUAT: builds tools/gamefast/gt18_quat_test.cpp with the real sources and runs it. Default: x86 -m32 -msse2 with clang like the host build (NP_GT18_CXX overrides the
compiler line, e.g. the armhf shim + NP_QEMU_ARM for qemu-arm). MSC_GT18_BREAK=1 in the environment must make it fail (proof mutant). Usage: run_gt18_quat_test.py [scratch dir]"""
import os, shlex, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE = os.path.abspath(os.path.join(HERE, "..", ".."))
FLAGS = ("-isystem /usr/include/x86_64-linux-gnu -include np/compat.h -O2 -w -fno-math-errno -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -DNP_NATIVE=1 -DdSINGLE=1 -DNP_DISC_HOST_ORDER=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 "
         "-Iplatform/gx/include -I. -Iinclude -Ilibs/RVL_SDK/include -Iref/RVL_SDK -Ilibs/Runtime/include -Iplatform/sdk/include -idirafter libs/MSL_C/include -funsigned-char").split()
SRC = ["tools/gamefast/gt18_quat_test.cpp", "src/NL/math.cpp", "src/NL/nlMath.cpp", "src/NL/plat/platqmath.cpp", "platform/sdk/src/mtx_native.c"]

def main():
    cxx = os.environ.get("NP_GT18_CXX", "clang++ -m32 -msse2 -mfpmath=sse -DNP_TARGET_HOST32=1 -std=gnu++11 -fno-exceptions -fno-rtti")
    qemu = os.environ.get("NP_QEMU_ARM", "")
    out = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
    os.makedirs(out, exist_ok=True)
    objs = []
    for f in SRC:
        o = os.path.join(out, os.path.basename(f) + ".o")
        c = shlex.split(cxx)
        extra = ["-x", "c", "-std=gnu11"] if f.endswith(".c") else ["-x", "c++"]
        if f.endswith(".c"):
            c = [x for x in c if not x.startswith("-std=") and x not in ("-fno-exceptions", "-fno-rtti")]
        subprocess.run(c + extra + FLAGS + ["-c", f, "-o", o], cwd=NATIVE, check=True)
        objs.append(o)
    exe = os.path.join(out, "test")
    subprocess.run(shlex.split(cxx) + objs + ["-lm", "-o", exe], cwd=NATIVE, check=True)
    return subprocess.run(([qemu, "-cpu", "cortex-a9"] if qemu else []) + [exe]).returncode

if __name__ == "__main__":
    sys.exit(main())
