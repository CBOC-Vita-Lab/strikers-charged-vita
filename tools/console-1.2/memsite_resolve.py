#!/usr/bin/env python3
# memsite17-resolve.py <log> <elf> [tag-substring]: names the function holding each "[memsite] ... site XXXXXXXX (ref+N)" line (address = vglMemSiteDump in the ELF + N).
import re, subprocess, sys, bisect
log, elf = sys.argv[1], sys.argv[2]; want = sys.argv[3] if len(sys.argv) > 3 else ""
nm = subprocess.run(["/opt/vitasdk/bin/arm-vita-eabi-nm", "-n", "-C", elf], capture_output=True, text=True).stdout.splitlines()
syms = []
for l in nm:
    p = l.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
ref = [a for a, n in syms if n == "vglMemSiteDump"][0]
for l in open(log, errors="replace"):
    m = re.match(r"\[memsite\] (.*?): pool (\d) (\w+)\s+site ([0-9a-f]+) \(ref([+-]\d+)\): +(\d+) blocks +(\d+) KiB", l)
    if not m or want not in m.group(1): continue
    a = ref + int(m.group(5)); i = bisect.bisect_right(addrs, a) - 1
    print("%-34s pool %s %-4s %6s KiB %5s blocks  %s" % (m.group(1)[:34], m.group(2), m.group(3), m.group(7), m.group(6), syms[i][1][:90] if i >= 0 else "?"))
