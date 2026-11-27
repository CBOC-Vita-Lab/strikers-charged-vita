#!/usr/bin/env python3
"""pack17_request.py: the shader texts the shader pack lacks on the console routes, as a request for the owner's pack-17 compile.

Nothing is compiled here and no shader compiler (libshacccg) is used or needed. Inputs:
  * the pack the console ships (<key>.pack; its index says which shader texts it answers),
  * console logs: 'shader pack MISS #n: <kind> shader <HASH>' lines (vgx_host_vita.cpp) and '[shaderplay] ... (text hash <HASH>) took N ms' lines
    (vgx_backend_gl.cpp), in any file under the given paths,
  * shader lists (VGXSHL01, MSC_VGX_SHADERLIST, the emulator profiles' vgx_shaderlist.bin and the helpers' list.shl files): the keys each emulator/host run asked for,
  * shader text logs (MSC_VGX_SHADERTEXT_LOG=<file>, one per route, named <route>.texts): every text the renderer handed the compiler on a host run, including the back end's
    own programs (picture, copy, effects), which no list holds.
A console log gives only the hash of a shader text, never its key or its text. There are two hashes: the pack MISS line's is P = XXH3-64 of "#version 100\\n" + text (what the pack
indexes), the [shaderplay] line's is T = XXH3-64 of the text alone. Both are worked out for every key of every list (tools/pack17/hashlist, a 32-bit build of the renderer's own
generator) and every text of every text log, so a console miss becomes a key (a generated shader) or a text and the routes that make it (a back end program). A miss no
input holds is UNRESOLVED: record the route on the host (tools/pack17/pack17_record.sh) or in Vita3K with MSC_VGX_SHADERLIST / MSC_VGX_SHADERTEXT_LOG and give that file here.

Outputs in --out (every file is the same bytes on every run with the same inputs):
  pack17-request.shl   a shader list: every shader key the pack lacks that a console log or a list names, with the programs and variants that use it. The owner's
                       machine compiles this list (tools/pack17/pack17_build.sh) and merges the result into the old pack.
  pack17-request.txt   the same as a table: kind, hash, where it was seen (console log files / lists / routes), the longest compile seen in play (ms, console), and the UNRESOLVED ones.
  pack17-routes.txt    the routes the owner's machine must PLAY in Vita3K after the list is warmed, because the request holds back end programs (no key: the game compiles them when it
                       first draws them): one repo-relative route path a line.
  pack17-request-texts.txt  the texts of the back end programs the pack lacks.
Exit 0 when the request is written (also when it is empty: then the pack answers everything given), 2 on a usage or input error.

  pack17_request.py --pack pack16/874a13e0.pack --hashlist ./hashlist --console w8/console --lists a.shl b.shl --out out/
"""
import argparse, collections, glob, hashlib, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'native', 'tools', 'seed'))
import shl  # noqa: E402

MISS = re.compile(r'shader pack MISS #\d+: (fragment|vertex) shader ([0-9A-Fa-f]{16}) asked for at present (\d+)')
PLAY = re.compile(r'\[shaderplay\] picture (\d+): a (vertex|fragment) shader \(text hash ([0-9A-Fa-f]{16})\) took ([0-9.]+) ms')
KIND = {'vertex': 0, 'vs': 0, 'fragment': 1, 'fs': 1}
# the route each text log's name stands for (repo-relative); a name not here is printed as it is
ROUTES = {'dump': 'tools/console-1.2/fastboot/routes/fb-dump.txt', 'lava': 'tools/console-1.2/fastboot/routes/fb-lava.txt', 'mega': 'tools/console-1.2/fastboot/routes/fb-mega.txt',
          'replay': 'tools/console-1.2/fastboot/routes/fb-replay.txt', 'crep': 'tools/console-1.2/fastboot/routes/fb-crep.txt', 'pipe': 'tools/console-1.2/fastboot/routes/fb-pipe.txt',
          'tour': 'tools/console-1.2/fastboot/routes/fb-tour.txt', 'sand': 'tools/pack17/routes/fb-sand.txt'}


def pack_index(path):
    d = open(path, 'rb').read()
    if d[:8] != b'VGXPACK1':
        sys.exit('%s: not a shader pack' % path)
    n, _start = struct.unpack('<II', d[8:16])
    idx = set()
    for i in range(n):
        h, kind, off, size, _z = struct.unpack('<QIIII', d[16 + 24 * i:16 + 24 * (i + 1)])
        idx.add((kind, h))
    return idx, hashlib.sha256(d).hexdigest()[:16], n


def expand(paths, suffixes):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for dp, _dn, fn in os.walk(p):
                files += [os.path.join(dp, f) for f in fn if f.endswith(suffixes)]
        else:
            files += glob.glob(p) or [p]
    return sorted(set(files))


def console_misses(paths):
    """-> ({(kind, P): entry}, {(kind, T): entry}); entry: files, ms (longest compile in play), present"""
    P = collections.defaultdict(lambda: {'files': set(), 'ms': 0.0, 'present': set()})
    T = collections.defaultdict(lambda: {'files': set(), 'ms': 0.0, 'present': set()})
    for f in expand(paths, ('.log', '.txt', '.out')):
        try:
            text = open(f, errors='replace').read()
        except OSError:
            continue
        name = os.path.basename(os.path.dirname(f)) + '/' + os.path.basename(f)
        for m in MISS.finditer(text):
            e = P[(KIND[m.group(1)], int(m.group(2), 16))]
            e['files'].add(name)
            e['present'].add(int(m.group(3)))
        for m in PLAY.finditer(text):
            e = T[(KIND[m.group(2)], int(m.group(3), 16))]
            e['files'].add(name)
            e['ms'] = max(e['ms'], float(m.group(4)))
    return P, T


def list_hashes(hashlist, path):
    """-> ((SList, {(kind, P): key}, {(kind, T): P}), '') or (None, why) when the list is from another generator"""
    r = subprocess.run([hashlist, path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        return None, r.stderr.strip()
    L = shl.read(path)
    m, t = {}, {}
    for line in r.stdout.splitlines():
        k, i, p, th = line.split()
        kind = 0 if k == 'vs' else 1
        m[(kind, int(p, 16))] = (L.vs if kind == 0 else L.fs)[int(i)]
        t[(kind, int(th, 16))] = int(p, 16)
    return (L, m, t), ''


def read_texts(path):
    """-> {(kind, P): (T, text)} of one text log"""
    d = open(path, 'rb').read()
    out = {}
    pos = 0
    while True:
        j = d.find(b'@@ ', pos)
        if j < 0:
            break
        e = d.index(b'\n', j)
        _at, k, p, t, n = d[j:e].decode().split()
        n = int(n)
        text = d[e + 1:e + 1 + n].decode('utf-8', 'replace')
        out[(KIND[k], int(p, 16))] = (int(t, 16), text)
        pos = e + 1 + n
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pack', required=True, help='the shader pack the console ships (<key>.pack)')
    ap.add_argument('--hashlist', required=True, help='the tools/pack17/hashlist program (build_hashlist.sh)')
    ap.add_argument('--console', nargs='*', default=[], help='console log files or folders')
    ap.add_argument('--lists', nargs='*', default=[], help='shader lists (emulator profiles, helper runs)')
    ap.add_argument('--texts', nargs='*', default=[], help='shader text logs (MSC_VGX_SHADERTEXT_LOG), named <route>.texts')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    idx, packid, npack = pack_index(a.pack)
    missP, missT = console_misses(a.console)
    cands, skipped = [], []
    for p in sorted(set(a.lists)):
        r, why = list_hashes(a.hashlist, p)
        if r is None:
            skipped.append((os.path.basename(p), why))
        else:
            cands.append((os.path.basename(os.path.dirname(os.path.abspath(p))) + '/' + os.path.basename(p), r[0], r[1], r[2]))
    texts = []   # (route name, {(kind, P): (T, text)})
    for p in expand(a.texts, ('.texts',)):
        texts.append((os.path.basename(p)[:-len('.texts')], read_texts(p)))
    # T -> P over everything known
    t2p = {}
    for _n, _L, _m, t in cands:
        t2p.update(t)
    for _r, m in texts:
        for k, (t, _x) in m.items():
            t2p[(k[0], t)] = k[1]
    # the console's misses, as P
    seen = {}   # (kind, P) -> {'ms', 'files', 'present'}
    unresolved = []
    answered = 0
    for k, e in sorted(missP.items()):
        if k in idx:
            answered += 1
            continue
        seen[k] = {'ms': 0.0, 'files': set(e['files']), 'present': set(e['present'])}
    for k, e in sorted(missT.items()):
        p = t2p.get((k[0], k[1]))
        if p is None:
            unresolved.append(('T', k, e))
            continue
        if (k[0], p) in idx:
            answered += 1   # (an older pack's miss: this pack holds the text)
            continue
        n = seen.setdefault((k[0], p), {'ms': 0.0, 'files': set(), 'present': set()})
        n['ms'] = max(n['ms'], e['ms'])
        n['files'] |= e['files']
    # what the pack lacks, by P: keys (from lists), texts (from text logs), and where
    need = {}
    for k, e in seen.items():
        need[k] = {'key': None, 'text': None, 'lists': set(), 'routes': set(), 'console': True, 'ms': e['ms'], 'files': set(e['files']), 'present': set(e['present'])}
    for name, _L, m, _t in cands:
        for k, key in m.items():
            if k in idx:
                continue
            n = need.setdefault(k, {'key': None, 'text': None, 'lists': set(), 'routes': set(), 'console': False, 'ms': 0.0, 'files': set(), 'present': set()})
            n['key'] = key
            n['lists'].add(name)
    for route, m in texts:
        for k, (_t, text) in m.items():
            if k in idx:
                continue
            n = need.setdefault(k, {'key': None, 'text': None, 'lists': set(), 'routes': set(), 'console': False, 'ms': 0.0, 'files': set(), 'present': set()})
            n['text'] = text
            n['routes'].add(route)
    for k, n in need.items():
        if n['key'] is None:
            for name, _L, m, _t in cands:
                if k in m:
                    n['key'] = m[k]
                    n['lists'].add(name)
    for k, e in sorted(missP.items()):
        if k not in idx and need[k]['key'] is None and need[k]['text'] is None:
            unresolved.append(('P', k, e))
    # the request list: the keys, and every program that uses them with its variants (as native/tools/seed/newkeys.py does)
    D = shl.SList()
    if cands:
        D.vsSize, D.fsSize, D.gen = cands[0][1].vsSize, cands[0][1].fsSize, cands[0][1].gen
    needkeys = {0: {n['key'] for k, n in need.items() if k[0] == 0 and n['key'] is not None}, 1: {n['key'] for k, n in need.items() if k[0] == 1 and n['key'] is not None}}
    for _n, L, _m, _t in cands:
        bad = {(v, f) for v, f in L.prog if v in needkeys[0] or f in needkeys[1]}
        for v in sorted(needkeys[0]):
            if v in L.vs:
                D.add_vs(v)
        for f in sorted(needkeys[1]):
            if f in L.fs:
                D.add_fs(f)
        for v, f in sorted(bad):
            D.add_vs(v); D.add_fs(f); D.add_prog(v, f)
        for t in L.var:
            if (t[0], t[1]) in bad:
                D.add_var(*t)
    D.dups = 0
    D.prog.sort(); D.var.sort()
    os.makedirs(a.out, exist_ok=True)
    outs = {n: os.path.join(a.out, n) for n in ('pack17-request.shl', 'pack17-request.txt', 'pack17-routes.txt', 'pack17-request-texts.txt')}
    for n in ('pack17-request.shl', 'pack17-routes.txt', 'pack17-request-texts.txt'):
        if os.path.exists(outs[n]):
            os.remove(outs[n])
    keyed = {k: n for k, n in need.items() if n['key'] is not None}
    own = {k: n for k, n in need.items() if n['key'] is None and n['text'] is not None}
    if keyed and D.vs:
        shl.write(D, outs['pack17-request.shl'])
    routes = sorted({ROUTES.get(r, r) for n in own.values() for r in n['routes']})
    if routes:
        open(outs['pack17-routes.txt'], 'w').write('\n'.join(routes) + '\n')
    if own:
        with open(outs['pack17-request-texts.txt'], 'w') as f:
            for k in sorted(own):
                f.write('@@ %s %016X routes %s\n%s\n' % ('fs' if k[0] else 'vs', k[1], ','.join(sorted(own[k]['routes'])), own[k]['text']))
    nvs = sum(1 for k in keyed if k[0] == 0); nfs = len(keyed) - nvs
    lines = ['pack17 request: shader texts the pack lacks (no shader was compiled to make this file)',
             'pack: %s (%d shaders, sha256 %s...)' % (os.path.basename(a.pack), npack, packid),
             'console misses read: %d pack MISS lines + %d [shaderplay] hashes; already answered by this pack (the logs are of an older pack): %d; UNRESOLVED (no input holds the shader): %d' % (len(missP), len(missT), answered, len(unresolved)),
             'inputs: %d lists (skipped, other generator: %d), %d text logs' % (len(cands), len(skipped), len(texts)),
             'request: %d generated shaders (%d vertex, %d fragment; %d programs, %d variants in pack17-request.shl), %d back end programs (play %d routes: pack17-routes.txt)' % (len(keyed), nvs, nfs, len(D.prog), len(D.var), len(own), len(routes)),
             '',
             'kind P hash           what      seen          longest compile in play (console ms)  where',
             ]
    for k in sorted(need, key=lambda k: (-need[k]['ms'], k)):
        n = need[k]
        what = 'key' if n['key'] is not None else 'program' if n['text'] is not None else '?'
        seen_in = '+'.join(x for x, ok in (('console', n['console']), ('list', bool(n['lists'])), ('route', bool(n['routes']))) if ok)
        where = sorted(n['files']) + sorted(n['lists']) + ['route ' + r for r in sorted(n['routes'])]
        lines.append('%-4s %016X %-9s %-13s %8.1f   %s' % ('frag' if k[0] else 'vert', k[1], what, seen_in, n['ms'], ', '.join(where[:3]) + (' (+%d)' % (len(where) - 3) if len(where) > 3 else '')))
    if unresolved:
        lines += ['', 'UNRESOLVED (a console log names the hash, no list or text log holds the shader: record the route with MSC_VGX_SHADERLIST / MSC_VGX_SHADERTEXT_LOG and give that file here):']
        for ns, k, e in unresolved:
            lines.append('%-4s %s %016X  %8.1f   %s' % ('frag' if k[0] else 'vert', ns, k[1], e['ms'], ', '.join(sorted(e['files'])[:3])))
    for p, why in skipped:
        lines.append('skipped list %s: %s' % (p, why))
    open(outs['pack17-request.txt'], 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines[:5]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
