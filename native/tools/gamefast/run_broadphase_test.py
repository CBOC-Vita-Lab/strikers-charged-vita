#!/usr/bin/env python3
"""MSC_ODE_BROADPHASE: builds the real ODE sources with tools/gamefast/np_broadphase_test.cpp and requires the same callback sequence with the switch at 0 and 1, for the NEON path and the C path, armhf under qemu-arm
(tool chain and emulator named in NP_ARMHF_CXX / NP_QEMU_ARM). Usage: run_broadphase_test.py [scratch dir] [seeds]"""
import glob, os, shlex, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE = os.path.abspath(os.path.join(HERE, "..", ".."))
FLAGS = "-include stddef.h -include include/np/compat.h -O2 -w -fpermissive -fno-strict-aliasing -DNP_NATIVE=1 -DNP_DISC_HOST_ORDER=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -DdNODEBUG=1 -DdSINGLE=1 -Iplatform/gx/include -I. -Iinclude -Isrc/ode -Isrc/zlib -Ilibs/RVL_SDK/include -Iref/RVL_SDK -Ilibs/Runtime/include -idirafter libs/MSL_C/include".split()

def build(cxx, extra, out):
    objs = []
    os.makedirs(out, exist_ok=True)
    for f in sorted(glob.glob(os.path.join(NATIVE, "src/ode/*.cpp"))) + [os.path.join(HERE, "np_broadphase_test.cpp")]:
        o = os.path.join(out, os.path.basename(f) + ".o")
        if not os.path.exists(o) or os.path.getmtime(o) < max(os.path.getmtime(p) for p in [f] + glob.glob(os.path.join(NATIVE, "src/ode/*.h"))):
            subprocess.run(cxx + FLAGS + extra + ["-c", f, "-o", o], cwd=NATIVE, check=True)
        objs.append(o)
    exe = os.path.join(out, "test")
    subprocess.run(cxx + objs + ["-Wl,--unresolved-symbols=ignore-all", "-o", exe], cwd=NATIVE, check=True)
    return exe

def compare(run, seeds, name):
    outs = {}
    for mode in "012":
        env = dict(os.environ, MSC_ODE_BROADPHASE=mode)
        outs[mode] = subprocess.run(run + [str(seeds)], env=env, check=True, capture_output=True, text=True).stdout
    a = [l for l in outs["0"].splitlines() if not l.startswith("ode ")]
    pairs = sum(int(l.split()[2]) for l in a)
    for mode in "12":
        b = [l for l in outs[mode].splitlines() if not l.startswith("ode ")]
        if a != b:
            bad = [(x, y) for x, y in zip(a, b) if x != y]
            print(f"FAIL {name}: switch {mode} differs on {len(bad)} seeds, first {bad[:1]}")
            return 1
    wrong = [l for l in outs["2"].splitlines() if l.startswith("ode ") and "wrong=0 " not in l and " wrong 0" not in l]
    if wrong:
        print(f"FAIL {name}: verify mode: {wrong[0]}")
        return 1
    print(f"ok {name}: {len(a)} seeds, {pairs} callbacks, same sequence at 0, 1 and 2")
    return 0

def main():
    scratch = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
    seeds = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    cxx, qemu = os.environ.get("NP_ARMHF_CXX"), os.environ.get("NP_QEMU_ARM")
    if not (cxx and qemu):
        print("skip: NP_ARMHF_CXX / NP_QEMU_ARM not set (the ODE sources build for 32-bit targets only)")
        return 0
    q = [qemu, "-cpu", "cortex-a9"]
    rc = compare(q + [build(shlex.split(cxx), ["-mthumb"], os.path.join(scratch, "armhf"))], seeds, "armhf NEON path (qemu)")
    rc |= compare(q + [build(shlex.split(cxx), ["-mthumb", "-DNP_BP_NO_NEON=1"], os.path.join(scratch, "armhf-c"))], seeds, "armhf C path (qemu)")
    return rc

if __name__ == "__main__":
    sys.exit(main())
