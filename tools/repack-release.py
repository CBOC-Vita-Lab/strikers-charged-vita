#!/usr/bin/env python3
"""Make a release VPK: the previous release's package with the newly built executable and notices.

    python3 tools/repack-release.py [--shadercache DIR] [--app-version NN.NN] PREVIOUS.vpk BUILT.vpk OUT.vpk

The previous package supplies the members a source build does not make (the startup shader cache
`shadercache/`, the LiveArea images and `param.sfo`). From the build come `eboot.bin` and
`licenses/`. Entries are written in a fixed order with a fixed timestamp, so the same inputs give
the same bytes. Prints the SHA-256 of the VPK and of its eboot.bin.

--shadercache DIR (1.2): the package gets the files of DIR as its `shadercache/` instead of the previous package's (the folder
`native/platform/vita/tools/make_shader_pack.py` writes: `<key>.pack` and `vgx_shaderlist.bin`; see docs/BUILDING.md, "Making the
shader seed"). Every `shadercache/` member of the previous package is dropped, so no pack of an older key is left behind by mistake.

--app-version NN.NN: update only the donor SFO's APP_VER string (for example, 01.30 for 1.3: the NP_APP_VER of native/platform/vita/vita.mk).
The donor's title identity, artwork and every other SFO byte stay unchanged.

One of --app-version and --keep-donor-version is required (1.3, SC10): the public 1.1 package showed APP_VER 01.00 on the console because this tool copied its donor's SFO
unchanged and nobody gave a version. --keep-donor-version is the old behaviour, for repacking a package that is meant to keep the donor's number.
The version actually in the output's SFO is printed, and `--app-version` is checked against NP_APP_VER of vita.mk when that file is next to this tool's tree.
"""
import argparse
import hashlib
import os
import re
import struct
import sys
import zipfile

FIXED = (2026, 1, 1, 0, 0, 0)
FROM_BUILD = ("eboot.bin", "licenses/")


def patch_app_version(raw, version):
    if not re.fullmatch(r"[0-9]{2}\.[0-9]{2}", version):
        raise ValueError("APP_VER must have Vita format NN.NN (for example 01.20)")
    try:
        magic, revision, keys, data, count = struct.unpack_from("<4sIIII", raw)
        if magic != b"\0PSF" or revision != 0x101 or count > 128 or not (20 + count * 16 <= keys <= data <= len(raw)):
            raise ValueError("malformed SFO header")
        names, ranges, target = set(), [], None
        for index in range(count):
            key, fmt, length, maximum, offset = struct.unpack_from("<HHIII", raw, 20 + index * 16)
            if keys + key >= data or length > maximum or data + offset + maximum > len(raw):
                raise ValueError("malformed SFO entry")
            keyend = raw.find(b"\0", keys + key, data)
            if keyend < 0:
                raise ValueError("unterminated SFO key")
            name = raw[keys + key:keyend].decode("ascii")
            if not name or name in names:
                raise ValueError("empty or duplicate SFO key")
            names.add(name)
            start, end = data + offset, data + offset + maximum
            if any(start < other_end and other_start < end for other_start, other_end in ranges):
                raise ValueError("overlapping SFO data fields")
            if maximum:
                ranges.append((start, end))
            value = raw[start:start + length]
            if fmt == 0x204:
                if not value or value[-1:] != b"\0" or b"\0" in value[:-1]:
                    raise ValueError("invalid SFO string")
                value[:-1].decode("utf-8")
            if name == "APP_VER":
                if fmt != 0x204 or length != 6 or not re.fullmatch(rb"[0-9]{2}\.[0-9]{2}\0", value):
                    raise ValueError("invalid APP_VER string field")
                target = start
        if target is None:
            raise ValueError("missing APP_VER field")
    except (ValueError, struct.error, UnicodeError) as error:
        raise ValueError("APP_VER: " + str(error)) from error
    return raw[:target] + version.encode("ascii") + raw[target + 5:]


def read_app_version(raw):
    """The APP_VER string of a param.sfo, or None when the file is not a readable SFO (the donor's bytes are then copied as they are)."""
    try:
        magic, revision, keys, data, count = struct.unpack_from("<4sIIII", raw)
        if magic != b"\0PSF" or count > 128:
            return None
        for index in range(count):
            key, fmt, length, maximum, offset = struct.unpack_from("<HHIII", raw, 20 + index * 16)
            if raw[keys + key:raw.find(b"\0", keys + key, data)] == b"APP_VER":
                return raw[data + offset:data + offset + length].rstrip(b"\0").decode("ascii")
    except (struct.error, UnicodeError, TypeError):
        pass
    return None


def main(previous, built, out, shadercache=None, app_version=None):
    members = {}
    with zipfile.ZipFile(previous) as old:
        for name in old.namelist():
            if not name.endswith("/") and not name.startswith(FROM_BUILD):
                members[name] = old.read(name)
    if shadercache:
        members = {n: d for n, d in members.items() if not n.startswith("shadercache/")}
        names = sorted(os.listdir(shadercache))
        if not any(n.endswith(".pack") for n in names):
            sys.exit("no <key>.pack in " + shadercache)
        for n in names:
            path = os.path.join(shadercache, n)
            if not os.path.isfile(path):
                sys.exit(path + " is not a file (give the folder make_shader_pack.py wrote)")
            with open(path, "rb") as f:
                members["shadercache/" + n] = f.read()
    with zipfile.ZipFile(built) as new:
        for name in new.namelist():
            if not name.endswith("/") and name.startswith(FROM_BUILD):
                members[name] = new.read(name)
    if "eboot.bin" not in members or not any(n.startswith("shadercache/") for n in members):
        sys.exit("missing eboot.bin or shadercache/ in the inputs")
    if app_version is not None:
        if "sce_sys/param.sfo" not in members:
            raise ValueError("APP_VER: missing donor sce_sys/param.sfo")
        members["sce_sys/param.sfo"] = patch_app_version(members["sce_sys/param.sfo"], app_version)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as vpk:
        for name in sorted(members, key=lambda n: (n != "eboot.bin", n)):
            info = zipfile.ZipInfo(name, FIXED)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            vpk.writestr(info, members[name])
    version = app_version if app_version is not None else read_app_version(members.get("sce_sys/param.sfo"))
    print("APP_VER in the output's param.sfo:", version or "unknown (opaque SFO)")
    with open(out, "rb") as f:
        print(hashlib.sha256(f.read()).hexdigest(), out)
    print(hashlib.sha256(members["eboot.bin"]).hexdigest(), "eboot.bin")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--shadercache", metavar="DIR")
    parser.add_argument("--app-version", metavar="NN.NN")
    parser.add_argument("--keep-donor-version", action="store_true", help="copy the donor's param.sfo unchanged (the donor's APP_VER stays)")
    parser.add_argument("previous")
    parser.add_argument("built")
    parser.add_argument("out")
    args = parser.parse_args()
    if args.app_version is None and not args.keep_donor_version:
        parser.error("give --app-version NN.NN (the release's APP_VER, 01.30 for 1.3) or --keep-donor-version: a package repacked from an older donor otherwise shows the donor's version")
    if args.app_version is not None and args.keep_donor_version:
        parser.error("--app-version and --keep-donor-version exclude each other")
    try:
        main(args.previous, args.built, args.out, shadercache=args.shadercache, app_version=args.app_version)
    except ValueError as error:
        parser.error(str(error))
