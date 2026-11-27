import hashlib
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReleaseResourceTests(unittest.TestCase):
    def test_original_package_art_matches_inventory(self):
        manifest = ROOT / "store/artwork.json"
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
            "GPL-3.0.txt": ["GNU GENERAL PUBLIC LICENSE", "Version 3"],
            "LGPL-2.1.txt": ["GNU LESSER GENERAL PUBLIC LICENSE", "Version 2.1"],
            "GCC-runtime-exception.txt": ["GCC RUNTIME LIBRARY EXCEPTION", "Version 3.1", "No Weakening of GCC Copyleft"],
            "vitaGL-bundled-notices.txt": ["Harm Hanemaaijer", "Chukong Technologies", "Permission is hereby granted"],
        }
        for name, phrases in expected.items():
            path = ROOT / "licenses" / name
            with self.subTest(notice=name):
                self.assertTrue(path.is_file(), name + " is missing")
                for phrase in phrases:
                    self.assertIn(phrase, path.read_text())

    def test_notices_name_every_static_library_with_its_terms(self):
        text = (ROOT / "THIRD_PARTY_NOTICES.md").read_text()
        rows = {line.split("|")[1].strip(): line for line in text.splitlines() if line.startswith("| ")}
        self.assertIn("LGPL", rows["vitaGL"])
        self.assertNotIn("GPL-3.0, text in", rows["vitaGL"])
        self.assertIn("LGPL-3.0", rows["vitaShaRK"])
        self.assertIn("GPL-3.0", rows["SceShaccCgExt"])
        for library in ("libSceShaccCgExt.a", "libvitashark.a", "libvitaGL.a", "libmathneon.a", "libpthread.a", "libstdc++.a", "libgcc.a", "libc.a", "libm.a"):
            self.assertIn(library, text)
        for section in ("What a release must ship", "Source route", "Relinking route", "For the owner to decide"):
            self.assertIn(section, text)


if __name__ == "__main__":
    unittest.main()
