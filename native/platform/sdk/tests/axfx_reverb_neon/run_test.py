#!/usr/bin/env python3
"""Builds tests/axfx_reverb_neon with the Vita compiler and runs it as ARM code under Unicorn (QEMU's CPU core; pip install unicorn; no system needed).
Exit 0: the checks ran and passed. Exit 1: a check failed. Exit 2: could not run (no compiler or no unicorn), and it says so; that is not a pass.
    run_test.py [--break] [--keep DIR]        --break builds the kernel with a deliberate wrong order, which must fail."""
import os, re, shutil, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.abspath(os.path.join(HERE, "..", ".."))
NATIVE = os.path.abspath(os.path.join(SDK, "..", ".."))
VITASDK = os.environ.get("VITASDK", "/opt/vitasdk")
CC = os.path.join(VITASDK, "bin", "arm-vita-eabi-gcc")
ISA = ["-mcpu=cortex-a9", "-mfpu=neon", "-mfloat-abi=hard", "-mthumb"]
FLAGS = ISA + ["-O2", "-g0", "-std=gnu11", "-w", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-pic",
         "-fno-strict-aliasing", "-fwrapv", "-fshort-wchar", "-funsigned-char", "-fno-short-enums", "-fno-delete-null-pointer-checks", "-fno-common", "-ffunction-sections", "-fdata-sections"]


def build(work, broken):
    inc = os.path.join(work, "inc")
    subprocess.check_call([os.path.join(SDK, "tools", "mkinc.sh"), inc])
    incs = ["-I" + p for p in (os.path.join(SDK, "shim"), os.path.join(SDK, "shim", "internal"), os.path.join(NATIVE, "include"), os.path.join(SDK, "include"), os.path.join(SDK, "src"), inc)]
    incs += ["-include", "np/compat.h", "-DNP_VITA=1", "-DNP_TARGET_VITA=1", "-DNP_NATIVE=1", "-DBUILD_VERSION=0", "-DVERSION_R4QE01", "-DNDEBUG=1"]
    gen = os.path.join(work, "AXFXReverbHiExp.c")
    ref = os.path.join(NATIVE, "ref", "RVL_SDK", "axfx", "AXFXReverbHiExp.c")
    with open(gen, "w") as out:
        subprocess.check_call(["sed", "-f", os.path.join(SDK, "patches", "axfx", "AXFXReverbHiExp.c.sed"), ref], stdout=out)
    kernel = os.path.join(SDK, "src", "axfx_reverb_neon.c")
    if broken:
        text = open(kernel).read()
        text2 = text.replace("return vmlsq_f32(tmp, written, coef);", "return vmlsq_f32(tmp, in, coef);")
        assert text != text2
        kernel = os.path.join(work, "broken.c")
        open(kernel, "w").write(text2)
    objs = []
    for src, extra in ((gen, ["-include", "np_ref_prelude.h"]), (kernel, []), (os.path.join(HERE, "reverb_test.c"), [])):
        obj = os.path.join(work, os.path.basename(src) + ".o")
        subprocess.check_call([CC] + FLAGS + incs + extra + ["-c", src, "-o", obj])
        objs.append(obj)
    elf = os.path.join(work, "t.elf")
    subprocess.check_call([CC] + ISA + ["-nostdlib", "-nostartfiles", "-static", "-Wl,--gc-sections", "-Wl,-e,TestMain", "-Wl,-u,CountSetup", "-Wl,-u,CountFrames", "-T", os.path.join(HERE, "link.ld")] + objs + ["-lgcc", "-o", elf])
    return elf


def load(elf):
    """Program headers and symbols, with the Python standard library only."""
    data = open(elf, "rb").read()
    phoff, shoff = struct.unpack_from("<II", data, 28)
    phentsize, phnum, shentsize, shnum, _ = struct.unpack_from("<HHHHH", data, 42)
    segments = []
    for i in range(phnum):
        ptype, off, vaddr, _, filesz, memsz = struct.unpack_from("<IIIIII", data, phoff + i * phentsize)
        if ptype == 1:
            segments.append((vaddr, data[off:off + filesz], memsz))
    symbols = {}
    for i in range(shnum):
        _, stype, _, _, off, size, link = struct.unpack_from("<IIIIIII", data, shoff + i * shentsize)
        if stype == 2:
            strs = struct.unpack_from("<I", data, shoff + link * shentsize + 16)[0]
            for k in range(size // 16):
                name, value = struct.unpack_from("<II", data, off + k * 16)
                end = data.index(b"\0", strs + name)
                symbols[data[strs + name:end].decode()] = value
    return segments, symbols


def main():
    broken = "--break" in sys.argv
    if not os.path.exists(CC):
        print("COULD NOT RUN: no " + CC)
        return 2
    try:
        import unicorn
        from unicorn import arm_const as A
    except ImportError:
        print("COULD NOT RUN: python module unicorn is missing (pip install unicorn). The test was not run.")
        return 2
    work = tempfile.mkdtemp(prefix="axfx-neon-")
    try:
        elf = build(work, broken)
        segments, sym = load(elf)
        mu = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_THUMB)
        mu.ctl_set_cpu_model(A.UC_CPU_ARM_CORTEX_A9)
        low = min(s[0] for s in segments) & ~0xfff
        high = (max(s[0] + s[2] for s in segments) + 0xfff) & ~0xfff
        mu.mem_map(low, high - low)
        for vaddr, blob, _ in segments:
            mu.mem_write(vaddr, blob)
        stack, stop = 0x40000000, 0x41000000
        mu.mem_map(stack, 0x100000)
        mu.mem_map(stop, 0x1000)
        mu.mem_write(stop, b"\x00\xbf\x00\xbf")
        mu.reg_write(A.UC_ARM_REG_SP, stack + 0xff000)
        # Coprocessors 10/11 on, FPEXC.EN, FPSCR as a process gets it: FZ clear, round to nearest.
        mu.reg_write(A.UC_ARM_REG_C1_C0_2, mu.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xf << 20))
        mu.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
        mu.reg_write(A.UC_ARM_REG_FPSCR, 0)

        def word(name):
            return struct.unpack("<I", mu.mem_read(sym[name], 4))[0]

        def run(entry):
            mu.reg_write(A.UC_ARM_REG_LR, stop | 1)
            mu.emu_start(entry | 1, stop)

        run(sym["TestMain"])
        fields = ("g_frames", "g_cases", "g_initFailed", "g_outputMismatch", "g_positionMismatch", "g_stateNormalMismatch", "g_stateDenormalDiffs", "g_canaryBad")
        got = {f: word(f) for f in fields}
        print(" ".join("%s=%u" % (k[2:], v) for k, v in got.items()))
        if got["g_outputMismatch"]:
            print("first bad (frame, sample, reference, neon):", struct.unpack("<4i", mu.mem_read(sym["g_firstBad"], 16)))
        ok = got["g_cases"] == 8 and got["g_stateDenormalDiffs"] > 0 and got["g_initFailed"] == 0 and not any(got[k] for k in fields[3:6]) and got["g_canaryBad"] == 0
        if not ok:
            print("FAILED")
            return 1
        counts = []
        for neon in (0, 1):
            run(sym["CountSetup"])
            mu.mem_write(sym["g_countNeon"], struct.pack("<I", neon))
            n = [0]

            def tick(uc, address, size, user):
                n[0] += 1

            hook = mu.hook_add(unicorn.UC_HOOK_CODE, tick)
            mu.ctl_flush_tb()   # (blocks translated before the hook existed would not count)
            run(sym["CountFrames"])
            mu.hook_del(hook)
            counts.append(n[0] / 32.0)
        print("instructions per 96-sample frame, reference %.0f, NEON %.0f (a count, not a time: cycles were not measured)" % (counts[0], counts[1]))
        print("PASS: ARM code run under Unicorn %s (cortex-a9 model, QEMU's NEON and VFP helpers); no Vita involved" % unicorn.__version__)
        return 0
    finally:
        if "--keep" in sys.argv:
            print("kept", work)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
