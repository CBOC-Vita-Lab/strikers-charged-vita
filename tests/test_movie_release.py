import hashlib
import json
from pathlib import Path
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def sha(data):
    return hashlib.sha256(data).hexdigest()


class MovieReleaseTests(unittest.TestCase):
    def test_release_source_matches_inventory(self):
        release = json.loads((ROOT / "RELEASE.json").read_text())
        inventory = json.loads((ROOT / "SOURCE-MANIFEST.json").read_text())
        entries = {item["path"]: item["sha256"] for item in inventory["files"]}
        self.assertEqual(len(release["sourceFiles"]), 17)
        self.assertEqual(release["buildOptions"], {"NP_PLAYER": 1, "NP_LOGGING": 0})
        self.assertEqual(sum(hashes["before"] is None for hashes in release["sourceFiles"].values()), 3)
        for path, hashes in release["sourceFiles"].items():
            with self.subTest(path=path):
                self.assertNotEqual(hashes["before"], hashes["after"])
                self.assertEqual(entries[path], hashes["after"])
                self.assertEqual(sha((ROOT / path).read_bytes()), hashes["after"])

    def test_package_matches_selected_production_runtime(self):
        release = json.loads((ROOT / "RELEASE.json").read_text())
        manifest = json.loads((ROOT / "cboc-release.json").read_text())
        self.assertEqual(release["package"], manifest["package"])
        self.assertEqual(manifest["titleId"], "MSCN00003")
        self.assertEqual(manifest["visibility"], "private")
        package = ROOT / manifest["package"]["path"]
        self.assertEqual(sha(package.read_bytes()), manifest["package"]["sha256"])
        with zipfile.ZipFile(package) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(sha(archive.read("eboot.bin")), manifest["package"]["runtimeSha256"])
            expected = release["unchangedPackageMembers"]
            self.assertEqual(set(archive.namelist()), set(expected) | {"eboot.bin"})
            for name, digest in expected.items():
                self.assertEqual(sha(archive.read(name)), digest, name)
            self.assertNotIn("boot/loading.bin", archive.namelist())
            self.assertNotIn("sce_sys/pic0.png", archive.namelist())

    def test_hardware_evidence_does_not_claim_full_installer_acceptance(self):
        release = json.loads((ROOT / "RELEASE.json").read_text())
        evidence = release["deviceVerification"]
        self.assertEqual(evidence["runtimeSha256"], release["package"]["runtimeSha256"])
        self.assertEqual(evidence["protectedFilesUnchanged"], 42)
        self.assertIs(evidence["physicalGameplayTested"], False)
        self.assertIs(evidence["fullVpkInstallTested"], False)
        self.assertIs(evidence["desktopInstallTested"], False)
        self.assertIs(release["publicReleaseAccepted"], False)
