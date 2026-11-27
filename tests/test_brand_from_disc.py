"""tools/brand-from-disc.py on made-up game files (no game art anywhere in the repository): the containers, the decoders, the package step."""
import importlib.util
import io
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

try:
    from PIL import Image
except ImportError:  # the tool itself needs Pillow
    Image = None

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "brand-from-disc.py"


def load():
    spec = importlib.util.spec_from_file_location("brand_from_disc", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def u8(files):
    """A flat U8 archive {name: bytes} (root + files), as the Wii's tools write it."""
    names = b"\0" + b"".join(n.encode() + b"\0" for n in files)
    count = 1 + len(files)
    header_size = count * 12 + len(names)
    data_at = (0x20 + header_size + 0x1F) // 0x20 * 0x20
    entries = [struct.pack(">III", 0x01000000, 0, count)]
    body, at, name_at = b"", data_at, 1
    for name, blob in files.items():
        entries.append(struct.pack(">III", name_at, at, len(blob)))
        name_at += len(name) + 1
        body += blob + b"\0" * (-len(blob) % 0x20)
        at += len(blob) + (-len(blob) % 0x20)
    head = b"U\xaa8-" + struct.pack(">III", 0x20, header_size, data_at) + b"\0" * 16
    return head + b"".join(entries) + names + b"\0" * (data_at - 0x20 - header_size) + body


def lz77_literals(blob):
    out = b"\x10" + len(blob).to_bytes(3, "little")
    for i in range(0, len(blob), 8):
        chunk = blob[i:i + 8]
        out += b"\0" + chunk
    return b"LZ77" + out


def tpl_rgba8(w, h, pixel):
    """A TPL with one RGBA8 image (4 x 4 tiles: 16 AR pairs then 16 GB pairs)."""
    tiles = b""
    for _ in range((w // 4) * (h // 4)):
        tiles += bytes(v for _ in range(16) for v in (pixel[3], pixel[0])) + bytes(v for _ in range(16) for v in (pixel[1], pixel[2]))
    header = struct.pack(">III", 0x0020AF30, 1, 12) + struct.pack(">II", 20, 0)
    image = struct.pack(">HHII", h, w, 6, 0x40) + b"\0" * 20
    return header + image + b"\0" * (0x40 - len(header) - len(image)) + tiles


def bundle(entries, sector=32):
    """A BundleFile: header, directory at sector 1, the entries after it, each on a sector."""
    directory = b"".join(struct.pack(">III", h, 0, len(b)) for h, b in entries)
    first = 1 + (len(directory) + sector - 1) // sector
    out, at, rows = b"", first, b""
    for h, blob in entries:
        rows += struct.pack(">III", h, at, len(blob))
        padded = blob + b"\0" * (-len(blob) % sector)
        out += padded
        at += len(padded) // sector
    rows += b"\0" * (-len(rows) % sector)
    return struct.pack(">IIII", sector, len(entries), 1, 0) + b"\0" * (sector - 16) + rows + out


def texture(game_fmt, w, h, texels, palette=b"", entries=0):
    return struct.pack(">II", 1, game_fmt) + b"\x05\x05\x05\x04" + b"\0\0" + struct.pack(">HH", w, h) + b"\0\0" + struct.pack(">II", entries, 0) + b"\0" * 4 + texels + palette


@unittest.skipIf(Image is None, "Pillow is not installed")
class BrandFromDiscTests(unittest.TestCase):
    def setUp(self):
        self.brand = load()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def make_disc(self):
        files = self.root / "game" / "files"
        (files / "Art/fe/fonts").mkdir(parents=True)
        banner_inner = u8({"arc/timg/logo_UK_Banner.tpl": tpl_rgba8(16, 8, (250, 120, 20, 255))})
        # the real file names have directories; the flat made-up archive keeps the full path as the name (the reader joins directories the same way)
        banner = b"IMD5" + b"\0" * 28 + lz77_literals(banner_inner)
        (files / "opening.bnr").write_bytes(b"\0" * 0x600 + u8({"meta/banner.bin": banner}))
        art = texture(2, 512, 512, bytes([0xF8, 0x00, 0xF8, 0x00, 0, 0, 0, 0]) * (512 * 512 // 16))
        (files / "Art/fe/mainui.dmn").write_bytes(bundle([(self.brand.KEY_ART_HASH, art)]))
        page = texture(8, 512, 512, bytes(512 * 512), b"".join(struct.pack(">H", 0x8000 | ((i & 31) << 10 | (i & 31) << 5 | (i & 31))) for i in range(256)), 256)
        glyphs = "".join("Glyph %s Width 20 24 1\r\n" % c for c in "ISTARGEN")
        description = ("NLG Font Description file\r\nVersion 1.1\r\nPageSize 512 PageCount 1\r\nHeight 57 RenderHeight 75 Ascent 46 RenderAscent 50 IL 9\r\n"
                       + glyphs + "End\r\n").encode()
        (files / "Art/fe/fonts/engfontheading36.res").write_bytes(bundle([(1, page), (2, description)]))
        return self.root / "game"

    def test_lz77_and_u8_round_trip(self):
        blob = bytes(range(200))
        self.assertEqual(self.brand.lz77_10(lz77_literals(blob)), blob)
        members = self.brand.u8_members(u8({"a/b.bin": b"abc", "c.bin": b"defgh"}))
        self.assertEqual(members, {"a/b.bin": b"abc", "c.bin": b"defgh"})

    def test_texture_formats(self):
        # RGB5A3 with the alpha bit set is opaque RGB555; 0x7FFF without it is 3-bit alpha 7 and 4-bit colours 15
        self.assertEqual(self.brand.rgb5a3(0x8000 | (31 << 10)), (255, 0, 0, 255))
        self.assertEqual(self.brand.rgb5a3(0x7FFF), (255, 255, 255, 255))
        self.assertEqual(self.brand.rgb565(0x07E0), (0, 255, 0, 255))
        image = self.brand.tpl_image(tpl_rgba8(8, 8, (10, 20, 30, 200)))
        self.assertEqual(image.size, (8, 8))
        self.assertEqual(image.getpixel((3, 5)), (10, 20, 30, 200))

    def test_everything_is_made_at_the_sizes_the_console_takes(self):
        disc = self.make_disc()
        files, shown = self.brand.build(str(disc))
        self.assertEqual(set(files), set(self.brand.OUTPUTS))
        sizes = {"sce_sys/icon0.png": (128, 128), "sce_sys/pic0.png": (960, 544), "sce_sys/livearea/contents/bg.png": (840, 500),
                 "sce_sys/livearea/contents/startup.png": (280, 158), "sce_sys/livearea/contents/settings.png": (280, 124)}
        for name, size in sizes.items():
            with self.subTest(name=name):
                png = Image.open(io.BytesIO(files[name]))
                self.assertEqual(png.size, size)
                self.assertEqual(png.mode, "RGBA" if name.endswith("settings.png") else "P")
        loading = files["boot/loading.bin"]
        self.assertEqual(loading[:4], b"NPBS")
        self.assertEqual(struct.unpack_from("<HHI", loading, 4), (960, 544, 1))
        self.assertEqual(len(loading), 12 + 960 * 544 * 2)

    def test_the_package_keeps_everything_else_byte_for_byte(self):
        disc = self.make_disc()
        files, _ = self.brand.build(str(disc))
        source, target = self.root / "in.vpk", self.root / "out.vpk"
        with zipfile.ZipFile(source, "w") as vpk:
            vpk.writestr("eboot.bin", b"executable")
            vpk.writestr("sce_sys/icon0.png", b"generic icon")
            vpk.writestr("shadercache/x.pack", b"cache")
        self.brand.apply_to_vpk(files, str(source), str(target))
        with zipfile.ZipFile(source) as old, zipfile.ZipFile(target) as new:
            self.assertIsNone(new.testzip())
            self.assertEqual(new.read("eboot.bin"), old.read("eboot.bin"))
            self.assertEqual(new.read("shadercache/x.pack"), b"cache")
            self.assertNotEqual(new.read("sce_sys/icon0.png"), b"generic icon")
            self.assertEqual(new.namelist().count("sce_sys/icon0.png"), 1)
            self.assertIn("boot/loading.bin", new.namelist())
            self.assertIn("sce_sys/pic0.png", new.namelist())
        with self.assertRaises(self.brand.BrandError):   # never over the original
            self.brand.apply_to_vpk(files, str(source), str(source))
        again = self.root / "again.vpk"
        self.brand.apply_to_vpk(files, str(source), str(again))
        self.assertEqual(again.read_bytes(), target.read_bytes())   # the same disc gives the same bytes

    def test_a_folder_without_game_files_is_refused_with_a_message(self):
        with self.assertRaises(self.brand.BrandError):
            self.brand.build(str(self.root))


if __name__ == "__main__":
    unittest.main()
