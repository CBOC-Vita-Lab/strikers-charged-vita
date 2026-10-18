#!/usr/bin/env python3
"""Hottest translation blocks of one function in a qemu framecount-plugin profile, with disassembly.
  prof_hot.py <tag> <function-name-substring> [blocks=12] [g|o] [frames=313]
Files: build/host-gl-arm/runs/<tag>.{prof,bin,log}. Thread: o = render + audio threads (default),
g = game thread. Counts are instructions per presented frame."""
import subprocess, sys, bisect, re, os
os.chdir('/home/claude/msc/build/host-gl-arm/runs')
tag, pat, top = sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 12
col = 1 if (len(sys.argv) > 4 and sys.argv[4] == 'g') else 2
FR = float(sys.argv[5]) if len(sys.argv) > 5 else 313.0
binp = tag + '.bin'
syms = []
for l in subprocess.run(['arm-linux-gnueabihf-nm', '-n', '-C', '--defined-only', binp], capture_output=True, text=True).stdout.split('\n'):
    p = l.split(' ', 2)
    if len(p) == 3 and p[1] in 'tTwW': syms.append((int(p[0], 16) & ~1, p[2]))
keys = [s[0] for s in syms]
cands = [i for i, s in enumerate(syms) if pat in s[1]]
i = cands[0]; lo = syms[i][0]; hi = syms[i + 1][0]
print(syms[i][1][:100], hex(lo), hex(hi))
blocks = []
base = 0
for line in open(tag + '.log', errors='replace'):
    m = re.match(r'\[maps\] ([0-9a-f]+)-([0-9a-f]+) \S+ ([0-9a-f]+) \S+ \S+\s+(\S+)', line)
    if m and m.group(4).endswith('/' + os.path.basename(binp)):
        base = int(m.group(1), 16) - int(m.group(3), 16); break
for l in open(tag + '.prof'):
    p = l.split()
    pc = (int(p[0], 16) - base) & ~1
    if lo <= pc < hi and int(p[col]): blocks.append((int(p[col]), pc))
blocks.sort(reverse=True)
tot = sum(b[0] for b in blocks)
print('total %.3f M/frame in %d blocks' % (tot / FR / 1e6, len(blocks)))
dis = subprocess.run(['arm-linux-gnueabihf-objdump', '-d', '-C', '--no-show-raw-insn', '--start-address=%d' % lo, '--stop-address=%d' % hi, binp], capture_output=True, text=True).stdout.split('\n')
lines = {}
order = []
for l in dis:
    m = re.match(r'\s*([0-9a-f]+):\s+(.*)', l)
    if m: lines[int(m.group(1), 16)] = m.group(2); order.append(int(m.group(1), 16))
for cnt, pc in blocks[:top]:
    print('--- block %x: %.1f k instr/frame (%.1f%%)' % (pc, cnt / FR / 1e3, 100.0 * cnt / tot))
    j = bisect.bisect_left(order, pc)
    for a in order[j:j + 14]:
        print('   %x: %s' % (a, lines[a][:110]))
        if re.match(r'(b|bx|bl|blx|b\w\w|cbz|cbnz|pop)\b', lines[a].split('\t')[0]) and not lines[a].startswith('bic'): break
