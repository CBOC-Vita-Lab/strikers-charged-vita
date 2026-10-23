#!/usr/bin/env python3
"""Regenerate SHA256SUMS (every tracked file but itself) and SOURCE-MANIFEST.json (native/, renderer/,
dependencies/). Run from the repository root after `git add`, before committing a release.

    python3 tools/update-checksums.py [YYYY-MM-DD]
"""
import datetime
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = ("native/", "renderer/", "dependencies/")


def tracked():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, check=True, capture_output=True).stdout
    return sorted(p for p in out.decode().split("\0") if p and p != "SHA256SUMS" and (ROOT / p).is_file())


def sha(path):
    return hashlib.sha256((ROOT / path).read_bytes()).hexdigest()


def main():
    date = sys.argv[1] if len(sys.argv) > 1 else datetime.date.today().isoformat()
    files = tracked()
    manifest = {"date": date, "scope": "Source of the release in RELEASE.json (native/, renderer/, dependencies/)",
                "files": [{"path": p, "sha256": sha(p)} for p in files if p.startswith(SOURCE_DIRS)]}
    (ROOT / "SOURCE-MANIFEST.json").write_text(json.dumps(manifest, indent=1) + "\n")
    digests = {p: sha(p) for p in files}   # after SOURCE-MANIFEST.json was rewritten: SHA256SUMS covers it too
    (ROOT / "SHA256SUMS").write_text("".join(f"{digests[p]}  {p}\n" for p in files))
    print(f"SHA256SUMS: {len(files)} files; SOURCE-MANIFEST.json: {len(manifest['files'])} files")


if __name__ == "__main__":
    main()
