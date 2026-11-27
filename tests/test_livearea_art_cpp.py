"""vita_livearea_art.cpp (on-device LiveArea & Home-screen bubble generator) tested on synthetic game files.

No game art anywhere: the synthetic disc is generated from a seeded PRNG in the exact container formats the game uses
(BundleFile GX textures, Wii opening.bnr with IMD5 + LZ77 + nested U8 + TPL). Verifies:
  1. Both the full art33 stadium/portrait pipeline and the minimal fallback (no superabilityui.res or heading font)
     produce valid SceShell-compatible 8-bit palette-indexed PNGs (mode P, <= 256 colors) at exact Vita dimensions
     (icon0 128x128, bg 840x500, startup 280x158, pic0 960x544) with zero ASan/UBSan errors.
  2. Missing, truncated, or corrupted inputs fail cleanly without crashes or sanitizer errors.
  3. IsGenericIconBytes matches the shipped generic CBOC-Vita icon0.png / icon0-test.png and rejects generated art.
"""
import importlib.util
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VITA = ROOT / "native" / "platform" / "vita"
THIRD_PARTY = ROOT / "renderer" / "third_party"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


brand = load("brand_from_disc", ROOT / "tools" / "brand-from-disc.py")
helpers = load("test_brand_from_disc", ROOT / "tests" / "test_brand_from_disc.py")
bootart = load("test_bootart_cpp", ROOT / "tests" / "test_bootart_cpp.py")
HAVE_PILLOW = brand.Image is not None
CXX = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")

EXPECTED_SIZES = {
    "icon0.png": (128, 128),
    "bg.png": (840, 500),
    "startup.png": (280, 158),
    "pic0.png": (960, 544),
}


class LiveAreaArtCppTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = os.path.join(cls.tmp.name, "tla")
        cls.have_tool = CXX is not None and HAVE_PILLOW
        if cls.have_tool:
            cmd = [
                CXX, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                "-I", str(VITA / "src"),
                "-I", str(THIRD_PARTY),
                str(VITA / "tests" / "test_livearea_art.cpp"),
                str(VITA / "src" / "vita_livearea_art.cpp"),
                "-o", cls.exe,
            ]
            subprocess.run(cmd, check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        if not self.have_tool:
            self.skipTest("needs a C++17 compiler and Pillow")

    def make_synthetic_disc(self, name, with_heroes=True, seed=11):
        files = Path(self.tmp.name) / name / "files"
        (files / "Art" / "fe").mkdir(parents=True)
        rng = random.Random(seed)
        logo_tpl = bootart.rgba8_logo(rng, 96, 56)
        sky_tpl = bootart.tpl_image(4, 64, 48, bootart.random_texels(rng, 4, 64, 48))
        inner = bootart.u8_tree({
            "arc": {
                "timg": {
                    "logo_UK_Banner.tpl": logo_tpl,
                    "sky_banner.tpl": sky_tpl,
                }
            }
        })
        member = b"IMD5" + b"\0" * 28 + bootart.lz77_matches(inner)
        banner = b"\0" * 0x600 + bootart.u8_tree({"meta": {"banner.bin": member, "icon.bin": b"x" * 32}})
        (files / "opening.bnr").write_bytes(banner)

        dmn_entries = [(brand.KEY_ART_HASH, bootart.cmpr_art(rng, 256, 256))]
        if with_heroes:
            for h, w, ht in [
                (0x944D752D, 128, 128),  # ball
                (0x250BBFA5, 160, 160),  # sunburst
                (0x23B38F66, 96, 96),    # iceburst
                (0x9CF58C6E, 64, 32),    # boltA
                (0x292D15FB, 64, 32),    # boltB
                (0x14083241, 64, 64),    # boltC
                (0x2FD2A2E6, 64, 32),    # sparks
            ]:
                dmn_entries.append((h, bootart.cmpr_art(rng, w, ht)))
            sup_entries = [
                (0x886F2DF4, bootart.cmpr_art(rng, 128, 128)),  # Mario
                (0x6C217D2E, bootart.cmpr_art(rng, 128, 128)),  # Bowser
            ]
            (files / "Art" / "fe" / "superabilityui.res").write_bytes(helpers.bundle(sup_entries))
        (files / "Art" / "fe" / "mainui.dmn").write_bytes(helpers.bundle(dmn_entries))
        return files

    def check_valid_pngs(self, out_dir):
        Image = brand.Image
        for filename, want_size in EXPECTED_SIZES.items():
            path = out_dir / filename
            self.assertTrue(path.is_file(), f"missing {filename}")
            with Image.open(path) as im:
                im.verify()
            with Image.open(path) as im:
                self.assertEqual(im.mode, "P", f"{filename} must be 8-bit indexed mode P for SceShell")
                self.assertEqual(im.size, want_size, f"{filename} wrong dimensions")
                pal = im.getpalette()
                self.assertIsNotNone(pal)
                self.assertLessEqual(len(pal) // 3, 256)
                colors = im.getcolors(maxcolors=512)
                self.assertIsNotNone(colors)
                self.assertGreaterEqual(len(colors), 8, f"{filename} looks blank/degenerate")

    def test_full_art33_pipeline_produces_valid_indexed_pngs(self):
        files = self.make_synthetic_disc("full", with_heroes=True, seed=17)
        out = Path(self.tmp.name) / "out-full"
        out.mkdir()
        done = subprocess.run([self.exe, str(files), str(out)], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertNotIn("ERROR: AddressSanitizer", done.stderr)
        self.assertNotIn("runtime error", done.stderr)
        self.check_valid_pngs(out)

    def test_fallback_without_heroes_or_font_produces_valid_indexed_pngs(self):
        files = self.make_synthetic_disc("minimal", with_heroes=False, seed=23)
        out = Path(self.tmp.name) / "out-minimal"
        out.mkdir()
        done = subprocess.run([self.exe, str(files), str(out)], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.check_valid_pngs(out)

    def test_generic_icon_identity_guard(self):
        for rel in ("icon0.png", "icon0-test.png"):
            p = VITA / "vpk" / rel
            done = subprocess.run([self.exe, "--check-generic", str(p)], capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, f"{rel} should match generic guard: {done.stdout}{done.stderr}")
            self.assertEqual(done.stdout.strip(), "GENERIC")
        # Generated icon must NOT trigger the generic guard (so custom/branded art is never overwritten)
        files = self.make_synthetic_disc("guard", with_heroes=True, seed=31)
        out = Path(self.tmp.name) / "out-guard"
        out.mkdir()
        subprocess.run([self.exe, str(files), str(out)], check=True, capture_output=True)
        done = subprocess.run([self.exe, "--check-generic", str(out / "icon0.png")], capture_output=True, text=True)
        self.assertEqual(done.returncode, 1)
        self.assertEqual(done.stdout.strip(), "CUSTOM")

    def test_damaged_or_missing_disc_fails_cleanly(self):
        empty = Path(self.tmp.name) / "empty-la"
        out = Path(self.tmp.name) / "out-empty-la"
        empty.mkdir()
        out.mkdir()
        done = subprocess.run([self.exe, str(empty), str(out)], capture_output=True, text=True)
        self.assertEqual(done.returncode, 1)
        self.assertNotIn("ERROR: AddressSanitizer", done.stderr)
        self.assertNotIn("runtime error", done.stderr)

        files = self.make_synthetic_disc("dmg", with_heroes=False, seed=41)
        (files / "opening.bnr").write_bytes(b"\0" * 128)
        done2 = subprocess.run([self.exe, str(files), str(out)], capture_output=True, text=True)
        self.assertEqual(done2.returncode, 1)
        self.assertNotIn("ERROR: AddressSanitizer", done2.stderr)
        self.assertNotIn("runtime error", done2.stderr)


if __name__ == "__main__":
    unittest.main()
