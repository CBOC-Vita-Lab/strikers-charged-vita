"""bg30c (17 Nov 2026): the two reasons MSC_BG30 never engaged on the real Vita (build s28-int2).
1. The copy program's varying and attribute names must be ones the console's GLSL translator has a TEXCOORD slot for (vitaGL global semantic mode binds v_tex0..v_tex7, v_col0, v_col1: any other
   varying name gets no slot and the text does not compile: "invalid token TEXCOORD/ created by paste operator").
2. MSC_BG30 on makes NpViewLayers() 1 (forked frames send shadows of the layer views: without it every draw class is 0 and the plan finds no background prefix)."""
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
BOUND = {"v_tex%d" % i for i in range(8)} | {"v_col0", "v_col1"}


class Bg30Console(unittest.TestCase):
    def test_shader_names(self, src=None):
        text = src if src is not None else (ROOT / "renderer/src/vgx_bg30.h").read_text()
        block = text[text.index("void EnsureProgram()"):text.index("const GLuint v = Compile(GL_VERTEX_SHADER")]
        names = set(re.findall(r"varying\s+(?:\w+\s+)*vec\d\s+(\w+)\s*;", block))
        self.assertTrue(names, "no varying found")
        self.assertLessEqual(names, BOUND, "a varying the console translator cannot bind: %s" % sorted(names - BOUND))
        self.assertNotIn("highp vec", re.sub(r"precision highp float", "", " ".join(re.findall(r"varying[^;]*;", block))))

    @unittest.skipUnless(shutil.which("gcc"), "no host compiler")
    def test_view_layers_follow_bg30(self):
        prog = '#include <stdio.h>\n#include "np/amortize.h"\nint main(void){printf("%d\\n", NpViewLayers());return 0;}\n'
        with tempfile.TemporaryDirectory() as tmp:
            c = pathlib.Path(tmp) / "t.c"
            c.write_text(prog)
            exe = pathlib.Path(tmp) / "t"
            subprocess.run(["gcc", "-I", str(ROOT / "native/include"), str(c), "-o", str(exe)], check=True)
            def run(**env):
                e = {k: v for k, v in os.environ.items() if k not in ("MSC_BG30", "MSC_VIEW_LAYERS")}
                e.update(env)
                return subprocess.run([str(exe)], capture_output=True, text=True, env=e).stdout.strip()
            self.assertEqual(run(), "0")
            self.assertEqual(run(MSC_BG30="0"), "0")
            self.assertEqual(run(MSC_BG30="off"), "0")
            self.assertEqual(run(MSC_BG30="2"), "1")
            self.assertEqual(run(MSC_BG30="1"), "1")
            self.assertEqual(run(MSC_VIEW_LAYERS="1"), "1")

    def test_mutant_shader_name_caught(self):
        text = (ROOT / "renderer/src/vgx_bg30.h").read_text().replace("v_tex0", "v_tc")
        with self.assertRaises(AssertionError):
            self.test_shader_names(text)


if __name__ == "__main__":
    unittest.main()
