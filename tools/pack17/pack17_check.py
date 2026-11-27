#!/usr/bin/env python3
"""pack17_check.py <pack> <request.shl> <hashlist exe> [--base-pack <old pack>]: does the pack answer every shader of the request (by text hash)?
Exit 0: all answered. 1: some are not (printed). With --base-pack, also checks that every shader of the old pack is still in the new one (a pack only grows).
Used by tools/pack17/pack17_build.sh after the compile, and runnable on any machine: it reads files, compiles nothing."""
import os, struct, subprocess, sys

def index(path):
    d = open(path, 'rb').read()
    if d[:8] != b'VGXPACK1':
        sys.exit('%s: not a shader pack' % path)
    n, _ = struct.unpack('<II', d[8:16])
    return {(k, h) for h, k, _o, _s, _z in (struct.unpack('<QIIII', d[16 + 24 * i:16 + 24 * (i + 1)]) for i in range(n))}

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    base = sys.argv[sys.argv.index('--base-pack') + 1] if '--base-pack' in sys.argv else None
    if base:
        a.remove(base)
    if len(a) != 3:
        sys.exit(__doc__)
    pack, req, exe = a
    idx = index(pack)
    r = subprocess.run([exe, req], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        sys.exit(r.stderr)
    lack = []
    for line in r.stdout.splitlines():
        t, i, h, _th = line.split()
        if ((1 if t == 'fs' else 0), int(h, 16)) not in idx:
            lack.append((t, int(h, 16)))
    print('request shaders: %d, still not answered by the pack: %d' % (len(r.stdout.splitlines()), len(lack)))
    for t, h in lack:
        print('  NOT IN PACK: %s %016X' % (t, h))
    rc = 1 if lack else 0
    if base:
        gone = sorted(index(base) - idx)
        print('old pack shaders missing from the new pack: %d' % len(gone))
        rc = rc or (1 if gone else 0)
    return rc

if __name__ == '__main__':
    sys.exit(main())
