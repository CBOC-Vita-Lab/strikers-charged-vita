#!/usr/bin/env python3
"""pairedret.py: which retail functions return their float in the paired-single representation (their translated callers convert with
PPC_PsToScalarInline right after the call).  Writes pairedret.pkl (set of addresses) next to the script."""
import re, glob, pickle, os
HERE = os.path.dirname(os.path.abspath(__file__))
pat = re.compile(r'(?:InvokeDirectCpu<0x([0-9A-F]{8})u>\(ctx\)|MKW_STATIC_TRANSLATED_CALL\(0x([0-9A-F]{8})u, [^)]*\));\s*ctx->fpr\[1\]\.d = PPC_PsToScalarInline\(ctx->fpr\[1\]\.d\);')
s = set(); n = 0
for f in glob.glob('/home/claude/msc/split/gen-trunk/build_shards/base_common/shard_*.cpp') + ['/home/claude/msc/split/gen-trunk/fast_reference/fast_reference.cpp']:
    t = open(f, errors='replace').read()
    for m in pat.finditer(t):
        s.add(int(m.group(1) or m.group(2), 16)); n += 1
pickle.dump(s, open(os.path.join(HERE, 'pairedret.pkl'), 'wb'))
print(len(s), 'functions return paired singles (', n, 'call sites )')
