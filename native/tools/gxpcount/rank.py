#!/usr/bin/env python3
"""Fragment shaders by real cost: fragments of a census (MSC_VGX_FSCENSUS_OUT) times the GXP's primary instructions.
   rank.py <log.txt with [fscensus] lines> <pack> [out.bin of shc.c ...] [--top n]
Instructions come from the pack by the text's hash, or from an out.bin of shc.c (records named <hash> or <hash>.<variant>)."""
import struct, sys, collections
def gxp(b):
    p = b[b.find(b'GXP\0'):]
    f = struct.unpack_from
    flags = f('<I', p, 20)[0]; t0, t1 = f('<II', p, 28)
    nd = dep = 0
    for w in (t0, t1):
        for i in range(8):
            x = (w >> (4 * i)) & 15; nd += x & 1; dep += (x >> 1) & 1
    return dict(pi=f('<I', p, 60)[0], si=f('<I', p, 68)[0], pa=f('<H', p, 48)[0], flags=flags, nd=nd, dep=dep)
def pack(path):
    d = open(path, 'rb').read(); n = struct.unpack('<I', d[8:12])[0]; o = {}
    for i in range(n):
        h, kind, off, size, _ = struct.unpack('<QIIII', d[16 + 24 * i:16 + 24 * (i + 1)])
        if kind == 1: o['%016X' % h] = d[off:off + size]
    return o
def outbin(path):
    d = open(path, 'rb').read(); i = 0; o = {}
    while i < len(d):
        nl = struct.unpack_from('<I', d, i)[0]; i += 4
        if nl > 64 or i + nl + 4 > len(d): break   # (a run cut off in mid-record)
        n = d[i:i + nl].decode(); i += nl
        sz = struct.unpack_from('<I', d, i)[0]; i += 4
        if i + sz > len(d): break
        o[n] = d[i:i + sz]; i += sz
    return o
def census(path):
    R = []; head = None
    d = open(path, 'rb').read()
    last = d.rfind(b'[fscensus] #frames')   # (cumulative: the last table of the log)
    d = d[last:].replace(b'[fscensus] ', b''); p = 0
    while p < len(d):
        e = d.find(b'\n', p)
        if e < 0: break
        l = d[p:e].decode('latin1')
        if l.startswith('#'): head = l; p = e + 1
        elif l.startswith('V '):
            VS.append((l.split()[1], int(l.split()[2]))); p = e + 1
        elif l.startswith('R '):
            x = l.split()
            R.append(dict(key=x[1], scene=int(x[2]), tgt=int(x[3]), state=int(x[4]), blend=int(x[5]), texop=int(x[6]), cls=int(x[7]), draws=int(x[8]), unm=int(x[9]), raster=int(x[10]), zrej=int(x[11]), shaded=int(x[12]), kill=int(x[13]), ideal=int(x[14]), early=int(x[15])))
            p = e + 1
        else: p = e + 1
    return head, R
VS = []
SCENE = ['other', 'play', 'replay', 'nis']
CLS = ["unknown", "player", "item", "fx", "hud", "npc", "6", "7", "stadium", "stadium-alpha", "crowd", "sky", "far", "crowd-capture"]
def kind(s):
    if s & 1 and s & 2: return 'blend+test'
    if s & 1: return 'blend'
    if s & 2: return 'test'
    if not s & 4: return 'opaque-nozw'
    return 'opaque'
def main():
    a = [x for x in sys.argv[1:]]
    top = 25
    if '--top' in a: i = a.index('--top'); top = int(a[i + 1]); del a[i:i + 2]
    head, R = census(a[0])
    P = pack(a[1])
    O = {}
    for x in a[2:]: O.update(outbin(x))
    print(head)
    def instr(k, v=''):
        h = k
        b = O.get(h + v) if v else (P.get(h) or O.get(h))
        if not b: return None
        return gxp(b)
    tot = collections.Counter(); byk = collections.defaultdict(collections.Counter)
    miss = 0
    for r in R:
        g = instr(r['key'])
        c = (g['pi'] if g else 0) * r['shaded']
        if not g: miss += r['shaded']
        r['cost'] = c; r['g'] = g
        tot['frag'] += r['shaded']; tot['cost'] += c; tot['raster'] += r['raster']
        for name in (SCENE[r['scene']], 'pass' if r['tgt'] else 'efb', kind(r['state']), CLS[r['cls']] if r['cls'] < len(CLS) else '?'):
            byk[name]['frag'] += r['shaded']; byk[name]['cost'] += c; byk[name]['raster'] += r['raster']; byk[name]['ideal'] += r['ideal']
    print('total: raster %d shaded %d cost %d; shaded with no GXP %d' % (tot['raster'], tot['frag'], tot['cost'], miss))
    for n, c in sorted(byk.items(), key=lambda kv: -kv[1]['cost']):
        print('  %-14s frag %5.1f%%  cost %5.1f%%  mean instr %5.1f  raster/shaded %5.2f' % (n, 100 * c['frag'] / max(tot['frag'], 1), 100 * c['cost'] / max(tot['cost'], 1), c['cost'] / max(c['frag'], 1), c['raster'] / max(c['frag'], 1)))
    # per shader
    S = collections.defaultdict(lambda: collections.Counter()); info = {}
    for r in R:
        s = S[r['key']]
        for f in ('draws', 'raster', 'zrej', 'shaded', 'kill', 'ideal', 'early', 'cost', 'unm'): s[f] += r[f]
        info.setdefault(r['key'], set()).add('%s/%s' % (kind(r['state']), CLS[r['cls']] if r['cls'] < len(CLS) else '?'))
    print('rank: cost%, cum%, frag%, instr(pi/si/nd/dep/flags), draws, unmeasured, kinds')
    cum = 0
    for k, s in sorted(S.items(), key=lambda kv: -kv[1]['cost'])[:top]:
        g = instr(k); cum += s['cost']
        print('%5.1f %5.1f %5.1f  %s  pi %s si %s nd %s dep %s %s  d %d u %d  %s' % (100 * s['cost'] / max(tot['cost'], 1), 100 * cum / max(tot['cost'], 1), 100 * s['shaded'] / max(tot['frag'], 1), k, g and g['pi'], g and g['si'], g and g['nd'], g and g['dep'], g and hex(g['flags'] & 0x18), s['draws'], s['unm'], ','.join(sorted(info[k]))[:70]))
    # vertex shaders: vertices times primary instructions
    def vpack(path):
        d = open(path, 'rb').read(); n = struct.unpack('<I', d[8:12])[0]; o = {}
        for i in range(n):
            h, kind, off, size, _ = struct.unpack('<QIIII', d[16 + 24 * i:16 + 24 * (i + 1)])
            if kind == 0: o['%016X' % h] = d[off:off + size]
        return o
    PV = vpack(a[1])
    vt = sum(n * gxp(PV[h])['pi'] for h, n in VS if h in PV)
    print('vertex: %d indices, %d weighted instructions (fragment %d); vertices with no GXP %d' % (sum(n for _, n in VS), vt, tot['cost'], sum(n for h, n in VS if h not in PV)))
    cum = 0
    for h, n in sorted(VS, key=lambda x: -x[1] * (gxp(PV[x[0]])['pi'] if x[0] in PV else 0))[:top]:
        if h not in PV: continue
        c = n * gxp(PV[h])['pi']; cum += c
        print('%5.1f %5.1f  %s  pi %d  indices %d  %s' % (100 * c / max(vt, 1), 100 * cum / max(vt, 1), h, gxp(PV[h])['pi'], n, ' '.join('%s %d' % (v, gxp(O[h + '.' + v])['pi']) for v in 'ACURN' if h + '.' + v in O)))
    for v in 'ACURN':
        b = sum(n * gxp(PV[h])['pi'] for h, n in VS if h in PV and h + '.' + v in O); w = sum(n * gxp(O[h + '.' + v])['pi'] for h, n in VS if h in PV and h + '.' + v in O)
        if b: print('vertex variant %s: weighted %d -> %d (%.1f%%)' % (v, b, w, 100 * (w - b) / b))
    # variants
    vs = sorted({n.split('.')[1] for n in O if '.' in n and n.split('.')[1] in 'TPFH'})
    for v in vs:
        base = new = cov = 0
        for r in R:
            g = instr(r['key']); w = instr(r['key'], '.' + v)
            if g and w: base += g['pi'] * r['shaded']; new += w['pi'] * r['shaded']; cov += r['shaded']
            elif g and v == 'P': base += g['pi'] * r['shaded']; new += instr(r['key'], '.T')['pi'] * r['shaded'] if instr(r['key'], '.T') else g['pi'] * r['shaded']
        print('variant %s: covers %.1f%% of fragments; weighted instructions %d -> %d (%.1f%%)' % (v, 100 * cov / max(tot['frag'], 1), base, new, 100 * (new - base) / max(base, 1)))
main()
