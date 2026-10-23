"""The release files agree with each other: package hashes, manifest, checksums and source inventory."""
import hashlib
import json
from pathlib import Path
import subprocess
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def sha(data):
    return hashlib.sha256(data).hexdigest()


class ReleaseMetadataTests(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads((ROOT / "cboc-release.json").read_text())
        self.release = json.loads((ROOT / "docs/RELEASE.json").read_text())

    def test_manifests_agree(self):
        self.assertEqual(self.release["package"], self.manifest["package"])
        self.assertEqual(self.release["releaseId"], self.manifest["releaseId"])
        self.assertEqual(self.manifest["titleId"], "MSCN00003")
        self.assertEqual(self.release["titleId"], "MSCN00003")
        self.assertEqual(self.manifest["destinations"]["data"], "ux0:data/mscharged/game")

    def test_package_matches_manifest(self):
        package = ROOT / self.manifest["package"]["path"]
        self.assertEqual(sha(package.read_bytes()), self.manifest["package"]["sha256"])
        with zipfile.ZipFile(package) as vpk:
            self.assertIsNone(vpk.testzip())
            names = set(vpk.namelist())
            self.assertEqual(sha(vpk.read("eboot.bin")), self.manifest["package"]["runtimeSha256"])
            for required in ("sce_sys/param.sfo", "shadercache/bc447993.pack", "shadercache/vgx_shaderlist.bin"):
                self.assertIn(required, names)
            # personal artwork and the boot picture are never shipped
            self.assertNotIn("boot/loading.bin", names)
            self.assertNotIn("sce_sys/pic0.png", names)
            self.assertEqual(set(self.release["packageMembersFromPreviousRelease"]),
                             {n for n in names if not n.startswith(("eboot.bin", "licenses/"))})

    def test_previous_release_can_update(self):
        self.assertNotEqual(self.release["previousRelease"]["runtimeSha256"], self.manifest["package"]["runtimeSha256"])
        self.assertTrue(self.manifest["appUpdateFrom"])
        for digest in self.manifest["appUpdateFrom"]:
            self.assertRegex(digest, r"^[0-9a-f]{64}$")

    def test_checksums_cover_tracked_files(self):
        listed = {}
        for line in (ROOT / "SHA256SUMS").read_text().splitlines():
            digest, path = line.split("  ", 1)
            listed[path] = digest
        out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True)
        if out.returncode != 0:
            self.skipTest("not a git checkout")
        files = {p for p in out.stdout.decode().split("\0") if p and p != "SHA256SUMS"}
        self.assertEqual(set(listed), files)
        for path, digest in listed.items():
            with self.subTest(path=path):
                self.assertEqual(sha((ROOT / path).read_bytes()), digest)

    def test_source_manifest_matches_tree(self):
        inventory = json.loads((ROOT / "docs/SOURCE-MANIFEST.json").read_text())
        for item in inventory["files"]:
            with self.subTest(path=item["path"]):
                self.assertEqual(sha((ROOT / item["path"]).read_bytes()), item["sha256"])


if __name__ == "__main__":
    unittest.main()
