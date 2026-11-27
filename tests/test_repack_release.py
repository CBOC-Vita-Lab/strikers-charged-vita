"""Release repacking preserves donor identity while explicitly versioning its SFO."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/repack-release.py"


def sfo(entries=None):
    if entries is None:
        entries = [
            ("TITLE_ID", 0x204, b"MSCN00003\0", 12),
            ("TITLE", 0x204, b"Example 01.00\0", 16),
            ("APP_VER", 0x204, b"01.00\0", 8),
            ("CONTENT_ID", 0x204, b"EP9000-MSCN00003_00-EXAMPLE000000000\0", 40),
        ]
    keys, data, index = bytearray(), bytearray(), bytearray()
    for key, fmt, value, maximum in entries:
        index += struct.pack("<HHIII", len(keys), fmt, len(value), maximum, len(data))
        keys += key.encode("ascii") + b"\0"
        data += value + b"\xA5" * (maximum - len(value))
    return bytes(struct.pack("<4sIIII", b"\0PSF", 0x101, 20 + len(index),
                             20 + len(index) + len(keys), len(entries)) + index + keys + data)


class RepackReleaseTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.base = Path(temp.name)
        self.previous = self.base / "previous.vpk"
        self.built = self.base / "built.vpk"
        self.output = self.base / "release.vpk"
        self.write_archive(self.built, {"eboot.bin": b"new-runtime",
                                       "licenses/test.txt": b"new-license",
                                       "sce_sys/param.sfo": b"do-not-use-built-sfo"})

    def write_archive(self, path, members):
        with zipfile.ZipFile(path, "w") as archive:
            for name, value in members.items():
                archive.writestr(name, value)

    def donor(self, raw_sfo):
        self.members = {"eboot.bin": b"old-runtime", "sce_sys/param.sfo": raw_sfo,
                        "sce_sys/icon0.png": b"donor-art", "shadercache/old.pack": b"old-cache",
                        "shadercache/vgx_shaderlist.bin": b"old-list"}
        self.write_archive(self.previous, self.members)

    def run_tool(self, *options):
        return subprocess.run([sys.executable, str(TOOL), *options, str(self.previous),
                               str(self.built), str(self.output)], capture_output=True, text=True)

    def assert_rejected(self, raw_sfo, version="01.20"):
        self.donor(raw_sfo)
        result = self.run_tool("--app-version", version)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("APP_VER", result.stderr)
        self.assertFalse(self.output.exists())

    def test_a_version_or_an_explicit_keep_is_required(self):
        # 1.3 (SC10): the public 1.1 showed APP_VER 01.00 because nobody gave a version and the donor's SFO was copied. Now the tool refuses to guess.
        self.donor(sfo())
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--app-version", result.stderr)
        self.assertFalse(self.output.exists())
        result = self.run_tool("--app-version", "01.30", "--keep-donor-version")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_the_output_reports_the_version_it_carries(self):
        self.donor(sfo())
        result = self.run_tool("--app-version", "01.30")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("APP_VER in the output's param.sfo: 01.30", result.stdout)
        result = self.run_tool("--keep-donor-version")
        self.assertIn("APP_VER in the output's param.sfo: 01.00", result.stdout)   # said plainly, not hidden

    def test_keep_donor_version_keeps_donor_sfo_without_parsing_or_changing_it(self):
        self.donor(b"legacy-opaque-sfo")
        result = self.run_tool("--keep-donor-version")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(archive.read("sce_sys/param.sfo"), b"legacy-opaque-sfo")
            self.assertEqual(archive.read("sce_sys/icon0.png"), b"donor-art")
            self.assertEqual(archive.read("eboot.bin"), b"new-runtime")
            self.assertEqual(archive.read("licenses/test.txt"), b"new-license")
            self.assertEqual(archive.read("shadercache/old.pack"), b"old-cache")

    def test_app_version_changes_only_the_donor_version_payload(self):
        original = sfo()
        self.donor(original)
        result = self.run_tool("--app-version", "01.20")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.output) as archive:
            updated = archive.read("sce_sys/param.sfo")
            # The fixture's TITLE also contains 01.00 and must not be touched.
            version_at = 20 + 4 * 16 + len(b"TITLE_ID\0TITLE\0APP_VER\0CONTENT_ID\0") + 12 + 16
            self.assertEqual(updated, original[:version_at] + b"01.20" + original[version_at + 5:])
            self.assertEqual(archive.read("sce_sys/icon0.png"), b"donor-art")
            self.assertEqual(archive.read("shadercache/old.pack"), b"old-cache")
            self.assertEqual(archive.read("eboot.bin"), b"new-runtime")

    def test_app_version_and_shader_replacement_work_together(self):
        self.donor(sfo())
        cache = self.base / "cache"
        cache.mkdir()
        (cache / "874a13e0.pack").write_bytes(b"new-cache")
        (cache / "vgx_shaderlist.bin").write_bytes(b"new-list")
        result = self.run_tool("--shadercache", str(cache), "--app-version", "01.20")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.output) as archive:
            self.assertNotIn("shadercache/old.pack", archive.namelist())
            self.assertEqual(archive.read("shadercache/874a13e0.pack"), b"new-cache")
            self.assertEqual(archive.read("shadercache/vgx_shaderlist.bin"), b"new-list")
            self.assertIn(b"01.20\0", archive.read("sce_sys/param.sfo"))

    def test_rejects_versions_outside_vita_app_ver_format(self):
        for version in ("1.2", "1.20", "01.2", "001.20", "01.200", "ab.cd", "01.20\n", ""):
            with self.subTest(version=version):
                self.assert_rejected(sfo(), version)

    def test_requires_an_app_ver_entry_in_the_donor(self):
        self.assert_rejected(sfo([("TITLE_ID", 0x204, b"MSCN00003\0", 12)]))

    def test_requires_the_donor_sfo(self):
        self.donor(sfo())
        del self.members["sce_sys/param.sfo"]
        self.write_archive(self.previous, self.members)
        result = self.run_tool("--app-version", "01.20")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("APP_VER", result.stderr)
        self.assertFalse(self.output.exists())

    def test_rejects_invalid_app_ver_field_format_or_value(self):
        for fmt, value, maximum in ((0x404, b"01.00\0", 8), (0x204, b"01.0\0", 8),
                                    (0x204, b"bad!!\0", 8), (0x204, b"01.00X", 8),
                                    (0x204, b"01\0.0\0", 8), (0x204, b"01.00\0", 5)):
            with self.subTest(fmt=fmt, value=value, maximum=maximum):
                self.assert_rejected(sfo([("APP_VER", fmt, value, maximum)]))

    def test_rejects_duplicate_app_ver_entries(self):
        self.assert_rejected(sfo([("APP_VER", 0x204, b"01.00\0", 8),
                                  ("APP_VER", 0x204, b"01.10\0", 8)]))

    def test_rejects_malformed_sfo_and_out_of_bounds_or_overlapping_fields(self):
        truncated = sfo()[:-1]
        bad_magic = b"BAD!" + sfo()[4:]
        invalid_offset = bytearray(sfo())
        struct.pack_into("<I", invalid_offset, 20 + 2 * 16 + 12, len(invalid_offset))
        overlapping = bytearray(sfo())
        struct.pack_into("<I", overlapping, 20 + 3 * 16 + 12, 28)
        for raw in (b"short", bad_magic, truncated, bytes(invalid_offset), bytes(overlapping)):
            with self.subTest(raw=raw[:12]):
                self.assert_rejected(raw)


if __name__ == "__main__":
    unittest.main()
