#!/usr/bin/env python3
"""Make a release VPK: the previous release's package with the newly built executable and notices.

    python3 tools/repack-release.py PREVIOUS.vpk BUILT.vpk OUT.vpk

The previous package supplies the members a source build does not make (the startup shader cache
`shadercache/`, the LiveArea images and `param.sfo`). From the build come `eboot.bin` and
`licenses/`. Entries are written in a fixed order with a fixed timestamp, so the same inputs give
the same bytes. Prints the SHA-256 of the VPK and of its eboot.bin.
"""
import hashlib
import sys
import zipfile

FIXED = (2026, 1, 1, 0, 0, 0)
FROM_BUILD = ("eboot.bin", "licenses/")


def main(previous, built, out):
    members = {}
    with zipfile.ZipFile(previous) as old:
        for name in old.namelist():
            if not name.endswith("/") and not name.startswith(FROM_BUILD):
                members[name] = old.read(name)
    with zipfile.ZipFile(built) as new:
        for name in new.namelist():
            if not name.endswith("/") and name.startswith(FROM_BUILD):
                members[name] = new.read(name)
    if "eboot.bin" not in members or not any(n.startswith("shadercache/") for n in members):
        sys.exit("missing eboot.bin or shadercache/ in the inputs")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as vpk:
        for name in sorted(members, key=lambda n: (n != "eboot.bin", n)):
            info = zipfile.ZipInfo(name, FIXED)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            vpk.writestr(info, members[name])
    with open(out, "rb") as f:
        print(hashlib.sha256(f.read()).hexdigest(), out)
    print(hashlib.sha256(members["eboot.bin"]).hexdigest(), "eboot.bin")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
