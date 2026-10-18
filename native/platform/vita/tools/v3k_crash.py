#!/usr/bin/env python3
"""Symbolises the first guest fault in a Vita3K output file against the link's ELF (no ASLR: run-time addresses = link addresses).
   v3k_crash.py <emulator stdout or stdout.head/.errors> <elf>      e.g. build/np/NPV2/v3k-runs/boot2/stdout.head .../mscharged-np.elf
Prints the emulator's message, PC/LR/SP, and for PC, LR and the other code-looking registers the function (arm-vita-eabi-addr2line -f -C)."""
import re, subprocess, sys
txt = open(sys.argv[1], errors='replace').read()
elf = sys.argv[2]
m = re.search(r'\|[EC]\| \[(Memory\w+)\]: ([^\n]*)\nPC: (0x[0-9a-f]+),\s+SP: (0x[0-9a-f]+),\s+LR: (0x[0-9a-f]+)\n((?:r\d+\s*:[^\n]*\n)+)', txt)
if not m:
    print("no fault block found"); sys.exit(1)
kind, msg, pc, sp, lr, regs = m.groups()
print(f"{kind}: {msg}\nPC {pc} SP {sp} LR {lr}")
cand = {'PC': pc, 'LR': lr}
for r, v in re.findall(r'(r\d+)\s*:\s*(0x[0-9a-f]+)', regs):
    if 0x81000000 <= int(v, 16) < 0x81400000: cand[r] = v
for k, v in cand.items():
    a = int(v, 16) & ~1
    out = subprocess.run(['/opt/vitasdk/bin/arm-vita-eabi-addr2line', '-f', '-C', '-e', elf, hex(a)], capture_output=True, text=True).stdout.split('\n')
    print(f"  {k:3} {v}: {out[0]}  {out[1] if len(out) > 1 else ''}")
