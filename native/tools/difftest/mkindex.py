#!/usr/bin/env python3
"""mkindex.py : index the recompiler's shards (function address -> shard file, start line, end line) into index.pkl (not committed)."""
import re, glob, pickle, os
idx = {}
for f in glob.glob('/home/claude/msc/split/gen-trunk/build_shards/base_common/shard_*.cpp'):
    lines = open(f, errors='replace').read().split('\n')
    marks = [i for i, l in enumerate(lines) if l.startswith('#line 1 "func_')]
    for k, i in enumerate(marks):
        m = re.match(r'#line 1 "func_([0-9A-Fa-f]{8})\.cpp"', lines[i])
        end = marks[k + 1] if k + 1 < len(marks) else len(lines)
        idx[int(m.group(1), 16)] = (f, i, end)
pickle.dump(idx, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'index.pkl'), 'wb'))
print(len(idx), 'functions indexed')
