#!/usr/bin/env python3
"""MSC_RENDER_NEON: builds tools/gamefast/render_neon_test.cpp (freestanding armhf, VitaSDK g++) and runs it under qemu-arm -cpu cortex-a9. Usage: run_render_neon_test.py [scratch dir]
Skips (0) when the compiler or qemu-arm is missing. NP_ARMHF_CXX overrides the compiler."""
import os, shlex, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
def main():
    cxx = os.environ.get("NP_ARMHF_CXX", "/opt/vitasdk/bin/arm-vita-eabi-g++")
    qemu = shutil.which("qemu-arm")
    if not (os.path.exists(shlex.split(cxx)[0]) and qemu):
        print("skip: no armhf compiler or qemu-arm")
        return 0
    out = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, "render_neon_test")
    subprocess.run(shlex.split(cxx) + "-std=c++17 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -O2 -ffp-contract=off -ffreestanding -fno-exceptions -fno-rtti -nostdlib -static -Irn_stub -I../../include".split()
                   + ["-o", exe, "render_neon_test.cpp"], cwd=HERE, check=True)
    return subprocess.run([qemu, "-cpu", "cortex-a9", exe]).returncode
if __name__ == "__main__":
    sys.exit(main())
