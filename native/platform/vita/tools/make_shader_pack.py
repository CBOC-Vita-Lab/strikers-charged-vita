#!/usr/bin/env python3
"""W5 BOOT: the package's compiled shaders as ONE file per cache key (vgx vita/vgx_host_vita.cpp "the package's compiled shaders as ONE file").

    make_shader_pack.py <seed shadercache dir> <out dir>

<seed>/<key>/v2/{v,f}/<HH>/<HASH>.gxp (vitaGL's cache layout, NP_SHADER_SEED) -> <out>/<key>.pack, and <seed>/vgx_shaderlist.bin is copied to
<out>/. Pack: b"VGXPACK1", u32 count, u32 data start; count x (u64 hash, u32 kind 0 vertex / 1 fragment, u32 offset, u32 size, u32 0), sorted
by (kind, hash); then the files' bytes, each at a 16-byte aligned offset. A file whose name does not match its bucket is refused.
"""
import os, shutil, struct, sys

def pack_key(keydir, out):
    entries = []
    for kind, sub in ((0, 'v'), (1, 'f')):
        base = os.path.join(keydir, 'v2', sub)
        if not os.path.isdir(base):
            continue
        for bucket in sorted(os.listdir(base)):
            bdir = os.path.join(base, bucket)
            for name in sorted(os.listdir(bdir)):
                if not name.endswith('.gxp'):
                    continue
                h = int(name[:-4], 16)
                if '%02X' % (h >> 56) != bucket:
                    sys.exit('%s: not in its bucket' % os.path.join(bdir, name))
                with open(os.path.join(bdir, name), 'rb') as f:
                    entries.append((kind, h, f.read()))
    entries.sort(key=lambda e: (e[0], e[1]))
    start = 16 + 24 * len(entries)
    start = (start + 15) & ~15
    index, blob, off = [], bytearray(), start
    for kind, h, data in entries:
        index.append(struct.pack('<QIIII', h, kind, off, len(data), 0))
        blob += data
        pad = (-len(data)) & 15
        blob += b'\0' * pad
        off += len(data) + pad
    with open(out, 'wb') as f:
        f.write(b'VGXPACK1' + struct.pack('<II', len(entries), start))
        f.write(b''.join(index))
        f.write(b'\0' * (start - 16 - 24 * len(entries)))
        f.write(blob)
    return len(entries), os.path.getsize(out)

def main():
    seed, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for key in sorted(os.listdir(seed)):
        kd = os.path.join(seed, key)
        if os.path.isdir(kd) and len(key) == 8:
            n, size = pack_key(kd, os.path.join(out, key + '.pack'))
            print('shader pack %s: %d shaders, %d bytes' % (key, n, size))
    lst = os.path.join(seed, 'vgx_shaderlist.bin')
    if os.path.exists(lst):
        shutil.copyfile(lst, os.path.join(out, 'vgx_shaderlist.bin'))

if __name__ == '__main__':
    main()
