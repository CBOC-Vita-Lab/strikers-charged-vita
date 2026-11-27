#!/usr/bin/env python3
"""Cut test of platform/sdk/save/np_safe_file.h: builds test_safe_file.cpp with the syscall wrappers and runs it (about 15 s)."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

WRAPS = ["open", "write", "fsync", "rename", "unlink", "close"]


def main():
    here = Path(__file__).resolve().parent
    native = here.parents[2]
    cxx = shlex.split(os.environ.get("CXX", "g++"))
    with tempfile.TemporaryDirectory(prefix="safe-file-") as tmp:
        exe = Path(tmp) / "test_safe_file"
        cmd = cxx + ["-std=c++17", "-O0", "-g", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", "-I" + str(native), str(here / "test_safe_file.cpp"), "-o", str(exe)]
        cmd += ["-Wl,--wrap=" + w for w in WRAPS]
        subprocess.run(cmd, check=True)
        result = subprocess.run([str(exe)])
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
