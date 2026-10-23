#!/usr/bin/env python3
"""Fetch pinned dependency source and build the native player's Vita libraries.

SPDX-License-Identifier: MIT
Copyright (c) 2026 CBOC-Vita contributors. See licenses/CBOC-tools-MIT.txt.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def git(directory, *args):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    return subprocess.check_output(["git", "-c", "core.hooksPath=" + os.devnull, *args],
                                   cwd=directory, env=env, text=True).strip()


def load_lock(path):
    rows = json.loads(path.read_text())["dependencies"]
    seen = set()
    if not rows:
        raise ValueError("Dependency lock is empty")
    for row in rows:
        name, url, commit = row["name"], row["url"], row["commit"]
        if (not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", name) or name.lower() in seen
                or not re.fullmatch(r"https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\.git", url)
                or not re.fullmatch(r"[0-9a-f]{40}", commit)):
            raise ValueError("Invalid or unpinned dependency lock entry")
        seen.add(name.lower())
    return rows


def ensure_checkout(destination, url, commit):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("A full pinned revision is required")
    if destination.is_symlink():
        raise ValueError("Dependency checkout must not be a symlink")
    if destination.exists():
        if not (destination / ".git").is_dir() or (destination / ".git").is_symlink():
            raise ValueError("Existing dependency directory is not a managed Git checkout")
        if git(destination, "remote", "get-url", "origin") != url:
            raise ValueError("Dependency origin differs; existing files were left alone")
        if git(destination, "rev-parse", "HEAD") != commit:
            raise ValueError("Dependency revision differs; existing files were left alone")
        if git(destination, "status", "--porcelain", "--untracked-files=all"):
            raise ValueError("Dependency has local changes; existing files were left alone")
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Only promote a verified checkout; failed downloads remain safe to retry.
    with tempfile.TemporaryDirectory(prefix="." + destination.name + "-fetch-", dir=destination.parent) as temp:
        checkout = Path(temp) / "checkout"
        checkout.mkdir()
        git(checkout, "init", "--quiet")
        git(checkout, "remote", "add", "origin", url)
        git(checkout, "fetch", "--quiet", "--depth=1", "origin", commit)
        git(checkout, "checkout", "--quiet", "--detach", commit)
        if git(checkout, "rev-parse", "HEAD") != commit:
            raise ValueError("Fetched dependency revision did not match")
        if destination.exists() or destination.is_symlink():
            raise ValueError("Dependency destination appeared during download; existing files were left alone")
        checkout.rename(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fetch-only", action="store_true", help="Prepare sources without compiling")
    args = parser.parse_args()
    work = ROOT / "build/dependencies"
    try:
        for folder in (ROOT / "build", work, work / "sources", work / "prefix"):
            if folder.is_symlink():
                raise ValueError("Build folders must not be symlinks")
        sdk = Path(os.environ.get("VITASDK", "/opt/vitasdk"))
        if not args.fetch_only:
            if platform.system() != "Linux" or any(c.isspace() for c in str(ROOT) + str(sdk)):
                raise ValueError("Build on Linux in paths without spaces; see docs/BUILDING.md. --fetch-only works on other hosts.")
            if not (sdk / "bin/arm-vita-eabi-gcc").is_file():
                raise ValueError("Set VITASDK to your Linux VitaSDK directory")
        rows = load_lock(ROOT / "dependencies/lock.json")
        for row in rows:
            print("Checking " + row["name"], flush=True)
            ensure_checkout(work / "sources" / row["name"], row["url"], row["commit"])
        if args.fetch_only:
            print("Pinned source is ready in build/dependencies/sources/.")
            return 0
        env = dict(os.environ, VITASDK=str(sdk), PATH=str(sdk / "bin") + os.pathsep + os.environ.get("PATH", ""))
        for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL"):
            env.pop(key, None)
        # The recovered recipe lacks full header/config tracking. Always rebuild
        # its small library set so a receipt never certifies stale objects.
        subprocess.run(["make", "-B", "-f", str(ROOT / "dependencies/Makefile.vitadeps"), "-j2",
                        "REFS=" + str(work / "sources"), "DEPS_OUT=" + str(work / "prefix"), "all"],
                       cwd=ROOT, env=env, check=True)
        libraries = ["libvitaGL.a", "libvitashark.a", "libmathneon.a", "libSceShaccCgExt.a", "libtaihen_stub.a"]
        receipt = {"dependencies": rows, "libraries": {
            name: hashlib.sha256((work / "prefix/lib" / name).read_bytes()).hexdigest() for name in libraries}}
        (work / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
        print("READY: build/dependencies/prefix/ (libraries and headers)")
        return 0
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print("STOP: " + str(error), file=sys.stderr)
        print("No existing dependency checkout was reset or removed.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
