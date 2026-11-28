"""One version everywhere (1.3, SC10): the public 1.1 package showed APP_VER 01.00 on the console. The version a player sees comes from five places that must agree:
the SFO's APP_VER (vita.mk NP_APP_VER, which Makefile.vpk gives vita-mksfoex and tools/repack-release.py --app-version must repeat), the dotted version of the game (np_app_version.h,
which Settings > ABOUT, the log header and the settings stamp read, and the online service gets), cboc-release.json and docs/RELEASE.json."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def dotted(app_ver):
    major, minor = app_ver.split(".")
    return "%d.%d.%d" % (int(major), int(minor) // 10, int(minor) % 10)


class VersionIdentity(unittest.TestCase):
    def setUp(self):
        self.app_ver = re.search(r"^NP_APP_VER\s*\?=\s*([0-9]{2}\.[0-9]{2})", read("native/platform/vita/vita.mk"), re.M).group(1)

    def test_sfo_version_is_the_game_version(self):
        self.assertEqual(self.app_ver, "02.01")
        built = re.search(r'return AppVersionValid\(NP_ONLINE_APP_VERSION\) \? NP_ONLINE_APP_VERSION : "([0-9.]+)"', read("native/platform/sdk/net/online/np_app_version.h")).group(1)
        self.assertEqual(built, dotted(self.app_ver))
        fallback = re.search(r"#else\s+return \"([0-9.]+)\";", read("native/platform/sdk/net/online/np_app_version.h")).group(1)
        self.assertEqual(fallback, built)

    def test_release_files_agree(self):
        release = json.loads(read("cboc-release.json"))
        ver = dotted(self.app_ver)
        expected = ver.rsplit(".", 1)[0] if ver.endswith(".0") else ver
        self.assertEqual(release["version"], expected)
        text = read("docs/RELEASE.json")
        self.assertIn("--app-version " + self.app_ver, text)

    def test_the_game_sfo_gets_the_version(self):
        makefile = read("native/platform/vita/Makefile.vpk")
        self.assertRegex(makefile, r"vita-mksfoex[^\n]*-s APP_VER=\$\(NP_APP_VER\)[^\n]*\$\(TITLE_game\)")

    def test_the_screens_read_the_one_definition(self):
        identity = read("native/platform/vita/src/np_build_identity.h")
        self.assertIn("NpNet::BuiltAppVersion()", identity)
        for path in ("native/platform/vita/src/np_vita_main.cpp", "native/platform/vita/src/np_settings.cpp", "native/platform/vita/src/np_vita_settings.cpp"):
            self.assertIn("np_build_identity.h", read(path), path)
        self.assertIn("NpBuildIdentity::Version()", read("native/platform/vita/src/np_vita_main.cpp"))   # the log header and the basic record

    def test_repack_and_verify_refuse_a_stale_version(self):
        self.assertIn("--keep-donor-version", read("tools/repack-release.py"))
        self.assertIn("APP_VER is %s", read("native/tools/verify_vpk.py"))


if __name__ == "__main__":
    unittest.main()
