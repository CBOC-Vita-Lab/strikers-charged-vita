#!/usr/bin/env python3
"""Prepare an owner's supported disc locally; never connect to or change a Vita.

SPDX-License-Identifier: MIT
Copyright (c) 2026 CBOC-Vita contributors. See CHECK-FILES-LICENSE.txt.
"""

import argparse
import hashlib
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile
import urllib.request

RELEASE = "https://github.com/encounter/nod/releases/download/v2.0.0-alpha.12/"
GAME_SHA256 = "8f811628ee6646c60620a9c599ef08ed929d2eccc98a779c211847f1500f81fd"
ASSETS = {
    ("Darwin", "arm64"): ("nodtool-macos-arm64", "524c0b27c0c4289e435df4474d288ea7e63d6a34814cc7c650231688dd1e53b4"),
    ("Darwin", "x86_64"): ("nodtool-macos-x86_64", "d1e7ba904c2e557fafbd3bf2d9b75a7901a2bf1568c0fb3a505fca0f30b0a833"),
    ("Windows", "arm64"): ("nodtool-windows-arm64.exe", "463d5f7fd7d753fde05c6772f8114ec6f8ab6eaf054790ed9b95a89ef8c63526"),
    ("Windows", "x86_64"): ("nodtool-windows-x86_64.exe", "f65de62c44f0ad5d179eff85bd1e92099713ffacbf717f651fb761129a25c7bc"),
    ("Linux", "arm64"): ("nodtool-linux-aarch64", "33e801c0e4aee7e714ad84e89521f50e2f79005571b9d4779b695e44ea21ff99"),
    ("Linux", "x86_64"): ("nodtool-linux-x86_64", "ee8345c004248eb82a86dd0c6068e0fcf3dc75baf68d18fe949bf4589822f5a5"),
}


def tool_asset(system, machine):
    machine = {"amd64": "x86_64", "aarch64": "arm64"}.get(machine.lower(), machine.lower())
    try:
        return ASSETS[(system, machine)]
    except KeyError:
        raise ValueError(f"Unsupported computer: {system} {machine}. See PREPARE-GAME.md.") from None


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def obtain_tool(cache, asset):
    name, expected = asset
    if cache.is_symlink():
        raise ValueError("The tool cache must not be a symlink.")
    cache.mkdir(parents=True, exist_ok=True)
    tool = cache / name
    if tool.is_symlink():
        raise ValueError("The extractor must not be a symlink.")
    if not tool.exists():
        print("Downloading the extractor from its official GitHub release...", flush=True)
        temporary = None
        try:
            with urllib.request.urlopen(RELEASE + name, timeout=60) as response:
                with tempfile.NamedTemporaryFile(dir=cache, delete=False) as handle:
                    temporary = Path(handle.name)
                    total = 0
                    while chunk := response.read(1024 * 1024):
                        total += len(chunk)
                        if total > 20 * 1024 * 1024:
                            raise ValueError("Unexpected extractor download size.")
                        handle.write(chunk)
            if sha256(temporary) != expected:
                raise ValueError("Extractor checksum mismatch. Nothing was run.")
            # Exclusive creation also protects an existing file if another run wins.
            with tool.open("xb") as output, temporary.open("rb") as source:
                output.write(source.read())
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    if sha256(tool) != expected:
        raise ValueError("Extractor checksum mismatch. Remove only .setup-tools and retry.")
    if os.name != "nt":
        tool.chmod(tool.stat().st_mode | 0o100)
    return tool.resolve()


def check_disc_info(info):
    # Partition boot headers can have a different revision; check the outer disc.
    header = re.split(r"(?m)^Partition\s+\d+", info, maxsplit=1)[0]
    if not re.search(r"(?m)^Game ID: R4QE01\s*$", header) or not re.search(
        r"(?m)^Disc 1, Revision 1\s*$", header
    ):
        raise ValueError("Use your own Wii USA Rev 1 (R4QE01) dump. This is a different disc/revision.")


def check_game(root):
    dol = root / "sys/main.dol"
    if not dol.is_file() or sha256(dol) != GAME_SHA256:
        raise ValueError("main.dol does not match the supported game. Do not copy this extraction.")
    if not (root / "sys/fst.bin").is_file() or not (root / "files/Art").is_dir():
        raise ValueError("The extraction is incomplete: sys/fst.bin or files/Art is missing.")


def prepare(disc, output, tool):
    if not disc.is_file():
        raise FileNotFoundError(f"Disc image not found: {disc}")
    if output.exists() or output.is_symlink():
        raise FileExistsError(f"Choose a new output folder; existing files will not be replaced: {output}")
    info = subprocess.run([str(tool), "info", str(disc.resolve())], check=True,
                          capture_output=True, text=True)
    check_disc_info(info.stdout)
    output.mkdir(parents=True)
    print("Supported disc found. Extracting and checking game data...", flush=True)
    subprocess.run([str(tool), "extract", "--quiet", "--validate", "--partition", "data",
                    str(disc.resolve()), str(output.resolve())], check=True)
    check_game(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("disc", type=Path, help="Your Wii USA Rev 1 ISO or RVZ")
    parser.add_argument("--output", type=Path, default=Path("prepared-game"), help="New output folder")
    args = parser.parse_args()
    try:
        if not args.disc.is_file():
            raise FileNotFoundError(f"Disc image not found: {args.disc}")
        if args.output.exists() or args.output.is_symlink():
            raise FileExistsError("Output already exists. Choose a new folder with --output.")
        asset = tool_asset(platform.system(), platform.machine())
        tool = obtain_tool(Path(__file__).resolve().parent / ".setup-tools", asset)
        prepare(args.disc, args.output, tool)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"STOP: {error}", file=sys.stderr)
        print("No Vita files were changed. Fix the error and use a new output folder.", file=sys.stderr)
        return 1
    print(f"READY: {args.output.resolve()}")
    print("Copy only its sys and files folders into ux0:data/mscharged/game/.")
    print("Then install the matching player VPK with VitaShell. See INSTALL.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
