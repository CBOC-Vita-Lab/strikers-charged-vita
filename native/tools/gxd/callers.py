#!/usr/bin/env python3
"""callers.py: call sites of a function (e.g. __udivsi3, memcmp@plt) in a NON-PIE armhf exe with how often each ran, from a framecount2 profile
(translation-block counts: a call ends its block, so executions = instructions counted at the block start / instructions from the start to the call).
  callers.py <prof.txt> <exe> <callee-regex> <frames> [--thread other|game] [--top 30]"""
import argparse, subprocess, re, collections, bisect
ap = argparse.ArgumentParser()
ap.add_argument('prof'); ap.add_argument('exe'); ap.add_argument('callee'); ap.add_argument('frames', type=float)
ap.add_argument('--thread', default='other'); ap.add_argument('--top', type=int, default=30)
a = ap.parse_args()
idx = 1 if a.thread == 'game' else 2
blocks = {}
for l in open(a.prof):
    f = l.split(); d = int(f[idx])
    if d: blocks[int(f[0], 16) & ~1] = d
starts = sorted(blocks)
dis = subprocess.run(['arm-linux-gnueabihf-objdump', '-d', '-C', '--no-show-raw-insn', a.exe], capture_output=True, text=True).stdout.splitlines()
ins = []          # (addr, text, function)
fn = '?'
for l in dis:
    m = re.match(r'^[0-9a-f]+ <(.*)>:$', l)
    if m: fn = m.group(1); continue
    m = re.match(r'^\s*([0-9a-f]+):\s+(.*)$', l)
    if m: ins.append((int(m.group(1), 16), m.group(2), fn))
addrs = [i[0] for i in ins]
rx = re.compile(a.callee)
sites = []
for k, (ad, tx, f) in enumerate(ins):
    if re.match(r'(bl|blx|b\.w|b)\s', tx) and rx.search(tx):
        sites.append(k)
res = collections.Counter(); where = {}
isbr = re.compile(r'^(b|bl|blx|bx|bne|beq|bhi|bls|bcs|bcc|bmi|bpl|bge|blt|bgt|ble|bvs|bvc|cbz|cbnz|pop|ldr)(\.[nw])?\s')
def is_branch(tx):
    if re.match(r'^(b\w*|cbz|cbnz)(\.[nw])?\s', tx) and not re.match(r'^(bic|bfi|bfc|bkpt)', tx): return True
    if re.match(r'^(pop|ldmia|ldm\w*)(\.w)?\s.*\bpc\b', tx): return True
    if re.match(r'^ldr(\.w)?\s+pc,', tx): return True
    return False
for k in sites:
    # the translation block that holds the call starts after the previous branch (or at a jump target in between) and ends at the call
    pb = k - 1
    while pb >= 0 and not is_branch(ins[pb][1]) and k - pb < 400: pb -= 1
    lo = bisect.bisect_right(starts, ins[pb][0]) if pb >= 0 else 0
    best = 0.0
    while lo < len(starts) and starts[lo] <= ins[k][0]:
        j = bisect.bisect_left(addrs, starts[lo])
        n = k - j + 1
        if n > 0: best = max(best, blocks[starts[lo]] / n)
        lo += 1
    if best: res[k] = best
tot = sum(res.values())
print(f'{a.callee}: {tot / a.frames:.0f} calls per frame ({a.thread} thread), {len(sites)} call sites')
top = res.most_common(a.top)
a2l = subprocess.run(['arm-linux-gnueabihf-addr2line', '-e', a.exe, '-C', '-f', '-i'], input='\n'.join('%x' % ins[k][0] for k, _ in top), capture_output=True, text=True).stdout
for k, v in top:
    print(f'  {v / a.frames:8.1f}/frame  {ins[k][2][:90]}  @{ins[k][0]:x}')
