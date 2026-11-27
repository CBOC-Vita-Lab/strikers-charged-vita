"""vita_bootart.cpp (the console's own loading picture) against tools/brand-from-disc.py, on made-up game files.

No game art anywhere: the "disc" is made here from a seeded random generator, in the containers the real files use (a bundle of GX textures
with a CMPR picture, a banner with IMD5 + LZ77 + nested U8 + a TPL logo). The C++ build must give the Python tool's boot/loading.bin byte for
byte, for logos of every texture format, and must fail cleanly (no crash) on files that are missing, cut short or damaged.
"""
import importlib.util
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VITA = ROOT / "native" / "platform" / "vita"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


brand = load("brand_from_disc", ROOT / "tools" / "brand-from-disc.py")
helpers = load("test_brand_from_disc", ROOT / "tests" / "test_brand_from_disc.py")   # bundle(), texture(), u8() ...
HAVE_PILLOW = brand.Image is not None
CXX = shutil.which("g++") or shutil.which("c++")


def u8_tree(tree):
    """A U8 archive with real directories: tree = {name: bytes | dict}."""
    nodes, names = [], b"\0"
    blobs = []

    def walk(items, depth):
        nonlocal names
        for name, value in items.items():
            at = len(names)
            names += name.encode() + b"\0"
            if isinstance(value, dict):
                idx = len(nodes) + 1
                nodes.append([1, at, 0, None])
                walk(value, depth + 1)
                nodes[idx - 1][2] = len(nodes) + 1
            else:
                nodes.append([0, at, 0, value])
                blobs.append(value)

    walk(tree, 0)
    count = len(nodes) + 1
    header_size = count * 12 + len(names)
    data_at = (0x20 + header_size + 0x1F) // 0x20 * 0x20
    body, at, rows = b"", data_at, [struct.pack(">III", 0x01000000, 0, count)]
    for kind, name_at, end, blob in nodes:
        if kind:
            rows.append(struct.pack(">III", 0x01000000 | name_at, 0, end))
        else:
            rows.append(struct.pack(">III", name_at, at, len(blob)))
            body += blob + b"\0" * (-len(blob) % 0x20)
            at += len(blob) + (-len(blob) % 0x20)
    head = b"U\xaa8-" + struct.pack(">III", 0x20, header_size, data_at) + b"\0" * 16
    return head + b"".join(rows) + names + b"\0" * (data_at - 0x20 - header_size) + body


def lz77_matches(blob):
    """Nintendo LZ77 0x10 with real back references (a greedy search; the made-up logos are small)."""
    out, i, n = bytearray(b"LZ77\x10" + len(blob).to_bytes(3, "little")), 0, len(blob)
    while i < n:
        flags, chunk = 0, bytearray()
        for k in range(8):
            if i >= n:
                break
            best_len, best_disp = 0, 0
            for disp in range(1, min(i, 4096) + 1):
                length = 0
                while length < 18 and i + length < n and blob[i + length - disp] == blob[i + length]:
                    length += 1
                if length > best_len:
                    best_len, best_disp = length, disp
            if best_len >= 3:
                flags |= 0x80 >> k
                chunk += bytes((((best_len - 3) << 4) | ((best_disp - 1) >> 8), (best_disp - 1) & 255))
                i += best_len
            else:
                chunk.append(blob[i])
                i += 1
        out.append(flags)
        out += chunk
    return bytes(out)


def tpl_image(fmt, w, h, texels, palette=None, pal_fmt=2):
    """A TPL with one image of the given GX format (the image header at 0x20, the texels at 0x80, the palette after them)."""
    header = struct.pack(">III", 0x0020AF30, 1, 12) + struct.pack(">II", 20, 0x60 if palette else 0)
    image = struct.pack(">HHII", h, w, fmt, 0x80) + b"\0" * 20
    out = header + image
    if palette:
        out += b"\0" * (0x60 - len(out)) + struct.pack(">HHII", len(palette) // 2, 0, pal_fmt, 0x80 + len(texels))
    out += b"\0" * (0x80 - len(out)) + texels
    if palette:
        out += palette
    return out


def rgba8_logo(rng, w, h):
    """An RGBA8 logo of a soft blob (so the trim box is smaller than the image) with a repeating pattern."""
    tiles = b""
    for ty in range(0, h, 4):
        for tx in range(0, w, 4):
            ar, gb = b"", b""
            for i in range(16):
                x, y = tx + i % 4, ty + i // 4
                d = ((x - w / 2) / (w / 3)) ** 2 + ((y - h / 2) / (h / 3)) ** 2
                a = 0 if d > 1 else int(255 * (1 - d) ** 0.5) if d > 0.6 else 255
                ar += bytes((a, (x * 7 + y * 3) & 255))
                gb += bytes(((x * 5) & 255, (y * 11 + 40) & 255))
            tiles += ar + gb
    return tpl_image(6, w, h, tiles)


def random_texels(rng, fmt, w, h):
    bw, bh, bpp = brand._BLOCK[fmt]
    n = ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bw * bh * bpp // 8
    data = bytearray(rng.getrandbits(8) for _ in range(n))
    if fmt == 14:   # keep both CMPR branches (c0 > c1 and c0 <= c1), whatever the random bytes say
        pass
    return bytes(data)


def cmpr_art(rng, w=512, h=512):
    """A made-up 512 x 512 CMPR picture: smooth gradients with noise, so that the resampling and blur have something to do."""
    blocks = b""
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for sy in range(0, 8, 4):
                for sx in range(0, 8, 4):
                    x, y = tx + sx, ty + sy
                    r, g, b = (x * 31 // w) & 31, (y * 63 // h) & 63, ((x + y) * 31 // (w + h)) & 31
                    c0 = (r << 11) | (g << 5) | b
                    c1 = ((r ^ 9) << 11) | (((g + 17) & 63) << 5) | ((b + 5) & 31)
                    blocks += struct.pack(">HHI", c0, c1, rng.getrandbits(32))
    return helpers.texture(2, w, h, blocks)


class BootArtCppTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = os.path.join(cls.tmp.name, "tba")
        cls.have_tool = CXX is not None and HAVE_PILLOW
        if cls.have_tool:
            cmd = [CXX, "-std=gnu++17", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-I", str(VITA / "src"),
                   str(VITA / "tests" / "test_bootart.cpp"), str(VITA / "src" / "vita_bootart.cpp"), "-o", cls.exe]
            subprocess.run(cmd, check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        if not self.have_tool:
            self.skipTest("needs a C++ compiler and Pillow")

    def make_disc(self, name, logo_tpl, imd5=True, lz=True, seed=1):
        files = Path(self.tmp.name) / name / "files"
        (files / "Art" / "fe").mkdir(parents=True)
        rng = random.Random(seed)
        inner = u8_tree({"arc": {"timg": {"logo_UK_Banner.tpl": logo_tpl}}})
        member = lz77_matches(inner) if lz else inner
        if imd5:
            member = b"IMD5" + b"\0" * 28 + member
        banner = b"\0" * 0x600 + u8_tree({"meta": {"banner.bin": member, "icon.bin": b"x" * 40}})
        (files / "opening.bnr").write_bytes(banner)
        (files / "Art" / "fe" / "mainui.dmn").write_bytes(helpers.bundle([(0x1234, b"x" * 70), (brand.KEY_ART_HASH, cmpr_art(rng))]))
        return files

    def python_bin(self, files):
        banner = brand.read_disc(str(files), brand.BANNER)
        logo = brand.trim(brand.tpl_image(brand.banner_member(banner, "meta/banner.bin", brand.LOGO_MEMBER)))
        art = brand.bundle_texture(brand.read_disc(str(files), brand.FE_BUNDLE), brand.KEY_ART_HASH)[1].convert("RGB")
        return brand.loading_bin(brand.make_loading(art, logo))

    def cpp_bin(self, files):
        out = Path(self.tmp.name) / ("out-%s.bin" % files.parent.name)
        done = subprocess.run([self.exe, str(files), str(out)], capture_output=True, text=True)
        return done, (out.read_bytes() if done.returncode == 0 else None)

    def check_same(self, name, logo_tpl, **kw):
        files = self.make_disc(name, logo_tpl, **kw)
        done, got = self.cpp_bin(files)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        want = self.python_bin(files)
        self.assertEqual(len(got), len(want))
        if got != want:
            diffs = sum(1 for a, b in zip(got, want) if a != b)
            self.fail("%s: %d of %d bytes differ from the Python tool's loading.bin" % (name, diffs, len(want)))

    def test_rgba8_logo_with_nested_archives_matches_python(self):
        self.check_same("rgba8", rgba8_logo(random.Random(3), 64, 40))

    def test_a_flat_stored_banner_matches_python(self):
        self.check_same("plain", rgba8_logo(random.Random(4), 48, 36), imd5=False, lz=False)

    def test_every_logo_texture_format_matches_python(self):
        rng = random.Random(5)
        pal_rgb5a3 = b"".join(struct.pack(">H", rng.getrandbits(16)) for _ in range(256))
        pal_565 = b"".join(struct.pack(">H", rng.getrandbits(16)) for _ in range(256))
        cases = [
            ("i4", tpl_image(0, 30, 21, random_texels(rng, 0, 30, 21))),                 # a size that is not a whole number of tiles
            ("i8", tpl_image(1, 33, 17, random_texels(rng, 1, 33, 17))),
            ("ia4", tpl_image(2, 40, 24, random_texels(rng, 2, 40, 24))),
            ("ia8", tpl_image(3, 36, 20, random_texels(rng, 3, 36, 20))),
            ("rgb565", tpl_image(4, 40, 20, random_texels(rng, 4, 40, 20))),
            ("rgb5a3", tpl_image(5, 41, 19, random_texels(rng, 5, 41, 19))),
            ("c4", tpl_image(8, 24, 16, random_texels(rng, 8, 24, 16), pal_rgb5a3[:32])),
            ("c8", tpl_image(9, 40, 20, random_texels(rng, 9, 40, 20), pal_565, pal_fmt=1)),
            ("cmpr", tpl_image(14, 48, 24, random_texels(rng, 14, 48, 24))),
        ]
        for name, tpl in cases:
            with self.subTest(format=name):
                self.check_same("fmt-" + name, tpl, seed=sum(name.encode()))

    def test_missing_cut_and_damaged_files_fail_cleanly(self):
        files = self.make_disc("good", rgba8_logo(random.Random(6), 40, 24))
        done, good = self.cpp_bin(files)
        self.assertEqual(done.returncode, 0)
        rng = random.Random(9)
        # no files at all
        empty = Path(self.tmp.name) / "empty"
        empty.mkdir()
        done = subprocess.run([self.exe, str(empty), str(empty / "x.bin")], capture_output=True, text=True)
        self.assertEqual(done.returncode, 3)
        banner = (files / "opening.bnr").read_bytes()
        bundle = (files / "Art" / "fe" / "mainui.dmn").read_bytes()
        cases = [("bnr-cut-30", "opening.bnr", banner[:len(banner) // 3]), ("bnr-cut-tiny", "opening.bnr", banner[:50]), ("dmn-cut", "Art/fe/mainui.dmn", bundle[:300]),
                 ("dmn-cut-texture", "Art/fe/mainui.dmn", bundle[:len(bundle) - 5000]), ("dmn-zero", "Art/fe/mainui.dmn", b"\0" * 5000), ("bnr-zero", "opening.bnr", b"\0" * 5000)]
        for i in range(24):   # random damage: a run of bytes overwritten somewhere
            blob, rel = (banner, "opening.bnr") if i % 2 else (bundle, "Art/fe/mainui.dmn")
            at = rng.randrange(0, len(blob) if rel == "opening.bnr" else 600)
            bad = bytearray(blob)
            bad[at:at + 16] = bytes(rng.getrandbits(8) for _ in range(16))
            cases.append(("damage-%d" % i, rel, bytes(bad)))
        for name, rel, blob in cases:
            with self.subTest(case=name):
                copy = Path(self.tmp.name) / ("bad-" + name) / "files"
                shutil.copytree(files, copy)
                (copy / rel).write_bytes(blob)
                done = subprocess.run([self.exe, str(copy), str(copy / "x.bin")], capture_output=True, text=True)
                # a clean failure (1 or 3) or, when the damage hit nothing the picture reads, a picture; never a crash or a sanitizer report
                self.assertIn(done.returncode, (0, 1, 3), done.stdout + done.stderr)
                self.assertNotIn("ERROR: AddressSanitizer", done.stderr)
                self.assertNotIn("runtime error", done.stderr)


if __name__ == "__main__":
    unittest.main()
