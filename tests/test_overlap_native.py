import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class OverlapHostTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "needs g++")
    def test_host_suite(self):
        with tempfile.TemporaryDirectory() as out:
            run = subprocess.run(
                ["sh", str(ROOT / "native/tools/overlap/run_tests.sh")],
                env={"OUT": out, "PATH": "/usr/bin:/bin:/usr/local/bin"},
                capture_output=True,
                text=True,
            )
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertNotIn("FAILED", run.stdout)


if __name__ == "__main__":
    unittest.main()
