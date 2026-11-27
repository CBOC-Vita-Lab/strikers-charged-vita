#!/usr/bin/env python3
"""W5 BOOT: the package's compiled shaders as ONE file per cache key (vgx vita/vgx_host_vita.cpp "the package's compiled shaders as ONE file").

    make_shader_pack.py [--base <dir with <key>.pack files> [--base-key <older key>]] <seed shadercache dir> <out dir>

<seed>/<key>/v3/{v,f}/<HH>/<HASH>.gxp (vitaGL's cache layout, NP_SHADER_SEED; v3 with 16-digit names since vitaGL 9a14d17, v2 with short names before: both are read, a hash in both is taken once) -> <out>/<key>.pack, and <seed>/vgx_shaderlist.bin is copied to
<out>/. Pack: b"VGXPACK1", u32 count, u32 data start; count x (u64 hash, u32 kind 0 vertex / 1 fragment, u32 offset, u32 size, u32 0), sorted
by (kind, hash); then the files' bytes, each at a 16-byte aligned offset. A file whose name does not match its bucket is refused.

--base <dir> (1.2): the shaders of an earlier package are kept. For each key the seed holds, <dir>/<key>.pack (for example the shadercache/ folder of the
previous release's VPK, unzipped) is read first and the seed's shaders are added to it (a hash in both is the seed's file: the two are the same shader, the hash is
the text's); a <key>.pack in <dir> with no folder in the seed is copied as it is. So a seed made by a delta compile of only the NEW shaders gives the whole pack.
--base-key <older key> (the cache key changes with the vitaGL id string: 1.0 and 1.1 packs are filed under bc447993, see docs/BUILDING.md): a seed key that has
no <key>.pack in the base takes <older key>.pack instead, and that file is then not copied on its own (the new pack replaces it).
"""
import os, shutil, struct, sys

def read_pack(path):
    """-> {(kind, hash): bytes} of a pack file (the format above)."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'VGXPACK1' or len(data) < 16:
        sys.exit('%s: not a shader pack' % path)
    count, _start = struct.unpack('<II', data[8:16])
    out = {}
    for i in range(count):
        h, kind, off, size, _zero = struct.unpack('<QIIII', data[16 + 24 * i:16 + 24 * (i + 1)])
        if kind > 1 or off + size > len(data):
            sys.exit('%s: entry %d is outside the file' % (path, i))
        out[(kind, h)] = data[off:off + size]
    return out

def pack_key(keydir, out, basepack=None):
    entries = []
    seen = set()
    for version in ('v3', 'v2'):   # (the newer layout first: its file wins if a hash is in both)
        for kind, sub in ((0, 'v'), (1, 'f')):
            base = os.path.join(keydir, version, sub)
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
                    if (kind, h) in seen:
                        continue
                    seen.add((kind, h))
                    with open(os.path.join(bdir, name), 'rb') as f:
                        entries.append((kind, h, f.read()))
    from_seed = len(entries)
    if basepack:
        for (kind, h), data in read_pack(basepack).items():
            if (kind, h) not in seen:
                seen.add((kind, h))
                entries.append((kind, h, data))
    entries.sort(key=lambda e: (e[0], e[1]))
    pack_key.last_from_base = len(entries) - from_seed
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
    args = sys.argv[1:]
    base = older = None
    while len(args) >= 2 and args[0] in ('--base', '--base-key'):
        if args[0] == '--base':
            base = args[1]
        else:
            older = args[1]
        args = args[2:]
    if len(args) != 2 or (older and not base):
        sys.exit(__doc__)
    seed, out = args
    os.makedirs(out, exist_ok=True)
    keys = []
    used = set()
    for key in sorted(os.listdir(seed)):
        kd = os.path.join(seed, key)
        if os.path.isdir(kd) and len(key) == 8:
            keys.append(key)
            bp = None
            if base:
                for cand in (key, older):
                    if cand and os.path.exists(os.path.join(base, cand + '.pack')):
                        bp = os.path.join(base, cand + '.pack')
                        used.add(cand)
                        break
            n, size = pack_key(kd, os.path.join(out, key + '.pack'), bp)
            print('shader pack %s: %d shaders (%d kept from the base pack%s), %d bytes' % (key, n, pack_key.last_from_base, ' ' + os.path.basename(bp) if bp else ': none', size))
    if base:   # a pack of the base that no seed key took: kept as it is
        for name in sorted(os.listdir(base)):
            if name.endswith('.pack') and name[:-5] not in keys and name[:-5] not in used and len(name) == 13:
                shutil.copyfile(os.path.join(base, name), os.path.join(out, name))
                print('shader pack %s: copied from the base' % name[:-5])
    lst = os.path.join(seed, 'vgx_shaderlist.bin')
    if os.path.exists(lst):
        shutil.copyfile(lst, os.path.join(out, 'vgx_shaderlist.bin'))
    elif base and os.path.exists(os.path.join(base, 'vgx_shaderlist.bin')):
        shutil.copyfile(os.path.join(base, 'vgx_shaderlist.bin'), os.path.join(out, 'vgx_shaderlist.bin'))

if __name__ == '__main__':
    main()
