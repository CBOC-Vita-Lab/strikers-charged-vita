import hashlib
import importlib.util
import io
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "PREPARE-GAME.py"


class PrepareGameTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), "The one-command preparation tool is missing")
        spec = importlib.util.spec_from_file_location("prepare_game", SCRIPT)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_host_selection_handles_windows_and_mac_arch_aliases(self):
        for system, machine, name in [
            ("Windows", "AMD64", "nodtool-windows-x86_64.exe"),
            ("Windows", "ARM64", "nodtool-windows-arm64.exe"),
            ("Darwin", "arm64", "nodtool-macos-arm64"),
            ("Linux", "aarch64", "nodtool-linux-aarch64"),
        ]:
            with self.subTest(system=system, machine=machine):
                self.assertEqual(self.module.tool_asset(system, machine)[0], name)

    def test_unsupported_host_stops_instead_of_choosing_wrong_executable(self):
        with self.assertRaisesRegex(ValueError, "Unsupported"):
            self.module.tool_asset("Linux", "riscv64")

    def test_disc_check_uses_outer_header_not_partition_revision(self):
        self.module.check_disc_info("Game ID: R4QE01\nDisc 1, Revision 1\n\nPartition 1\n\tGame ID: R4QE01\n\tDisc 1, Revision 0\n")

    def test_wrong_disc_or_revision_cannot_pass_via_partition_header(self):
        for header in ["Game ID: R4QP01\nDisc 1, Revision 1", "Game ID: R4QE01\nDisc 1, Revision 0", ""]:
            with self.subTest(header=header), self.assertRaises(ValueError):
                self.module.check_disc_info(header + "\nPartition 1\n\tGame ID: R4QE01\n\tDisc 1, Revision 1\n")

    def test_existing_output_is_never_overwritten(self):
        disc = self.root / "game.iso"
        disc.write_bytes(b"fake disc")
        output = self.root / "game"
        output.mkdir()
        save = output / "keep.txt"
        save.write_text("keep")
        with self.assertRaises(FileExistsError):
            self.module.prepare(disc, output, self.root / "nonexistent-tool")
        self.assertEqual(save.read_text(), "keep")

    def test_missing_disc_does_not_create_output(self):
        output = self.root / "game"
        with self.assertRaises(FileNotFoundError):
            self.module.prepare(self.root / "missing.iso", output, self.root / "tool")
        self.assertFalse(output.exists())

    def test_wrong_game_executable_is_rejected(self):
        (self.root / "sys").mkdir()
        (self.root / "files/Art").mkdir(parents=True)
        (self.root / "sys/main.dol").write_bytes(b"wrong revision")
        (self.root / "sys/fst.bin").write_bytes(b"table")
        with self.assertRaisesRegex(ValueError, "main.dol"):
            self.module.check_game(self.root)

    def extraction_fixture(self):
        (self.root / "sys").mkdir()
        (self.root / "files/Art").mkdir(parents=True)
        executable = b"supported executable fixture"
        (self.root / "sys/main.dol").write_bytes(executable)
        self.module.GAME_SHA256 = hashlib.sha256(executable).hexdigest()
        (self.root / "files/Art/test.bin").write_bytes(b"data")
        table = b"".join(struct.pack(">III", *node) for node in (
            (0x01000000, 0, 3),
            (0x01000001, 0, 3),
            (5, 0x1000, 4),
        )) + b"\0Art\0test.bin\0"
        (self.root / "sys/fst.bin").write_bytes(table)
        self.module.FST_SHA256 = hashlib.sha256(table).hexdigest()
        return table

    def test_complete_extraction_passes_without_changing_files(self):
        self.extraction_fixture()
        before = {p.relative_to(self.root): p.read_bytes()
                  for p in self.root.rglob("*") if p.is_file()}
        self.module.check_game(self.root)
        self.assertEqual(before, {p.relative_to(self.root): p.read_bytes()
                                 for p in self.root.rglob("*") if p.is_file()})

    def test_missing_asset_is_not_reported_as_ready(self):
        self.extraction_fixture()
        (self.root / "files/Art/test.bin").unlink()
        with self.assertRaisesRegex(ValueError, "Art/test.bin"):
            self.module.check_game(self.root)

    def test_truncated_or_oversized_asset_is_rejected(self):
        self.extraction_fixture()
        for contents in (b"", b"bad", b"extra"):
            with self.subTest(size=len(contents)):
                (self.root / "files/Art/test.bin").write_bytes(contents)
                with self.assertRaisesRegex(ValueError, "Art/test.bin"):
                    self.module.check_game(self.root)

    def test_asset_directory_is_not_accepted_as_a_file(self):
        self.extraction_fixture()
        path = self.root / "files/Art/test.bin"
        path.unlink()
        path.mkdir()
        with self.assertRaisesRegex(ValueError, "Art/test.bin"):
            self.module.check_game(self.root)

    def test_malformed_file_table_is_rejected(self):
        table = self.extraction_fixture()
        cases = (
            b"", table[:11],
            struct.pack(">III", 0x01000000, 0, 0xffffffff),
            table[:12] + struct.pack(">III", 0x01000001, 0, 1) + table[24:],
            table[:12] + struct.pack(">III", 0x01000001, 2, 3) + table[24:],
            table[:24] + struct.pack(">III", 0xffffff, 0x1000, 4) + table[36:],
            table[:-1],
            table[:36] + b"\0Art\0../x.bin\0",
        )
        for number, contents in enumerate(cases):
            with self.subTest(case=number):
                (self.root / "sys/fst.bin").write_bytes(contents)
                with self.assertRaisesRegex(ValueError, "fst.bin"):
                    self.module.check_file_table(self.root)

    def test_changed_supported_inventory_is_rejected_before_asset_validation(self):
        table = self.extraction_fixture()
        cases = {
            "forged_extent": table[:24] + struct.pack(">III", 5, 0x2000, 4) + table[36:],
            "no_assets": b"".join(struct.pack(">III", *node) for node in (
                (0x01000000, 0, 2), (0x01000000, 0, 2),
            )) + b"Art\0",
            "duplicate_file": b"".join(struct.pack(">III", *node) for node in (
                (0x01000000, 0, 4), (0x01000001, 0, 4),
                (5, 0x1000, 4), (5, 0x2000, 4),
            )) + b"\0Art\0test.bin\0",
        }
        for name, contents in cases.items():
            with self.subTest(case=name):
                (self.root / "sys/fst.bin").write_bytes(contents)
                with self.assertRaisesRegex(ValueError, "fst.bin does not match"):
                    self.module.check_game(self.root)

    def test_game_data_links_are_rejected(self):
        self.extraction_fixture()
        for relative in ("sys", "files", "sys/main.dol", "sys/fst.bin"):
            with self.subTest(path=relative):
                path = self.root / relative
                target = self.root / "linked-data"
                path.rename(target)
                try:
                    path.symlink_to(target, target_is_directory=target.is_dir())
                    with self.assertRaisesRegex(ValueError, "link instead of game data"):
                        self.module.check_game(self.root)
                finally:
                    path.unlink(missing_ok=True)
                    target.rename(path)

    def test_case_differences_match_runtime_file_lookup(self):
        self.extraction_fixture()
        (self.root / "files/Art/test.bin").rename(self.root / "files/Art/TEST.BIN")
        self.module.check_game(self.root)

    def test_case_different_asset_directory_matches_runtime_file_lookup(self):
        self.extraction_fixture()
        (self.root / "files/Art").rename(self.root / "files/ART")
        self.module.check_game(self.root)

    def test_missing_files_root_has_a_clear_incomplete_extraction_error(self):
        self.extraction_fixture()
        (self.root / "files/Art/test.bin").unlink()
        (self.root / "files/Art").rmdir()
        (self.root / "files").rmdir()
        with self.assertRaisesRegex(ValueError, "extraction is incomplete:.*files"):
            self.module.check_game(self.root)

    def test_missing_empty_directory_does_not_reject_complete_assets(self):
        self.extraction_fixture()
        table = b"".join(struct.pack(">III", *node) for node in (
            (0x01000000, 0, 4),
            (0x01000001, 0, 3),
            (5, 0x1000, 4),
            (0x0100000e, 0, 4),
        )) + b"\0Art\0test.bin\0GameLog\0"
        (self.root / "sys/fst.bin").write_bytes(table)
        self.module.check_file_table(self.root)

    def test_wrapped_disc_extent_is_rejected(self):
        table = self.extraction_fixture()
        (self.root / "sys/fst.bin").write_bytes(
            table[:24] + struct.pack(">III", 5, 0xffffffff, 4) + table[36:])
        with self.assertRaisesRegex(ValueError, "fst.bin"):
            self.module.check_file_table(self.root)

    def test_overlapping_disc_extents_are_rejected(self):
        self.extraction_fixture()
        (self.root / "files/Art/other.bin").write_bytes(b"else")
        table = b"".join(struct.pack(">III", *node) for node in (
            (0x01000000, 0, 4),
            (0x01000001, 0, 4),
            (5, 0x1000, 4),
            (14, 0x1000, 4),
        )) + b"\0Art\0test.bin\0other.bin\0"
        (self.root / "sys/fst.bin").write_bytes(table)
        with self.assertRaisesRegex(ValueError, "fst.bin"):
            self.module.check_file_table(self.root)

    def test_download_is_hash_checked_before_becoming_executable(self):
        asset = ("nodtool-test", hashlib.sha256(b"verified tool").hexdigest())
        with patch.object(self.module.urllib.request, "urlopen", return_value=io.BytesIO(b"tampered")):
            with self.assertRaisesRegex(ValueError, "checksum"):
                self.module.obtain_tool(self.root, asset)
        self.assertFalse((self.root / asset[0]).exists())
        with patch.object(self.module.urllib.request, "urlopen", return_value=io.BytesIO(b"verified tool")):
            tool = self.module.obtain_tool(self.root, asset)
        self.assertEqual(tool.read_bytes(), b"verified tool")

    def test_cached_tool_is_checked_and_not_silently_replaced(self):
        asset = ("nodtool-test", hashlib.sha256(b"verified tool").hexdigest())
        tool = self.root / asset[0]
        tool.write_bytes(b"modified tool")
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.module.obtain_tool(self.root, asset)
        self.assertEqual(tool.read_bytes(), b"modified tool")

    def test_symlink_output_is_rejected_even_when_target_is_missing(self):
        disc = self.root / "game.iso"
        disc.write_bytes(b"fake disc")
        output = self.root / "game"
        output.symlink_to(self.root / "missing")
        with self.assertRaises(FileExistsError):
            self.module.prepare(disc, output, self.root / "tool")

    def test_the_art_step_never_stops_the_preparation(self):
        output = self.root / "game"
        (output / "files").mkdir(parents=True)
        with patch("sys.stderr", new=io.StringIO()) as err:
            self.assertIsNone(self.module.brand_package(output, self.root / "missing.vpk"))   # no package
            package = self.root / "p.vpk"
            package.write_bytes(b"not a zip")
            self.assertIsNone(self.module.brand_package(output, package))                       # no game art in the folder
        self.assertIn("plain package works", err.getvalue())
        self.assertEqual(list(output.iterdir()), [output / "files"])


if __name__ == "__main__":
    unittest.main()
