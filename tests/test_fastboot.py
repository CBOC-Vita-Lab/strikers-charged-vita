"""MSC_BOOT_MATCH, the test-only fast boot (wave 10): it never writes a save, its routes carry no menu lines, the generator is in step."""
from pathlib import Path
import re
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
FB = ROOT / "tools/console-1.2/fastboot"


class FastBoot(unittest.TestCase):
    def test_no_save_is_written_while_the_switch_is_on(self):
        # host build of nand_native.cpp: on = nothing changes (and reads work), off = the contrast, a broken guard is caught
        r = subprocess.run([sys.executable, "-I", str(ROOT / "native/platform/sdk/tests/run_nand_nosave_test.py")], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("negative control: the broken guard is caught", r.stdout)

    def test_generated_files_are_current(self):
        r = subprocess.run([sys.executable, "-I", str(ROOT / "tools/gen_fb_routes.py"), "--check"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_routes_start_at_the_launch_and_envs_name_their_route(self):
        names = sorted(p.stem for p in (FB / "routes").glob("fb-*.txt"))
        self.assertGreaterEqual(len(names), 6)
        for n in names:
            route = [l for l in (FB / "routes" / (n + ".txt")).read_text().splitlines() if l.strip() and not l.startswith("#")]
            if n.startswith("fb-options"): continue   # front20: menu routes (options screens from the title) kept beside the fast-boot ones; no bootmatch
            self.assertRegex(route[0], r"^bootmatch \w+:\d+:\d+")
            self.assertTrue(route[1].startswith("wait ingame == 1"), n)
            self.assertFalse([l for l in route if l.startswith("launch") or "scene == 0" in l or "onctx" in l], n)   # no title, menu or launch lines
            env = (FB / "env" / (n + ".txt")).read_text()
            self.assertIn("MSC_INPUT_SCRIPT=ux0:data/mscharged-perf-test/%s.txt" % n, env)

    def test_the_switch_is_off_by_default_and_not_in_the_plan(self):
        plan = (ROOT / "native/platform/vita/src/vita_plan.cpp").read_text()
        # the plan may READ it (the launch guard skips kit runs, wave 13) but never default it
        self.assertNotRegex(plan, r'\{"MSC_BOOT_MATCH"|setenv\("MSC_BOOT_MATCH"')
        harness = (ROOT / "native/platform/harness/src/np_harness.cpp").read_text()
        self.assertIn('getenv("MSC_BOOT_MATCH")', harness)
        self.assertRegex(harness, r"if \(!e \|\| !\*e \|\| !std::strcmp\(e, \"0\"\)")   # unset, empty, 0 and off leave the boot alone
        self.assertIn("MSC_BOOT_MATCH", (ROOT / "docs/TECHNICAL.md").read_text())


if __name__ == "__main__":
    unittest.main()
