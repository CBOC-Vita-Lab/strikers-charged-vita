#!/usr/bin/env python3
"""Cut test of the game's save through the real platform/sdk/src/nand_native.cpp (host build, nand_native.cpp + stubs; about 30 s)."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

WRAPS = ["open", "write", "fsync", "rename", "unlink", "close"]


def main():
    here = Path(__file__).resolve().parent
    sdk = here.parent
    native = sdk.parents[1]
    cxx = shlex.split(os.environ.get("CXX", "g++"))
    flags = ["-std=gnu++17", "-O1", "-g", "-w", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", "-fshort-wchar", "-funsigned-char", "-fno-short-enums",
             "-DNP_NATIVE=1", "-DNP_HOST=1", "-DNP_TARGET_HOST32=1",
             "-I" + str(sdk / "shim"), "-I" + str(sdk / "shim/internal"), "-I" + str(native / "include"), "-I" + str(sdk / "include"), "-I" + str(sdk / "src"),
             "-I" + str(native / "libs/RVL_SDK/include"), "-I" + str(native), "-I" + str(here), "-include", "np/compat.h"]
    with tempfile.TemporaryDirectory(prefix="nand-cut-") as tmp:
        exe = Path(tmp) / "test_nand_save_cut"
        cmd = cxx + flags + [str(here / "test_nand_save_cut.cpp"), str(sdk / "src/nand_native.cpp"), str(sdk / "save/np_save_import.cpp"), "-o", str(exe)] + ["-Wl,--wrap=" + w for w in WRAPS]
        subprocess.run(cmd, check=True)
        result = subprocess.run([str(exe)])
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
