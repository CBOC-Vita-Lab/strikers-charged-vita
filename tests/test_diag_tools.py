"""The performance recorder's bank format and tools/diag/perf-report.py agree (host round trip)."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

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


if __name__ == "__main__":
    unittest.main()
