#!/usr/bin/env python3
"""Prepare an owner's supported disc locally; never connect to or change a Vita.

SPDX-License-Identifier: MIT
Copyright (c) 2026 CBOC-Vita contributors. See licenses/CBOC-tools-MIT.txt.
"""

import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import sys
import tempfile
import urllib.request

RELEASE = "https://github.com/encounter/nod/releases/download/v2.0.0-alpha.12/"
GAME_SHA256 = "8f811628ee6646c60620a9c599ef08ed929d2eccc98a779c211847f1500f81fd"
FST_SHA256 = "c987b8423ec17eb6fcd6f0dce669589d2a2a48b17fa2180adf4be8c2690461e0"
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
        raise ValueError(f"Unsupported computer: {system} {machine}. See MANUAL-PREPARE-GAME.md.") from None


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


def check_file_table(root):
    fst = root / "sys/fst.bin"
    if not fst.is_file() or not 12 <= fst.stat().st_size <= 16 * 1024 * 1024:
        raise ValueError("Invalid sys/fst.bin: missing, truncated or oversized file table.")
    raw = fst.read_bytes()
    kind, parent, count = struct.unpack_from(">III", raw)
    if kind != 0x01000000 or parent != 0 or count < 2 or count * 12 > len(raw):
        raise ValueError("Invalid sys/fst.bin: malformed root or entry count.")
    names = raw[count * 12:]
    stack = [(0, count, Path(), root / "files")]
    directories = {}
    extents = []
    for index in range(1, count):
        while index >= stack[-1][1]:
            stack.pop()
        word, offset, size = struct.unpack_from(">III", raw, index * 12)
        name_offset = word & 0xffffff
        end = names.find(b"\0", name_offset)
        if word >> 24 not in (0, 1) or end < 0:
            raise ValueError(f"Invalid sys/fst.bin: bad name or type at entry {index}.")
        name = os.fsdecode(names[name_offset:end])
        if not name or name in (".", "..") or any(c in name for c in "/\\:"):
            raise ValueError(f"Invalid sys/fst.bin: unsafe name at entry {index}.")
        parent_index, parent_end, relative, host = stack[-1]
        relative = relative / name
        is_directory = word >> 24 == 1
        if is_directory and (offset != parent_index or not index < size <= parent_end):
            raise ValueError(f"Invalid sys/fst.bin: bad directory range at entry {index}.")
        path = host / name
        # Extractors may omit empty disc directories (the retail GameLog entry
        # is one). They contain no game bytes to transfer or validate.
        if is_directory and size == index + 1 and not path.exists():
            continue
        # The runtime tolerates letter-case differences too. List each affected
        # directory once rather than searching the tree separately for each file.
        if not path.exists():
            if host not in directories:
                directories[host] = list(host.iterdir())
            matches = [p for p in directories[host] if p.name.casefold() == name.casefold()]
            if len(matches) != 1:
                raise ValueError(f"The extraction is incomplete: {relative.as_posix()} is missing or ambiguous.")
            path = matches[0]
        if path.is_symlink():
            raise ValueError(f"The extraction contains a link instead of game data: {relative.as_posix()}.")
        if is_directory:
            if not path.is_dir():
                raise ValueError(f"The extraction is incomplete: {relative.as_posix()} is not a directory.")
            stack.append((index, size, relative, path))
        elif not path.is_file() or path.stat().st_size != size:
            raise ValueError(f"The extraction is incomplete: {relative.as_posix()} must be a regular file of {size} bytes.")
        elif size:
            end_word = offset + (size + 3) // 4
            if size > 0xfffffffc or end_word > 0xffffffff:
                raise ValueError(f"Invalid sys/fst.bin: wrapped disc extent for {relative.as_posix()}.")
            extents.append((offset, end_word, relative))
    previous_end = 0
    for start, end, relative in sorted(extents):
        if start < previous_end:
            raise ValueError(f"Invalid sys/fst.bin: overlapping disc extent for {relative.as_posix()}.")
        previous_end = end


def check_game(root):
    for relative in ("sys", "files", "sys/main.dol", "sys/fst.bin"):
        if (root / relative).is_symlink():
            raise ValueError(f"The extraction contains a link instead of game data: {relative}.")
    dol = root / "sys/main.dol"
    if not dol.is_file() or sha256(dol) != GAME_SHA256:
        raise ValueError("main.dol does not match the supported game. Do not copy this extraction.")
    if not (root / "sys/fst.bin").is_file() or not (root / "files").is_dir():
        raise ValueError("The extraction is incomplete: sys/fst.bin or the files directory is missing or invalid.")
    if sha256(root / "sys/fst.bin") != FST_SHA256:
        raise ValueError("fst.bin does not match the supported game. Do not copy this extraction.")
    check_file_table(root)


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


BRANDED_NAME = "Strikers-Charged-Vita-with-game-art.vpk"


def brand_package(output, package):
    """Make a copy of the player package whose home-screen bubble, LiveArea pages and loading picture are your game's own art.

    The pictures are made on this computer from the disc you just prepared (tools/brand-from-disc.py); nothing is downloaded and
    the original package is not changed. Never stops the preparation: on any problem it says why and the plain package still works."""
    try:
        if not package.is_file():
            raise ValueError(f"{package.name} is not next to this tool")
        script = Path(__file__).resolve().parent / "tools" / "brand-from-disc.py"
        spec = importlib.util.spec_from_file_location("brand_from_disc", script)
        brand = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(brand)
        files, _ = brand.build(str(output))
        target = output / BRANDED_NAME
        brand.apply_to_vpk(files, str(package), str(target))
        return target
    except Exception as error:  # pictures are optional; the game is prepared either way
        print(f"Note: the package with your game's art was not made ({error}).", file=sys.stderr)
        print("The plain package works the same; it only has the generic icon and pictures.", file=sys.stderr)
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("disc", type=Path, help="Your Wii USA Rev 1 ISO or RVZ")
    parser.add_argument("--output", type=Path, default=Path("prepared-game"), help="New output folder")
    parser.add_argument("--no-art", action="store_true",
                        help="Do not make the copy of the package that uses your game's own art for the home screen")
    parser.add_argument("--package", type=Path, default=Path(__file__).resolve().parent / "Strikers-Charged-Vita.vpk",
                        help="The player package to copy with your game's art (default: the one next to this tool)")
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
    branded = None if args.no_art else brand_package(args.output, args.package)
    if branded:
        print(f"Your own package, with your game's art on the home screen: {branded.resolve()}")
        print("Install that one with VitaShell instead of the plain player VPK. See MANUAL-INSTALL.md.")
    else:
        print("Then install the matching player VPK with VitaShell. See MANUAL-INSTALL.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
