#!/usr/bin/env python3
"""MSC_BOOT_MATCH never touches a save: nand_native.cpp drops every write while it is on (host build of nand_native.cpp + stubs; the 'off' run is the contrast)."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    here = Path(__file__).resolve().parent
    sdk = here.parent
    native = sdk.parents[1]
    cxx = shlex.split(os.environ.get("CXX", "g++"))
    flags = ["-std=gnu++17", "-O1", "-g", "-w", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", "-fshort-wchar", "-funsigned-char", "-fno-short-enums",
             "-DNP_NATIVE=1", "-DNP_HOST=1", "-DNP_TARGET_HOST32=1",
             "-I" + str(sdk / "shim"), "-I" + str(sdk / "shim/internal"), "-I" + str(native / "include"), "-I" + str(sdk / "include"), "-I" + str(sdk / "src"),
             "-I" + str(native / "libs/RVL_SDK/include"), "-I" + str(native), "-I" + str(here), "-include", "np/compat.h"]
    rc = 0
    with tempfile.TemporaryDirectory(prefix="nand-nosave-") as tmp:
        exe = Path(tmp) / "test_nand_nosave"
        subprocess.run(cxx + flags + [str(here / "test_nand_nosave.cpp"), str(sdk / "src/nand_native.cpp"), str(sdk / "save/np_save_import.cpp"), "-o", str(exe)], check=True)
        for mode in ("on", "off"):
            r = subprocess.run([str(exe), mode])
            rc = rc or r.returncode
        # the negative control: the same source with the guard broken (nosave() always false) must FAIL the 'on' run
        mut = Path(tmp) / "nand_native_broken.cpp"
        text = (sdk / "src/nand_native.cpp").read_text()
        assert "bool nosave() {" in text
        mut.write_text(text.replace("bool nosave() {", "bool nosave() { return false;", 1).replace('#include "np_internal.h"', '#include "np_internal.h"', 1))
        exe2 = Path(tmp) / "test_nand_nosave_broken"
        subprocess.run(cxx + flags + ["-I" + str(sdk / "src"), str(here / "test_nand_nosave.cpp"), str(mut), str(sdk / "save/np_save_import.cpp"), "-o", str(exe2)], check=True)
        r = subprocess.run([str(exe2), "on"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode == 0:
            print("NEGATIVE CONTROL FAILED: the broken guard passed the 'on' test")
            rc = rc or 1
        else:
            print("negative control: the broken guard is caught")
    sys.exit(rc)


if __name__ == "__main__":
    main()
