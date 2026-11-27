"""Every key that native/platform/vita/src/vita_plan.cpp gives a default to has a docs/TECHNICAL.md row that states that default
(tools/check-plan-docs.py). A new plan default fails here until its row says so."""
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PlanDocs(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", str(ROOT / "tools/check-plan-docs.py"), *args], cwd=ROOT, capture_output=True, text=True)

    def test_plan_defaults_are_documented(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_the_check_can_fail(self):
        # the table must list the plan's own keys, so a run that sees none would pass for the wrong reason
        result = self.run_tool("--all")
        self.assertIn("MSC_VGX_FINCLAMP", result.stdout)
        self.assertIn("plan=3(plan)", result.stdout)


if __name__ == "__main__":
    unittest.main()
