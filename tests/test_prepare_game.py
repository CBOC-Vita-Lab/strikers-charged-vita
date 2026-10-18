import hashlib
import importlib.util
import io
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
