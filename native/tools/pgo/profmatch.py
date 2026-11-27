#!/usr/bin/env python3
# (22 Nov pgo13: paths are the helper container's; see w8/pgo13.md. profmatch.py: stale-profile measurement without the blind -Wprofile-instr-out-of-date; usage in its docstring, dry-run log from make -Bn of the Vita PGO build)
"""hashcmp.py <dry.txt> <cwd> <profdata> <regex> [jobs] : per game unit, compile the Vita line with -fprofile-generate -S -emit-llvm, read every
__profd_<name> {nameHash, funcHash}, compare with the profile (llvm-profdata show --all-functions: name, Hash). Reports matched/total functions
(only functions the profile has an entry for are 'known'), and weights by the profile's entry count."""
import re, shlex, subprocess, sys, collections
from concurrent.futures import ThreadPoolExecutor
dry, cwd, prof, sel = sys.argv[1:5]; nj = int(sys.argv[5]) if len(sys.argv) > 5 else 1
# profile
txt = subprocess.run(["llvm-profdata-18","show","--all-functions","--counts",prof],capture_output=True,text=True).stdout
P = {}; cur = None
for l in txt.split("\n"):
    m = re.match(r"^  (\S.*):$", l)
    if m: cur = m.group(1); P[cur] = {}; continue
    m = re.match(r"^    Hash: (0x[0-9a-f]+)", l)
    if m and cur: P[cur]["h"] = int(m.group(1), 16)
    m = re.match(r"^    Counters: (\d+)", l)
    m2 = re.match(r"^    Block counts: \[(.*)\]", l)
    if m2 and cur: c = [int(x) for x in m2.group(1).split(",") if x.strip()]; P[cur]["n"] = c[0] if c else 0
print(len(P), "profile functions", file=sys.stderr)
jobs = []; stack = []; c0 = cwd; cw = cwd
for l in open(dry).read().split("\n"):
    m = re.match(r"make\[?\d*\]?: (Entering|Leaving) directory '(.*)'", l)
    if m:
        if m.group(1) == "Entering": stack.append(m.group(2))
        elif stack: stack.pop()
        cw = stack[-1] if stack else c0; continue
    if " -c " in l and "-fprofile-use" in l and re.match(r"^(clang)", l):
        try: a = shlex.split(l)
        except ValueError: continue
        if "2>" in a: a = a[:a.index("2>")]
        i = a.index("-c"); src = a[i+1]
        if not re.search(sel, src): continue
        out = []; j = 0
        while j < len(a):
            x = a[j]
            if x in ("-o","-MF","-MT","-MQ"): j += 2; continue
            if x in ("-MMD","-MP","-w"): j += 1; continue
            if x == "-c": j += 2; continue
            if x.startswith("-flto") or x.startswith("-fprofile-use") or x.startswith("-Wno-profile"): j += 1; continue
            out.append(x); j += 1
        jobs.append((cw, out + ["-fprofile-generate","-S","-emit-llvm","-o","-",src], src))
print(len(jobs), "units", file=sys.stderr)
def run(j):
    c, cmd, src = j
    r = subprocess.run(cmd, cwd=c, capture_output=True, text=True)
    res = {}
    for m in re.finditer(r'^@"?__profd_([^"\s=]*)"? = .*?\{ i64 (-?\d+), i64 (-?\d+),', r.stdout, re.M):
        h = int(m.group(3)) & 0xffffffffffffffff; nm = m.group(1)
        mm = re.search(r"\.(\d{12,})$", nm)
        if mm and int(mm.group(1)) in (h, int(m.group(3))): nm = nm[:mm.start()]
        pre = src.replace('/', '_') + '_'
        if nm.startswith(pre): nm = src + ';' + nm[len(pre):]
        res[nm] = h
    return src, r.returncode, res
MISS = []; STALE = []; tot = match = known = 0; w_tot = w_match = 0; per = []; seen = set()
with ThreadPoolExecutor(nj) as ex:
    for src, rc, res in ex.map(run, jobs):
        t = m = k = 0
        for n, h in res.items():
            if n in seen: continue
            seen.add(n); t += 1
            e = P.get(n)
            w = e.get("n", 0) if e else 0
            if e is None: MISS.append(n)
            if e is not None:
                k += 1
                if e.get("h") == h: m += 1; w_match += w
                else: STALE.append((w, n))
                w_tot += w
        tot += t; known += k; match += m
        per.append((src, rc, t, k, m))
for src, rc, t, k, m in per:
    if rc: print("FAIL", src)
    elif k != m: print(f"{src}: {t} functions compiled, {k} in profile, {m} hash-equal, {k-m} STALE")
inprof = len(P); used = len(set(P) & seen)
print(f"TOTAL functions in current source {tot}; profile has {inprof}; of the current ones {known} have a profile entry, {match} with equal hash (stale {known-match}); profile entries not found in current source: {inprof-known}; entry-count weight matched {w_match}/{w_tot}")

STALE.sort(reverse=True)
open(sys.argv[6] if len(sys.argv)>6 else '/dev/null','w').write('\n'.join(f'{w} {n}' for w,n in STALE))

open('/tmp/profmatch-missing.txt','w').write('\n'.join(MISS))
