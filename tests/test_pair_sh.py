"""tools/mp/pair.sh must fail when the pair run fails (a green exit code after a dead run let a broken run look like a pass)."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SH = ROOT / "native/tools/mp/pair.sh"


class PairSh(unittest.TestCase):
    def test_a_run_that_cannot_start_exits_nonzero_and_says_so(self):
        with tempfile.TemporaryDirectory() as d:
            env = dict(os.environ, BIN=str(Path(d) / "no-such-binary"), PAIRS_DIR=d, PAIR_TIMEOUT="20")
            r = subprocess.run(["bash", str(SH), "t1"], capture_output=True, text=True, env=env, timeout=120)
            self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("pair rc=", r.stdout)

    def test_script_sets_pipefail_and_returns_the_run_status(self):
        text = SH.read_text()
        self.assertIn("set -o pipefail", text)
        self.assertIn("exit $rc", text)


if __name__ == "__main__":
    unittest.main()
