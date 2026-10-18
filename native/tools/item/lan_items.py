#!/usr/bin/env python3
"""W5-ITEM: the items of a LAN pair (tools/mp/native_pair.py run dir: peer0/runtime.log = host, peer1/runtime.log = joiner).
Each console logs every give it APPLIED (`[item] sim=<step> give side=..`), every item hit (`[item] sim=<step> hit ..`) and its slots; in a match in step
both consoles must show the same gives and hits at the same simulation steps. Prints the counts, the first difference, and per item type what was given,
thrown (objects) and hit, per console.
   lan_items.py <pair dir> [...]      exit status 1 if a pair's two lists differ"""
import re, sys, os
from collections import Counter

GIVE = re.compile(r'\[item\] sim=(\d+) give side=(\d) (\S+) x(\d+) (\w+): (\S+)')
HIT = re.compile(r'\[item\] sim=(\d+) hit (\S+) \((\w+)\) thrown by side(-?\d)#(-?\d) -> (\w+) side(-?\d)#(-?\d)')
OBJ = re.compile(r'\[item\] f=\d+ sim=(\d+) object \d+ new: (\S+) (\S+).* thrower side(-?\d)#')
QUEUE = re.compile(r'\[item\] sim=(\d+) queue give side=(\d) (\S+)')


def load(path):
    gives, hits, objs, queued = [], [], [], []
    with open(path, errors='replace') as f:
        for line in f:
            m = GIVE.search(line)
            if m:
                gives.append((int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4)), m.group(5), m.group(6)))
            m = HIT.search(line)
            if m:
                hits.append((int(m.group(1)), m.group(2), m.group(3), m.group(4), m.group(5), m.group(6), m.group(7), m.group(8)))
            m = OBJ.search(line)
            if m:
                objs.append((int(m.group(1)), m.group(2), m.group(3), m.group(4)))
            m = QUEUE.search(line)
            if m:
                queued.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    return gives, hits, objs, queued


def main():
    bad = 0
    for d in sys.argv[1:]:
        L = [load(os.path.join(d, 'peer%d' % i, 'runtime.log')) for i in (0, 1)]
        print('== %s' % d)
        for i, (g, h, o, q) in enumerate(L):
            print('  console %d (%s): queued %d, gives applied %d (side0 %d, side1 %d), hits %d, objects %d (by step, may differ by a frame)' %
                  (i, 'host' if i == 0 else 'joiner', len(q), len(g), sum(1 for x in g if x[1] == 0), sum(1 for x in g if x[1] == 1), len(h), len(o)))
        for name, k in (('gives', 0), ('hits', 1)):
            a, b = L[0][k], L[1][k]
            if a == b:
                print('  %s: EQUAL on both consoles (%d, same simulation steps)' % (name, len(a)))
            else:
                bad += 1
                n = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
                print('  %s: DIFFER (%d vs %d), first at #%d: %s | %s' % (name, len(a), len(b), n, a[n] if n < len(a) else '-', b[n] if n < len(b) else '-'))
        types = Counter('%s side%d' % (x[2], x[1]) for x in L[0][0])
        print('  given (host log): ' + ', '.join('%s %d' % kv for kv in sorted(types.items())))
        ht = Counter('%s by side%s' % (x[1], x[3]) for x in L[0][1])
        print('  hits (host log): ' + ', '.join('%s %d' % kv for kv in sorted(ht.items())))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
