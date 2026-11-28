"""Host model of the vitaGL scene census (patch 0033): the census block of gxm.c is compiled on the host with the same variables and driven with scenes per picture.
A control mutant (the over-capacity test off by one) must fail the same drive."""
import pathlib, re, shutil, subprocess, tempfile, unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
GXM = (ROOT / "dependencies/vitaGL/source/gxm.c").read_text()
HDR = (ROOT / "dependencies/vitaGL/source/vitaGL.h").read_text()
CC = shutil.which("gcc") or shutil.which("cc")

DRIVE = r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define vgl_memset memset
%(typedef)s
static uint8_t gxm_display_rt_size = 1, gxm_fbo_rt_size = 8;
%(block)s
static int a, b, c;
int main(void) {
    vglSceneCensus s;
    vglGetSceneCensus(&s, 0);                      /* off: nothing counted */
    scn_begin(&a, 0); scn_frame_end(); vglGetSceneCensus(&s, 1);
    if (s.frames) return 10;
    vglSceneCensusSetup(1);
    /* picture 1: 8 scenes on target a (== capacity), 3 on b, 1 display: no overflow */
    for (int i = 0; i < 8; i++) scn_begin(&a, 0);
    for (int i = 0; i < 3; i++) scn_begin(&b, 0);
    scn_begin(&c, 1); scn_frame_end();
    vglGetSceneCensus(&s, 0);
    if (s.frames != 1 || s.worst_fbo != 8 || s.worst_fbo_targets != 2 || s.worst_display != 1 || s.over_fbo || s.over_display) return 11;
    /* picture 2: 9 on a (one past), 2 display scenes (one past) */
    for (int i = 0; i < 9; i++) scn_begin(&a, 0);
    scn_begin(&c, 1); scn_begin(&c, 1); scn_frame_end();
    vglGetSceneCensus(&s, 1);
    if (s.frames != 2 || s.worst_fbo != 9 || s.over_fbo != 1 || s.over_display != 1 || s.worst_display != 2 || s.cap_fbo != 8 || s.cap_display != 1) return 12;
    /* reset cleared; a new picture starts from zero counts per target */
    scn_begin(&a, 0); scn_frame_end(); vglGetSceneCensus(&s, 0);
    if (s.frames != 1 || s.worst_fbo != 1 || s.over_fbo) return 13;
    /* more than 32 targets in a picture */
    static int t[40]; vglGetSceneCensus(&s, 1);
    for (int i = 0; i < 40; i++) scn_begin(&t[i], 0);
    scn_frame_end(); vglGetSceneCensus(&s, 0);
    if (s.worst_fbo_targets != 32 || s.slots_full != 8) return 14;
    return 0;
}
'''


def build(block):
    typedef = re.search(r"typedef struct \{[^}]*\} vglSceneCensus;", HDR, re.S).group(0)
    src = DRIVE % {"typedef": typedef, "block": block}
    with tempfile.TemporaryDirectory() as d:
        f = pathlib.Path(d, "t.c")
        f.write_text(src)
        subprocess.run([CC, "-O1", "-o", str(pathlib.Path(d, "t")), str(f)], check=True, capture_output=True)
        return subprocess.run([str(pathlib.Path(d, "t"))]).returncode


def census_block():
    i = GXM.index("#define SCN_SLOTS")
    j = GXM.index("void vglGetSceneCensus")
    j = GXM.index("\n}\n", j) + 3
    return GXM[i:j]


class SceneCensus(unittest.TestCase):
    def test_counts(self):
        self.assertEqual(build(census_block()), 0)

    def test_mutant_is_caught(self):
        blk = census_block()
        self.assertIn("scn_count[i] > gxm_fbo_rt_size", blk)
        self.assertNotEqual(build(blk.replace("scn_count[i] > gxm_fbo_rt_size", "scn_count[i] >= gxm_fbo_rt_size")), 0)
        self.assertIn("scn_display > gxm_display_rt_size", blk)
        self.assertNotEqual(build(blk.replace("scn_display > gxm_display_rt_size", "scn_display > gxm_display_rt_size + 1")), 0)

    def test_hooks_are_in_the_scene_begin_paths_and_the_swap(self):
        self.assertEqual(GXM.count("scn_begin(gxm_render_target, 1);"), 1)
        self.assertEqual(GXM.count("scn_begin(scene_rt, 0);"), 1)
        self.assertEqual(GXM.count("scn_frame_end();"), 1)


if __name__ == "__main__":
    unittest.main()
