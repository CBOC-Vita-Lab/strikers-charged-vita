"""The performance recorder's bank format and tools/diag/perf-report.py agree (host round trip)."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import struct
import sys
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("perf_report", ROOT / "tools/diag/perf-report.py")
perf_report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(perf_report)


@unittest.skipUnless(shutil.which("g++"), "needs g++")
class RecorderRoundTrip(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        out = Path(cls.tmp.name) / "out"
        out.mkdir()
        exe = Path(cls.tmp.name) / "roundtrip"
        native = ROOT / "native"
        subprocess.run(["g++", "-std=gnu++17", "-O1", "-Iinclude", "-Iplatform/vita/src",
                        str(ROOT / "tests/diag/recorder_roundtrip.cpp"), "platform/compat/np_touchdiag.cpp",
                        "-o", str(exe), "-lpthread"], cwd=native, check=True)
        env = dict(os.environ, MSC_TOUCH_DIAG="1", MSC_DIAG_DIR=str(out))
        subprocess.run([str(exe)], env=env, check=True, capture_output=True)
        cls.banks = sorted(out.glob("perf-*.bin"))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_bank_written_and_valid(self):
        self.assertEqual(len(self.banks), 1)
        bank = perf_report.read_bank(str(self.banks[0]))
        self.assertTrue(bank["crcOk"])
        self.assertEqual(bank["mode"], 2)
        self.assertEqual(bank["overflow"], 0)

    def test_report_figures(self):
        r = perf_report.analyse(perf_report.read_bank(str(self.banks[0])))
        self.assertEqual(r["handoverToGateMs"]["count"], 20)
        self.assertGreaterEqual(r["displayIntervalMs"]["median"], 9.5)       # 2 ms + 8 ms sleeps per frame
        self.assertEqual(r["drawsPerFrame"]["median"], 40.0)
        call = r["gpuCallMs"]["sceGxmDisplayQueueAddEntry"]
        self.assertEqual(call["count"], 20)
        self.assertGreaterEqual(call["median"], 5.0)

    def test_cli_json_and_damage_detection(self):
        out = subprocess.run(["python3", str(ROOT / "tools/diag/perf-report.py"), "--json", str(self.banks[0])],
                             check=True, capture_output=True, text=True).stdout
        self.assertTrue(json.loads(out)[0]["crcOk"])
        damaged = Path(self.tmp.name) / "damaged.bin"
        data = bytearray(self.banks[0].read_bytes())
        data[200] ^= 0xff
        damaged.write_bytes(bytes(data))
        self.assertFalse(perf_report.read_bank(str(damaged))["crcOk"])
        with self.assertRaises(ValueError):
            short = Path(self.tmp.name) / "short.bin"
            short.write_bytes(bytes(data[:100]))
            perf_report.read_bank(str(short))


def reader_bank(events=()):
    """Reader-only fixture: mirrors Persist offsets, not native/Vita evidence."""
    header = bytearray(perf_report.HEADER)
    header[:7] = b"MSCTD02"
    struct.pack_into("<6I", header, 8, 2, perf_report.EVENT.size, len(events), 16384, len(events), 0)
    struct.pack_into("<I", header, 36, 4)
    struct.pack_into("<2Q", header, 40, 1000, 2000)
    struct.pack_into("<4I", header, 92, 2, 2, 0, 0)
    data = bytes(header) + b"".join(perf_report.EVENT.pack(*event) for event in events)
    return data + struct.pack("<I", zlib.crc32(data) & 0xffffffff)


class ReadBankFileLifetime(unittest.TestCase):
    # No compiler skip guard: file ownership is a pure Python contract.
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.opened = []
        real_open = open

        def tracking_open(*args, **kwargs):
            held = real_open(*args, **kwargs)
            self.opened.append(held)
            return held

        patcher = mock.patch.object(perf_report, "open", tracking_open, create=True)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(self.close_opened)

    def close_opened(self):
        for held in self.opened:
            held.close()

    def bank_path(self, name, data):
        path = Path(self.tmp.name) / name
        path.write_bytes(data)
        return str(path)

    def assert_closed(self):
        self.assertEqual(len(self.opened), 1)
        self.assertTrue(self.opened[0].closed)

    def test_valid_file_closed_with_live_reference(self):
        data = reader_bank([(1500, 1, 0, 0, 0, 0, 0, 13, 2)])
        bank = perf_report.read_bank(self.bank_path("valid.bin", data))
        self.assertTrue(bank["crcOk"])
        self.assertEqual(bank["count"], 1)
        self.assert_closed()

    def test_crc_bad_file_closed_without_changing_damage_contract(self):
        data = bytearray(reader_bank([(1500, 1, 0, 0, 0, 0, 0, 13, 2)]))
        data[130] ^= 0xff
        self.assertFalse(perf_report.read_bank(self.bank_path("damaged.bin", data))["crcOk"])
        self.assert_closed()

    def test_short_file_closed_before_value_error(self):
        with self.assertRaisesRegex(ValueError, "not a recorder bank"):
            perf_report.read_bank(self.bank_path("short.bin", b"x" * 100))
        self.assert_closed()

    def test_bad_magic_file_closed_before_value_error(self):
        data = bytearray(reader_bank())
        data[:7] = b"WRONG!!"
        with self.assertRaisesRegex(ValueError, "not a recorder bank"):
            perf_report.read_bank(self.bank_path("magic.bin", data))
        self.assert_closed()

    def test_truncated_file_closed_before_value_error(self):
        data = bytearray(reader_bank([(1500, 1, 0, 0, 0, 0, 0, 13, 2)]))
        struct.pack_into("<I", data, 16, 5)
        with self.assertRaisesRegex(ValueError, "truncated"):
            perf_report.read_bank(self.bank_path("truncated.bin", data))
        self.assert_closed()

    def test_wrong_event_size_file_closed_before_value_error(self):
        data = bytearray(reader_bank())
        struct.pack_into("<I", data, 12, 32)
        with self.assertRaisesRegex(ValueError, "event size 32"):
            perf_report.read_bank(self.bank_path("size.bin", data))
        self.assert_closed()

    def test_read_exception_closes_and_propagates_same_oserror(self):
        failure = OSError("injected read failure")

        class FailingFile:
            closed = False

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                self.close()
                return False

            def read(self):
                raise failure

            def close(self):
                self.closed = True

        held = FailingFile()
        self.opened.append(held)
        with mock.patch.object(perf_report, "open", return_value=held, create=True):
            with self.assertRaises(OSError) as caught:
                perf_report.read_bank("read-failure.bin")
        self.assertIs(caught.exception, failure)
        self.assert_closed()

    def test_cli_partial_success_and_unknown_kind_contract(self):
        short = self.bank_path("short-cli.bin", b"x" * 100)
        valid = self.bank_path("unknown-kind.bin", reader_bank([(1500, 1, 0, 0, 0, 0, 0, 99, 0)]))
        result = subprocess.run([sys.executable, str(Path(perf_report.__file__)), "--json", short, valid],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not a recorder bank (MSCTD02)", result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(len(report), 1)
        self.assertTrue(report[0]["crcOk"])
        self.assertNotIn("events", report[0])
        self.assertEqual(report[0]["kinds"]["99"], 1)


if __name__ == "__main__":
    unittest.main()
