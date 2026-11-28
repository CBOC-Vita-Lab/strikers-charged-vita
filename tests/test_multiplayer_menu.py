"""Exercise the production multiplayer chooser without game assets or network access."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MultiplayerMenu(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "menu-flow-test"
        result = subprocess.run([
            os.environ.get("CXX", "clang++"), "-std=c++11", "-O0", "-g", "-DNP_TARGET_HOST32=1",
            "-I", str(ROOT / "native"), str(ROOT / "native/platform/sdk/net/np_menu_flow.cpp"),
            str(ROOT / "native/platform/sdk/net/np_conntest.cpp"), str(ROOT / "native/platform/sdk/net/np_mpdiag.cpp"), str(ROOT / "native/platform/sdk/net/np_mpreport.cpp"),   # (wave 11: the connection test's classifier and the diagnostics ring)
            str(ROOT / "native/tools/net/menu_flow_test.cpp"), "-o", str(cls.binary),
            *(["-Wl,-undefined,dynamic_lookup"] if sys.platform == "darwin" else []),
        ], text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def test_in_game_mode_selection(self):
        for case in ("discoverable", "cancel-button", "online-back", "local-back",
                     "wifi", "nearby-host", "nearby-join", "nearby-auto", "repeat", "shortcuts",
                     "scene-wait", "saved-preference", "online-cancel", "online-failure", "popup-gone",
                     "start-failure", "radio-failure",
                     # (mpbeta27) the beta's screens; the cases above run with MSC_MP_BETA=0 (the old screens are its off path)
                     "beta-notice", "beta-scripted", "beta-online", "beta-report-noaccount", "beta-report-account", "beta-offer-crash", "beta-off-entry"):
            with self.subTest(case=case):
                result = subprocess.run([str(self.binary), case], cwd=self.temp.name, text=True, capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
