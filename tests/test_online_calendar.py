"""The game's season / day calendar against the online service's (online audit, 1.3).

native/tools/net/season_clock_test.cpp runs the game's own NetworkSeasonCalendar.cpp with the region hour shift decided as the game decides
it (np/online_clock.h). For UTC times around season and day boundaries (year end and leap days included), the game must be in the same
weekly season and on the same day as the service (server/src/core/calendar.js), or its totals start again hours away from the service's
windows and points are lost. MSC_ONLINE_RETAIL_TZ=1 (the retail -8 h / +9 h) is the deliberate-break check: it must disagree.
"""
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "native"
CXX = shutil.which("g++") or shutil.which("clang++")
NODE = shutil.which("node")

NODE_SCRIPT = r"""
import { season, dateParts } from %s;
const times = JSON.parse(process.argv[2]);
for (const t of times) {
  const p = dateParts(t);
  console.log(`${t} ${season(t).index} ${p.year}-${p.month}-${p.mday}`);
}
"""


def sample_times():
    day = 86400000
    out = []
    for year in (2026, 2027, 2028):  # 2028 is a leap year
        for month, mday in ((1, 1), (1, 8), (2, 26), (2, 29), (3, 1), (3, 5), (11, 12), (12, 24), (12, 31)):
            try:
                import datetime
                base = int(datetime.datetime(year, month, mday, tzinfo=datetime.timezone.utc).timestamp() * 1000)
            except ValueError:
                continue
            for minutes in (-9 * 60 - 1, -8 * 60 - 1, -61, -1, 0, 1, 7 * 60 + 59, 8 * 60 + 1, 15 * 60, 23 * 60 + 59):
                out.append(base + minutes * 60000)
    out.append(1793361600000)  # PROTOCOL.md's example time
    out.extend(1793361600000 + k * day // 7 for k in range(200))
    return sorted(set(out))


@unittest.skipUnless(CXX and NODE, "needs a C++ compiler and node")
class OnlineCalendarTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        exe = Path(cls.tmp.name) / "season_clock_test"
        cmd = [CXX, "-O1", "-DNP_NATIVE=1", "-DNP_TARGET_HOST32=1", "-DVERSION_R4QE01",
               "-I" + str(NATIVE / "tools/net/season_shim"), "-I" + str(NATIVE), "-I" + str(NATIVE / "include"),
               "-I" + str(NATIVE / "libs/RVL_SDK/include"), "-I" + str(NATIVE / "libs/Runtime/include"),
               "-include", "np/compat.h", "-fno-exceptions", "-fno-rtti", "-w",
               str(NATIVE / "src/Game/NetworkSeasonCalendar.cpp"), str(NATIVE / "tools/net/season_clock_test.cpp"), "-o", str(exe)]
        build = subprocess.run(cmd, capture_output=True, text=True, cwd=str(NATIVE))
        if build.returncode != 0:
            raise AssertionError("build failed:\n" + build.stderr[-3000:])
        cls.exe = exe
        cls.times = sample_times()
        script = Path(cls.tmp.name) / "cal.mjs"
        script.write_text(NODE_SCRIPT % json.dumps((ROOT / "server/src/core/calendar.js").as_uri()))
        run = subprocess.run([NODE, str(script), json.dumps(cls.times)], capture_output=True, text=True, check=True)
        cls.server = run.stdout.split("\n")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def game(self, retail_hours, env_extra=None):
        env = {k: v for k, v in os.environ.items() if k != "MSC_ONLINE_RETAIL_TZ"}
        env.update(env_extra or {})
        run = subprocess.run([str(self.exe), str(retail_hours)], input="\n".join(map(str, self.times)) + "\n",
                             capture_output=True, text=True, env=env, check=True)
        return run.stdout.split("\n")

    def test_game_windows_are_the_service_windows(self):
        for retail in (-8, 9, 0):  # US, Japan, Europe
            with self.subTest(retail=retail):
                mismatches = [(g, s) for g, s in zip(self.game(retail), self.server) if g != s]
                self.assertEqual(mismatches, [], f"game vs service (retail shift {retail} h)")
        self.assertGreater(len(self.times), 250)

    def test_retail_shift_disagrees(self):
        # deliberate break: the retail US shift puts the game in another season or day for 8 hours around every boundary
        for retail in (-8, 9):
            got = self.game(retail, {"MSC_ONLINE_RETAIL_TZ": "1"})
            mismatches = [(g, s) for g, s in zip(got, self.server) if g != s]
            self.assertGreater(len(mismatches), 20, f"retail shift {retail} h should disagree with the UTC service")


if __name__ == "__main__":
    unittest.main()
