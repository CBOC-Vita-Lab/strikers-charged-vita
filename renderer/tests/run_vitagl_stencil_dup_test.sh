#!/bin/bash
# vitaGL 0022 (stencil leak): a surface's stencil must go to the garbage collector exactly once. The collector frees every entry of the purge list without a
# duplicate check, so a second mark of the same pointer is a double free (a console's dlmalloc corrupts its pool or aborts; the emulator may not notice).
# This test takes the real macros from dependencies/vitaGL/source/utils/mem_utils.h and, for every function of the library's sources that uses
# mark_depth_stencil_as_dirty, requires that no explicit mark of ->stencilData follows in the same function. It then runs the macro over a model of the
# purge list (a free() that counts) and requires one free per pointer. BREAK=1 puts the second mark back (the test must fail).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
SRC=${VITAGL_SRC:-$VGX/../dependencies/vitaGL/source}
BUILD=${TEST_BUILD:-$(mktemp -d)}
mkdir -p "$BUILD"
python3 - "$SRC" "$BUILD" "${BREAK:-0}" <<'PY'
import re, sys, os
src, build, brk = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
mu = open(os.path.join(src, "utils/mem_utils.h")).read()
m = re.search(r"#define mark_depth_stencil_as_dirty\(s\).*?while \(0\)", mu, re.S)
assert m, "macro not found"
macro = m.group(0)
bad = 0
users = 0
for name in os.listdir(src):
    if not name.endswith((".c", ".h")):
        continue
    text = open(os.path.join(src, name)).read()
    if brk and name == "framebuffers.c":
        text = text.replace("mark_depth_stencil_as_dirty(fb->depthbuffer_ptr);\n#endif\n\t\tfb->depthbuffer_ptr = NULL;\n\t}\n\tfb->depthbuffer_state",
                            "mark_depth_stencil_as_dirty(fb->depthbuffer_ptr);\n\t\tif (fb->depthbuffer_ptr->stencilData)\n\t\t\tmark_as_dirty(fb->depthbuffer_ptr->stencilData);\n#endif\n\t\tfb->depthbuffer_ptr = NULL;\n\t}\n\tfb->depthbuffer_state")
    # split into top-level functions by a column-0 closing brace
    for fn in re.split(r"\n\}\n", text):
        if "mark_depth_stencil_as_dirty(" not in fn or "#define mark_depth_stencil_as_dirty" in fn:
            continue
        users += 1
        if re.search(r"mark_as_dirty\([^;]*stencilData\)", fn):
            head = fn.strip().splitlines()[-1][:60] if fn.strip() else ""
            first = [l for l in fn.splitlines() if re.match(r"^[A-Za-z].*\(.*\)\s*\{", l)]
            print("FAIL: %s: a function marks the stencil twice: %s" % (name, first[-1] if first else head))
            bad += 1
print("functions using the macro: %d, marking twice: %d" % (users, bad))
if users < 5: print("FAIL: expected the macro in at least 5 functions"); bad += 1
# the model
t = r'''
#include <stdio.h>
#include <stdlib.h>
#define FRAME_PURGE_LIST_SIZE 64
static void *frame_purge_list[1][FRAME_PURGE_LIST_SIZE]; static int frame_purge_idx = 0, frame_elem_purge_idx = 0;
#define mark_as_dirty(x) frame_purge_list[frame_purge_idx][frame_elem_purge_idx++] = x
typedef struct { void *depthData, *stencilData; } S;
''' + macro + r'''
int main(void) {
  S s = { malloc(4), malloc(4) };
  mark_depth_stencil_as_dirty(&s);
  S d = { malloc(4), NULL };
  mark_depth_stencil_as_dirty(&d);
  int dup = 0;
  for (int i = 0; i < frame_elem_purge_idx; i++)
    for (int j = i + 1; j < frame_elem_purge_idx; j++) if (frame_purge_list[0][i] == frame_purge_list[0][j]) dup++;
  printf("model: %d entries, %d duplicates\n", frame_elem_purge_idx, dup);
  return !(frame_elem_purge_idx == 3 && dup == 0);
}
'''
open(os.path.join(build, "m.c"), "w").write(t)
sys.exit(1 if bad else 0)
PY
cc -O1 -o "$BUILD/m" "$BUILD/m.c" && "$BUILD/m"
echo "PASS vitagl stencil single mark"
