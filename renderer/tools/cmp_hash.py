#!/usr/bin/env python3
"""Compare the per-frame command-buffer hashes (MSC_VGX_HASH=1) of two runs.
  cmp_hash.py <a/vgx_frames.csv> <b/vgx_frames.csv>
Prints the number of frames compared and the first frames that differ (with the counters that
changed), exit code 1 if any differ."""
import csv, sys
a = {int(r['frame']): r for r in csv.DictReader(open(sys.argv[1]))}
b = {int(r['frame']): r for r in csv.DictReader(open(sys.argv[2]))}
frames = sorted(set(a) & set(b))
bad = [f for f in frames if a[f]['hash'] != b[f]['hash']]
print('%d frames in both (%d / %d), %d differ' % (len(frames), len(a), len(b), len(bad)))
for f in bad[:10]:
    d = ['%s %s->%s' % (k, a[f][k], b[f][k]) for k in a[f] if k not in ('hash', 'frame') and a[f][k] != b[f].get(k)]
    print('  frame %d: %s' % (f, ', '.join(d) if d else '(counters equal, contents differ)'))
sys.exit(1 if bad or not frames else 0)
