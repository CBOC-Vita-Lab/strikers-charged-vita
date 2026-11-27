#!/usr/bin/env python3
"""Unpack the isolated test packages of the three versions into what session.sh swap pushes to the console.

    python3 prepare-versions.py --out versions \
        --vpk 1.3=<isolated 1.3 .vpk> --vpk 1.3p=<isolated 1.3 proposed .vpk> --vpk 1.2=<isolated 1.2 .vpk> --vpk 1.1=<isolated 1.1 .vpk> --vpk 1.0=<isolated 1.0 .vpk> \
        [--sha256 1.3=<recorded eboot sha-256> ...]
    python3 prepare-versions.py --out versions --eboot r15=<r15 isolated .vpk> --eboot r15pgo=<r15 PGO isolated .vpk>    # executables alone, for session.sh eswap (the PGO A/B)

Each package is a fail-closed package from tools/stage-hardware-test.py (title CDXMP0001, content id ...CDXMP0001_00-HARDWARETEST0000).
The check is that tool's own `verify`: title and content id, a SELF executable, only approved members. With --sha256 the executable must
also match the digest recorded when it was built; without it the digest is taken from the package itself (printed, so you can compare it
with the build record) and the check says "structure only".

Writes <out>/<version>/eboot.bin, <out>/<version>/shadercache/... and <out>/<version>/MANIFEST.json (sha-256 of every file), and refuses to
overwrite an existing version folder. Nothing is sent anywhere.

The packages hold only the executable, sce_sys/param.sfo, the LiveArea art, licenses and the compiled-shader pack. A swap replaces
eboot.bin and shadercache/ of the installed test app and leaves everything else (the sce_sys of the first install) alone.
"""
import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
STAGE_TOOL = HERE.parent / "stage-hardware-test.py"
VERSIONS = ("1.3", "1.3p", "1.2", "1.1", "1.0")   # (1.3p: the proposed defaults, branch defaults-proposed; the core session swaps between 1.3 and 1.3p)


def load_stage_tool():
    spec = importlib.util.spec_from_file_location("stage_hardware_test", STAGE_TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def prepare(tool, version, vpk, out_dir, recorded=None):
    members = tool.read_zip(Path(vpk))
    if "eboot.bin" not in members:
        raise ValueError("%s: no eboot.bin in %s" % (version, vpk))
    digest = recorded or tool.sha(members["eboot.bin"])
    info = tool.verify(Path(vpk), digest)          # raises ValueError on a wrong identity, a non-SELF executable or an unapproved member
    dest = Path(out_dir) / version
    if dest.exists():
        raise ValueError("%s already exists; remove it or pick another --out" % dest)
    files = {"eboot.bin": members["eboot.bin"]}
    for name, raw in members.items():
        if name.startswith("shadercache/"):
            files[name] = raw
    if not any(n.startswith("shadercache/") for n in files):
        raise ValueError("%s: the package carries no shader pack" % version)
    manifest = {"version": version, "title_id": info["title_id"], "content_id": info["content_id"], "eboot_sha256": info["eboot_sha256"],
                "check": "eboot matches the recorded digest" if recorded else "structure only (digest taken from the package)",
                "package_sha256": info["package_sha256"], "files": {n: tool.sha(raw) for n, raw in sorted(files.items())}}
    for name, raw in sorted(files.items()):
        path = dest / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
    (dest / "MANIFEST.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    return manifest


def prepare_eboot(tool, name, vpk, out_dir):
    """versions/<name>/eboot.bin alone (session.sh eswap): another build of the same version. The package gets the same check as any other (title, content id, SELF)."""
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name):
        raise ValueError("bad executable name %r" % name)
    members = tool.read_zip(Path(vpk))
    if "eboot.bin" not in members:
        raise ValueError("%s: no eboot.bin in %s" % (name, vpk))
    info = tool.verify(Path(vpk), tool.sha(members["eboot.bin"]))
    dest = Path(out_dir) / name
    if dest.exists():
        raise ValueError("%s already exists; remove it or pick another --out" % dest)
    dest.mkdir(parents=True)
    (dest / "eboot.bin").write_bytes(members["eboot.bin"])
    manifest = {"version": name, "title_id": info["title_id"], "content_id": info["content_id"], "eboot_sha256": info["eboot_sha256"], "check": "executable only (session.sh eswap); structure checked",
                "package_sha256": info["package_sha256"], "files": {"eboot.bin": tool.sha(members["eboot.bin"])}}
    (dest / "MANIFEST.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--eboot", action="append", default=[], metavar="NAME=PATH", help="the executable alone of another build of a version (session.sh eswap NAME), e.g. r15=<vpk> r15pgo=<vpk>")
    ap.add_argument("--out", required=True)
    ap.add_argument("--vpk", action="append", default=[], metavar="VERSION=PATH")
    ap.add_argument("--sha256", action="append", default=[], metavar="VERSION=HEX")
    args = ap.parse_args()
    pairs = dict(item.split("=", 1) for item in args.vpk)
    recorded = dict(item.split("=", 1) for item in args.sha256)
    bad = [v for v in pairs if v not in VERSIONS]
    if (bad or not pairs) and not (args.eboot and not pairs):
        print("need --vpk VERSION=PATH with VERSION one of %s" % ", ".join(VERSIONS), file=sys.stderr)
        return 2
    tool = load_stage_tool()
    status = 0
    for item in args.eboot:
        name, _, vpk = item.partition("=")
        try:
            m = prepare_eboot(tool, name, vpk, args.out)
            print("%s: %s  eboot sha256 %s  executable only" % (name, m["content_id"], m["eboot_sha256"]))
        except (ValueError, OSError) as error:
            print("STOP %s: %s" % (name, error), file=sys.stderr)
            status = 1
    for version in VERSIONS:
        if version not in pairs:
            continue
        try:
            m = prepare(tool, version, pairs[version], args.out, recorded.get(version))
            print("%s: %s  eboot sha256 %s  %d files  [%s]" % (version, m["content_id"], m["eboot_sha256"], len(m["files"]), m["check"]))
        except (ValueError, OSError) as error:
            print("STOP %s: %s" % (version, error), file=sys.stderr)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
