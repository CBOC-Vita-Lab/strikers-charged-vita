"""tools/mp/lan_speed.py: two consoles of a pair run must have the same speed; a run made slow on purpose must be slow, else INVALID."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "native/tools/mp/lan_speed.py"


def log(steps_per_s, windows=8):
    lines = []
    t = 0.0
    for _ in range(windows):
        t += 5.0
        lines.append("[mp-lan] net: t=%.1f s active=5.0 s steps=%d starved=0 sps=%.1f" % (t, int(steps_per_s * 5), steps_per_s))
    return "\n".join(lines) + "\n"


def run(a, b, *args):
    with tempfile.TemporaryDirectory() as d:
        for i, steps in enumerate((a, b)):
            p = Path(d) / ("peer%d" % i)
            p.mkdir()
            (p / "runtime.log").write_text(log(steps))
        return subprocess.run([sys.executable, str(SCRIPT), d, *args], capture_output=True, text=True)


def mixed(fast_windows, slow_windows, dead_windows):
    """A slowdown that starts mid-run, then a dead link: the windows of the dead link are not a speed."""
    lines, t = [], 0.0
    for sps, n in ((50, fast_windows), (32, slow_windows), (0, dead_windows)):
        for _ in range(n):
            t += 5.0
            lines.append("[mp-lan] net: t=%.1f s active=5.0 s steps=%d starved=0 sps=%.1f" % (t, int(sps * 5), sps))
    return "\n".join(lines) + "\n"


class LanSpeed(unittest.TestCase):
    def test_a_slowdown_from_mid_run_and_a_dead_tail_do_not_split_the_two_consoles(self):
        with tempfile.TemporaryDirectory() as d:
            for i, (f, sl, de) in enumerate(((6, 9, 3), (6, 9, 0))):
                p = Path(d) / ("peer%d" % i)
                p.mkdir()
                (p / "runtime.log").write_text(mixed(f, sl, de))
            r = subprocess.run([sys.executable, str(SCRIPT), d, "--expect-slow", "0.8"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)

    def test_equal_speeds_pass(self):
        self.assertEqual(run(50, 49.5).returncode, 0)

    def test_one_sided_slow_motion_fails(self):
        r = run(50, 35)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("different speeds", r.stdout)

    def test_both_slow_together_passes_and_a_run_meant_to_be_slow_that_is_not_is_invalid(self):
        self.assertEqual(run(30, 30.5, "--expect-slow", "0.7").returncode, 0)
        self.assertEqual(run(50, 50, "--expect-slow", "0.7").returncode, 2)

    def test_too_short_is_invalid(self):
        with tempfile.TemporaryDirectory() as d:
            for i in (0, 1):
                p = Path(d) / ("peer%d" % i)
                p.mkdir()
                (p / "runtime.log").write_text(log(50, windows=2))
            self.assertEqual(subprocess.run([sys.executable, str(SCRIPT), d], capture_output=True, text=True).returncode, 2)


if __name__ == "__main__":
    unittest.main()
