#!/usr/bin/env python3
"""hashdiff.py: compare two MSC_VGX_HASHDUMP directories (same frames of two runs whose frame hashes differ) and say which arena, which draw and which bytes differ.
  hashdiff.py <dirA> <dirB> [--max N]
Per frame: the text description (draw records, uploads, shaders), then uni (4-byte words), vtx and idx byte by byte; uniform words are mapped to the draw whose
block contains them (sizes of the blocks: PROJ 16, MV 24, TEXM 192, PAL 384, LIGHT 176, TEV 112 floats), vertex bytes to the draw record (dynamic draws)."""
import argparse, os, re, struct, sys
KUB = [16, 24, 192, 384, 176, 112]; UBN = ['PROJ', 'MV', 'TEXM', 'PAL', 'LIGHT', 'TEV']
ap = argparse.ArgumentParser(); ap.add_argument('a'); ap.add_argument('b'); ap.add_argument('--max', type=int, default=6)
o = ap.parse_args()
frames = sorted({int(m.group(1)) for f in os.listdir(o.a) for m in [re.match(r'f(\d+)\.txt$', f)] if m})
def draws(path):
    out = []
    for l in open(path):
        if l.startswith('D '):
            t = l.split()
            ub = [int(x) for x in t[t.index('ub') + 1:t.index('ub') + 7]]
            nrm = int(t[t.index('nrm') + 1]) if 'nrm' in t else -1   # (HASHDUMP of 901adcd+: the draw's vertex shader reads the normal matrix)
            out.append(dict(i=int(t[1]), vs=t[3], fs=t[5], layout=t[11], vbuf=int(t[13]), vtxoff=int(t[15]), vtxcnt=int(t[17]), first=int(t[23]), chunk=int(t[25]), ub=ub, nrm=nrm))
    return out
nd = 0
for fr in frames:
    ta, tb = [open(f'{d}/f{fr}.txt').read() for d in (o.a, o.b)]
    ua, ub_ = [open(f'{d}/f{fr}.uni', 'rb').read() for d in (o.a, o.b)]
    va, vb = [open(f'{d}/f{fr}.vtx', 'rb').read() for d in (o.a, o.b)]
    ia, ib = [open(f'{d}/f{fr}.idx', 'rb').read() for d in (o.a, o.b)]
    msgs = []
    if ta != tb:
        la, lb = ta.split('\n'), tb.split('\n')
        dl = [(x, y) for x, y in zip(la, lb) if x != y]
        msgs.append(f'  text differs in {len(dl)} lines, first: {dl[0][0][:140]!r} / {dl[0][1][:140]!r}' if dl else '  text lengths differ')
    D = draws(f'{o.a}/f{fr}.txt')
    if ua != ub_:
        n = min(len(ua), len(ub_)) // 4
        wa, wb = struct.unpack(f'<{n}I', ua[:n * 4]), struct.unpack(f'<{n}I', ub_[:n * 4])
        dif = [i for i in range(n) if wa[i] != wb[i]]
        msgs.append(f'  uni: {len(dif)} of {n} words differ (sizes {len(ua)//4}/{len(ub_)//4})')
        for i in dif[:o.max]:
            # the block that holds the word: the one with the highest start <= i (a block ends where the next one starts), the draws that use it
            starts = sorted({d['ub'][k] for d in D for k in range(6) if d['ub'][k] != 0xFFFFFFFF})
            st = max([x for x in starts if x <= i], default=None)
            who = []
            if st is not None:
                nxt = min([x for x in starts if x > st], default=n)
                users = [(d['i'], UBN[k], d['nrm']) for d in D for k in range(6) if d['ub'][k] == st]
                who = [f"block {users[0][1]} at {st} (length {nxt - st}) word +{i - st}; used by draws " + ','.join(str(u[0]) for u in users[:6]) + (f" (vertex shader reads the normal matrix: {users[0][2]})" if users[0][2] >= 0 else '')]
            fa, fb = struct.unpack('<f', struct.pack('<I', wa[i]))[0], struct.unpack('<f', struct.pack('<I', wb[i]))[0]
            msgs.append(f'    word {i}: {wa[i]:08x} ({fa:g}) vs {wb[i]:08x} ({fb:g})  {who}')
    if va != vb:
        n = min(len(va), len(vb))
        dif = [i for i in range(n) if va[i] != vb[i]]
        msgs.append(f'  vtx: {len(dif)} of {n} bytes differ (sizes {len(va)}/{len(vb)})')
        for i in dif[:o.max]:
            who = [f"draw {d['i']} layout {d['layout']} vtx {d['vtxcnt']}" for d in D if d['vbuf'] == 0 and d['vtxoff'] <= i < d['vtxoff'] + 1][:1]
            msgs.append(f'    byte {i}: {va[i]:02x} vs {vb[i]:02x} {who}')
    if ia != ib:
        msgs.append(f'  idx: differs (sizes {len(ia)}/{len(ib)})')
    if msgs:
        nd += 1
        print(f'frame {fr}:'); print('\n'.join(msgs))
print(f'{nd} of {len(frames)} dumped frames differ')
