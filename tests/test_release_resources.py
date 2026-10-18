import hashlib
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReleaseResourceTests(unittest.TestCase):
    def test_original_package_art_matches_inventory(self):
        manifest = ROOT / "ARTWORK.json"
        self.assertTrue(manifest.is_file(), "Original project-art inventory is missing")
        entries = json.loads(manifest.read_text())["files"]
        required = {"icon0.png", "icon0-test.png", "livearea/bg.png", "livearea/startup.png",
                    "livearea/settings.png", "art/mp-icon0.png", "art/mp-bg.png", "art/mp-startup.png"}
        self.assertEqual({entry["path"] for entry in entries}, required)
        for entry in entries:
            with self.subTest(path=entry["path"]):
                file = ROOT / "native/platform/vita/vpk" / entry["path"]
                self.assertTrue(file.is_file())
                self.assertEqual(hashlib.sha256(file.read_bytes()).hexdigest(), entry["sha256"])

    def test_fonts_and_linked_dependencies_have_notices(self):
        expected = {
            "DejaVu.txt": ["Bitstream Vera Fonts Copyright", "Arev Fonts Copyright"],
            "LGPL-3.0.txt": ["GNU LESSER GENERAL PUBLIC LICENSE", "Version 3"],
            "taiHEN-MIT.txt": ["Copyright (c) 2016 Yifan Lu", "Permission is hereby granted"],
            "math-neon-MIT.txt": ["Lachlan Tychsen-Smith", "Permission is hereby granted"],
            "zlib.txt": ["1995-2004 Jean-loup Gailly and Mark Adler", "This notice may not be removed"],
        }
        for name, phrases in expected.items():
            path = ROOT / "licenses" / name
            with self.subTest(notice=name):
                self.assertTrue(path.is_file(), name + " is missing")
                for phrase in phrases:
                    self.assertIn(phrase, path.read_text())


if __name__ == "__main__":
    unittest.main()
