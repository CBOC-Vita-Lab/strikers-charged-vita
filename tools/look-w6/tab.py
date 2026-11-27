import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ev, pl, numpy as np
# usage: tab.py size tag mode [k|auto] [edge0]
size, tag, mode = sys.argv[1:4]
k = None if len(sys.argv) < 5 or sys.argv[4] == 'auto' else float(sys.argv[4])
edge = not (len(sys.argv) > 5 and sys.argv[5] == 'edge0')
R = ev.run(size, tag, mode, k=k, edge=edge)
for sc, r in R.items():
    print(size, tag, mode, k, sc, ' '.join('%s=%.3f' % (a, b) for a, b in r.items()))
