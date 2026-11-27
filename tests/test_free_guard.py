"""Host model of the vitaGL free guard (patch 0034): the guard block of gxm.c compiled on the host with stand-ins for the texture and framebuffer tables, driven with deletes and swaps.
Control mutants (a delete that is not held, one that is held too short, a missing once-per-site rule, a missing reason) must fail the same drive."""
import pathlib, shutil, subprocess, tempfile, unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
GXM = (ROOT / "dependencies/vitaGL/source/gxm.c").read_text()
HDR = (ROOT / "dependencies/vitaGL/source/vitaGL.h").read_text()
CC = shutil.which("gcc") or shutil.which("cc")

DRIVE = r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef int GLsizei; typedef unsigned GLuint; typedef unsigned char GLboolean;
#define FRAME_PURGE_FREQ 4
#define TEXTURES_NUM 64
enum { TEX_UNUSED, TEX_VALID };
typedef struct texture { uint32_t fg_rt_frame; int status; } texture;
typedef struct framebuffer { GLboolean active; texture *tex; } framebuffer;
static texture texture_slots[TEXTURES_NUM];
static framebuffer *active_write_fb, *active_read_fb;
static uint32_t vgl_framecount;
static int frame_elem_purge_idx, frame_rt_purge_idx;
%(types)s
static int real_tex[64], real_n; static GLuint real_fb[16]; static int real_fbn;
void glDeleteTextures(GLsizei n, const GLuint *ids);
void glDeleteFramebuffers(GLsizei n, const GLuint *ids);
%(block)s
void glDeleteTextures(GLsizei n, const GLuint *ids) {
    if (vgl_fg_on && !vgl_fg_flushing && vgl_fg_delete(0, n, ids, __builtin_return_address(0))) return;
    for (int i = 0; i < n; i++) real_tex[real_n++] = ids[i] | (vgl_framecount << 8);
}
void glDeleteFramebuffers(GLsizei n, const GLuint *ids) {
    if (vgl_fg_on && !vgl_fg_flushing && vgl_fg_delete(1, n, ids, __builtin_return_address(0))) return;
    for (int i = 0; i < n; i++) real_fb[real_fbn++] = ids[i];
}
static void swap(void) { if (vgl_fg_on) fg_flush(); vgl_framecount++; }
static void del_site_a(GLuint id) { glDeleteTextures(1, &id); }
static void del_site_b(GLuint id) { glDeleteTextures(1, &id); }
static void del_site_c(GLuint id) { glDeleteTextures(1, &id); }
int main(void) {
    vglFreeGuardEvent e; vglFreeGuardStats st;
    for (int i = 1; i < 20; i++) texture_slots[i].status = TEX_VALID;
    /* off: a delete is immediate and nothing is recorded */
    del_site_a(1);
    if (real_n != 1 || (real_tex[0] & 255) != 1) return 10;
    if (vglFreeGuardTake(&e)) return 11;
    /* on, hold 3 */
    vglSetFreeGuard(3);
    vgl_framecount = 10;
    del_site_a(2);                                      /* never drawn into: held, not reported */
    if (real_n != 1) return 12;
    if (vglFreeGuardTake(&e)) return 13;
    texture_slots[3].fg_rt_frame = 10 + 1;              /* a scene drew into 3 this present */
    del_site_b(3);
    if (!vglFreeGuardTake(&e) || e.kind != 0 || e.id != 3 || !(e.reason & 1) || e.age != 0) return 14;
    del_site_b(3);                                       /* the same site again: once only */
    if (vglFreeGuardTake(&e)) return 15;
    static framebuffer fb; fb.active = 1; fb.tex = &texture_slots[4]; active_write_fb = &fb;
    del_site_a(4);                                       /* colour of the bound framebuffer, other site: reported */
    if (!vglFreeGuardTake(&e) || e.id != 4 || !(e.reason & 2)) return 16;
    GLuint f = (GLuint)(uintptr_t)&fb;
    glDeleteFramebuffers(1, &f);                         /* the framebuffer is bound: reported (kind 1) */
    if (!vglFreeGuardTake(&e) || e.kind != 1) return 17;
    /* held for 3 presents: due at 10+3 */
    swap(); swap();                                      /* frames 10, 11 flushed with framecount 10, 11 */
    if (real_n != 1) return 18;
    swap();                                              /* flush at framecount 12: not yet */
    if (real_n != 1) return 19;
    swap();                                              /* flush at framecount 13: due */
    if (real_n != 1 + 4) return 20;                      /* 2, 3, 3, 4 */
    if (real_fbn != 1) return 21;
    vglGetFreeGuardStats(&st);
    if (st.deletes != 5 || st.referenced != 4 || st.sites != 3) return 22;
    /* a drawn-into texture whose scene is older than FRAME_PURGE_FREQ is not reported */
    texture_slots[5].fg_rt_frame = 1; vgl_framecount = 20;
    del_site_c(5);
    if (vglFreeGuardTake(&e)) return 23;
    frame_elem_purge_idx = 20000; frame_rt_purge_idx = 7; swap();
    vglGetFreeGuardStats(&st);
    if (st.purge_peak != 20000 || st.rt_purge_peak != 7) return 25;
    vglSetFreeGuard(0);
    del_site_a(6);
    if (real_n != 5 + 1 + 0 && real_tex[real_n - 1] != 6) return 24;
    return 0;
}
'''

DRIVE_REF = DRIVE.split("int main(void) {")[0] + r'''
int main(void) {
    vglFreeGuardEvent e; vglFreeGuardStats st;
    for (int i = 1; i < 20; i++) texture_slots[i].status = TEX_VALID;
    vglSetFreeGuard(3); vglSetFreeGuardReferencedOnly(1);
    vgl_framecount = 10;
    del_site_a(2);                                      /* never drawn into: deleted at once, exactly as with no guard, nothing recorded */
    if (real_n != 1 || (real_tex[0] & 255) != 2) return 10;
    if (vglFreeGuardTake(&e)) return 11;
    texture_slots[3].fg_rt_frame = 11;                  /* a scene drew into 3 this present: held */
    del_site_b(3);
    if (real_n != 1) return 12;
    if (!vglFreeGuardTake(&e) || e.id != 3 || !(e.reason & 1)) return 13;
    static framebuffer fb; fb.active = 1; fb.tex = &texture_slots[4]; active_write_fb = &fb;
    del_site_a(4);                                      /* colour of the bound framebuffer, never drawn into (no stamp): immediate (0050) */
    if (real_n != 2) return 14;
    GLuint f = (GLuint)(uintptr_t)&fb;
    glDeleteFramebuffers(1, &f);
    if (real_fbn != 1) return 15;
    active_write_fb = 0;
    texture_slots[6].fg_rt_frame = 1;                   /* drawn into long ago: immediate */
    del_site_c(6);
    if (real_n != 3) return 16;
    GLuint two[2] = {7, 3};                             /* one hazardous name holds the whole call */
    glDeleteTextures(2, two);
    if (real_n != 3) return 17;
    swap(); swap(); swap();                             /* held at 10, due 13: not yet at 12 */
    if (real_n != 3) return 18;
    swap();
    if (real_n != 3 + 3) return 19;                     /* 3 (first call), 7, 3 */
    vglGetFreeGuardStats(&st);
    if (st.deletes != 3 || st.postponed != 3) return 20;   /* the immediate ones are not counted */
    /* the allocation recovery (full memory): a held delete is done at once by vgl_fg_flush_all, once */
    texture_slots[8].fg_rt_frame = vgl_framecount + 1;
    del_site_a(8);
    if (real_n != 6) return 21;
    vgl_fg_flush_all();
    if (real_n != 7 || (real_tex[6] & 255) != 8) return 22;
    vglGetFreeGuardStats(&st);
    if (st.recovery_flushed != 1) return 23;
    vgl_fg_flush_all();
    if (real_n != 7) return 24;
    return 0;
}
'''

def build(block, drive=None):
    import re
    types = re.search(r"typedef struct \{[^}]*\} vglFreeGuardStats;", HDR, re.S).group(0) + "\n" + re.search(r"typedef struct \{\s*uint32_t kind;[^}]*\} vglFreeGuardEvent;", HDR, re.S).group(0)
    src = (drive or DRIVE) % {"types": types, "block": block}
    with tempfile.TemporaryDirectory() as d:
        f = pathlib.Path(d, "t.c")
        f.write_text(src)
        subprocess.run([CC, "-O0", "-w", "-no-pie", "-fno-pie", "-o", str(pathlib.Path(d, "t")), str(f)], check=True, capture_output=True)
        return subprocess.run([str(pathlib.Path(d, "t"))]).returncode


def block():
    i = GXM.index("int vgl_fg_on = 0, vgl_fg_flushing = 0;")
    j = GXM.index("void vgl_fg_flush_all(void) {")
    j = GXM.index("\n}\n", j) + 3
    return GXM[i:j]


class FreeGuard(unittest.TestCase):
    def test_model(self):
        self.assertEqual(build(block()), 0)

    def test_mutants_are_caught(self):
        b = block()
        for old, new in (("fg_ring[fg_ring_n++].due = vgl_framecount + fg_hold;", "fg_ring[fg_ring_n++].due = vgl_framecount;"),
                         ("fg_ring[fg_ring_n++].due = vgl_framecount + fg_hold;", "fg_ring[fg_ring_n++].due = vgl_framecount + fg_hold - 1;"),
                         ("if (fg_site[i] == (uintptr_t)caller && fg_site_kind[i] == kind)\n\t\t\treturn;", "if (0)\n\t\t\treturn;"),
                         ("if (active_write_fb && active_write_fb->tex == tex) reason |= 2;", ""),
                         ("<= FRAME_PURGE_FREQ) {\n\t\t\treason |= 1;", "<= 100) {\n\t\t\treason |= 1;"),
                         ("if ((uint32_t)frame_elem_purge_idx > fg_stats.purge_peak)", "if (0)")):
            self.assertIn(old, b)
            self.assertNotEqual(build(b.replace(old, new)), 0, old)

    def test_referenced_only_mode(self):
        b = block()
        self.assertEqual(build(b, DRIVE_REF), 0)
        for old, new in (("if (!any)\n\t\t\treturn 0;", ""),                                                  # holds every delete again
                         ("if (tex && tex->fg_rt_frame && vgl_framecount + 1 - tex->fg_rt_frame <= FRAME_PURGE_FREQ) any = 1;", ""),   # lets a drawn-into texture go at once
                         ("// (0050: the bound framebuffer's colour alone", "if (active_write_fb && active_write_fb->tex == tex) any = 1; // ("),   # a bound colour holds again (4b)
                         ("\tfg_stats.recovery_flushed += fg_ring_n;\n\tfg_ring_n = 0;", "\tfg_ring_n = 0;"),   # no count
                         ("\t\tif (fg_ring[i].kind == 0)\n\t\t\tglDeleteTextures(1, &id);\n\t\telse\n\t\t\tglDeleteFramebuffers(1, &id);\n\t}\n\tfg_stats.recovery", "\t}\n\tfg_stats.recovery"),   # the recovery flush deletes nothing (4a)
                         ("for (GLsizei j = 0; j < n && !any; j++) {", "for (GLsizei j = 0; j < 1 && !any; j++) {")):    # looks at the first name only
            self.assertIn(old, b)
            self.assertNotEqual(build(b.replace(old, new, 1), DRIVE_REF), 0, old)

    def test_hooks_are_in_the_delete_functions_and_the_swap(self):
        self.assertIn("vgl_fg_delete(0, n, gl_textures, __builtin_return_address(0))", (ROOT / "dependencies/vitaGL/source/textures.c").read_text())
        self.assertIn("vgl_fg_delete(1, n, ids, __builtin_return_address(0))", (ROOT / "dependencies/vitaGL/source/framebuffers.c").read_text())
        self.assertEqual(GXM.count("fg_flush(); // 0034"), 1)
        self.assertIn("vgl_fg_flush_all(); // 0050", (ROOT / "dependencies/vitaGL/source/utils/gpu_utils.c").read_text())   # the recovery's finish step flushes (4a)
        self.assertIn("texture_slots[i].fg_rt_frame = 0;", (ROOT / "dependencies/vitaGL/source/textures.c").read_text())   # 4c
        self.assertEqual(GXM.count("fg_rt_frame = vgl_framecount + 1; // 0034"), 1)


if __name__ == "__main__":
    unittest.main()
