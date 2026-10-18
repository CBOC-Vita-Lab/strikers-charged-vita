from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReleaseResourceTests(unittest.TestCase):
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
