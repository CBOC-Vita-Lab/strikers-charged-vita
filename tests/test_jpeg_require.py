"""Hardware JPEG must be linked (galcheck, wave 9): the four sceJpeg calls are weak declarations in thp_jpeg_vita.cpp, -lSceJpeg_stub_weak is an archive, and a weak
reference never pulls an archive member: image r7 had all four null and the default-on MSC_THP_HWJPEG never opened. Makefile.vpk requires them for the game image."""
import pathlib, re, shutil, subprocess, tempfile, unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
MK = (ROOT / "native/platform/vita/Makefile.vpk").read_text()
SRC = (ROOT / "native/platform/sdk/src/thp_jpeg_vita.cpp").read_text()
GCC = shutil.which("arm-vita-eabi-gcc") or "/opt/vitasdk/bin/arm-vita-eabi-gcc"
NM = shutil.which("arm-vita-eabi-nm") or "/opt/vitasdk/bin/arm-vita-eabi-nm"


def listed():
    m = re.search(r"^NP_JPEG_SYMS\s*:=\s*(.+)$", MK, re.M)
    return m.group(1).split() if m else []


class JpegRequire(unittest.TestCase):
    def test_list_is_the_weak_declarations(self):
        weak = set(re.findall(r"^int (sceJpeg\w+)\(.*__attribute__\(\(weak\)\)|^int (sceJpeg\w+)\(", SRC, re.M))
        names = {a or b for a, b in weak}
        self.assertEqual(set(listed()), names)
        self.assertEqual(len(names), 4)

    def test_game_image_requires_them_and_the_stub_stays_weak(self):
        self.assertRegex(MK, r"mscharged-np\.elf: NP_REQUIRE := .*\$\(NP_JPEG_REQUIRE\)")
        self.assertIn("-lSceJpeg_stub_weak", (ROOT / "native/platform/vita/vita.mk").read_text())
        self.assertNotRegex((ROOT / "native/platform/vita/vita.mk").read_text(), r"-lSceJpeg_stub(?!_weak)")

    @unittest.skipUnless(pathlib.Path(GCC).exists(), "no vitasdk")
    def test_mini_link_with_and_without_require(self):
        with tempfile.TemporaryDirectory() as d:
            c = pathlib.Path(d, "t.c")
            c.write_text("int sceJpegFinishMJpeg(void) __attribute__((weak));\nint main(void){ return sceJpegFinishMJpeg ? 1 : 0; }\n")
            def link(extra):
                out = str(pathlib.Path(d, "t.elf"))
                subprocess.run([GCC, str(c), "-o", out, "-lSceJpeg_stub_weak"] + extra, check=True, capture_output=True)
                return subprocess.run([NM, out], capture_output=True, text=True).stdout
            self.assertNotRegex(link([]), r"(?m) [TtW] sceJpegFinishMJpeg$")           # the control: without the flag it stays null
            self.assertRegex(link(["-Wl,--require-defined=sceJpegFinishMJpeg"]), r"(?m) [TtW] sceJpegFinishMJpeg$")


if __name__ == "__main__":
    unittest.main()
