#!/usr/bin/env python3
"""seedwatch_diff.py a.log b.log: compare the write sequences of gNetworkRandomSeed recorded by tools/msp/seedwatch.gdb in two runs.
Prints the number of writes in each, the first write at which the new value or the callers differ, and the callers around it."""
import re, sys

def load(path):
    hits, cur = [], None
    for line in open(path, errors='replace'):
        if line.startswith('HIT '):
            cur = [line.split()[1], []]
            hits.append(cur)
        elif cur is not None and line.startswith('#'):
            m = re.match(r'#\d+\s+(?:0x[0-9a-f]+ in )?(\S+)', line)
            if m:
                cur[1].append(m.group(1))
        elif cur is not None and not line.startswith('#'):
            if line.strip() and not line.startswith(('Continuing', '[')):
                pass
    return hits

def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    print('writes: %s %d, %s %d' % (sys.argv[1], len(a), sys.argv[2], len(b)))
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            print('first difference at write %d' % i)
            for j in range(max(0, i - 3), min(i + 4, min(len(a), len(b)))):
                print('  %5d A %s %s' % (j, a[j][0], ' < '.join(a[j][1][1:5])))
                print('  %5d B %s %s' % (j, b[j][0], ' < '.join(b[j][1][1:5])))
            return 1
    print('identical on the common %d writes' % min(len(a), len(b)))
    return 0

sys.exit(main())
