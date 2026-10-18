#!/usr/bin/env python3
"""Rewrites the material programs' GXSetArray calls to ask the model stream for its byte order.

    GXSetArray(GX_VA_POS, streams[0].address, 12);   ->   NP_GX_SETARRAY_STREAM(GX_VA_POS, streams[0], 12);

(platform/gx/include/np_gx.h: the stream's `unknown07` bit 0 says whether the array holds host-order values; docs/render.md,
"Endianness".) Every call in the game has this shape (186 sites in the 43 GX*MaterialProgram.cpp files; one writes
`packet->streams[N]`); the script refuses to leave any GXSetArray call unconverted, so a new call site of another shape is
an error and not a silent big-endian array.

    gen_stream_patch.py <np root>            rewrite in place (a git checkout: `git diff` is the patch)
    gen_stream_patch.py --check <np root>    count the sites, change nothing
"""
import re
import sys
from pathlib import Path

CALL = re.compile(r'\bGXSetArray\(\s*(GX_VA_\w+)\s*,\s*((?:\w+->)?streams\[\d+\])\.address\s*,\s*(\w+)\s*\)')
ANY = re.compile(r'\bGXSetArray\s*\(')


def main(argv):
    check = '--check' in argv
    args = [a for a in argv[1:] if a != '--check']
    if len(args) != 1:
        print(__doc__)
        return 2
    root = Path(args[0])
    sites = files = 0
    for path in sorted((root / 'src').rglob('*.cpp')):
        text = path.read_text()
        if not ANY.search(text):
            continue
        new, n = CALL.subn(r'NP_GX_SETARRAY_STREAM(\1, \2, \3)', text)
        left = len(ANY.findall(new))
        if left:
            print('%s: %d GXSetArray call(s) of an unknown shape remain' % (path, left))
            return 1
        sites += n
        files += 1
        if not check:
            path.write_text(new)
    print('%s %d sites in %d files' % ('would convert' if check else 'converted', sites, files))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
