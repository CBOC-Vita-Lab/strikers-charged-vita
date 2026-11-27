#!/usr/bin/env python3
"""symbolize_samples: turn the console's [sampler] log lines (MSC_SAMPLER=1) into a function table.

The sampler (native/platform/vita/src/vita_sampler.cpp) logs, every window, the top 40 pcs and the top 12 lr values of each sampled thread:

  [sampler] on (...): ...; anchor msc_sampler_anchor at 0x8123ABCC (...)
  [sampler] window 3 target 0 "game" 10.0 s: 9731 samples (...)
  [sampler]   pc 0x81234568 412 4.23        (address, samples, percent of this window's pc samples)
  [sampler]   lr 0x81230010 97 1.00
  [sampler]   wait type 0x40 id 0x40010123 311

The load base is worked out from the anchor line: runtime address of msc_sampler_anchor minus its address in the ELF. Use the ELF the package was built from
(/out/v13-*/game/vita/pkg/np/mscharged-np.elf, the one with symbols), or any file with `nm`-style text via --nm.

Usage:
  symbolize_samples.py --log console.log --elf mscharged-np.elf [--top 40] [--lines] [--base 0x...] [--json]
  symbolize_samples.py --log console.log --nm nm-output.txt        (llvm-nm -n output: address type name)

Tools: llvm-symbolizer (preferred, gives file:line with --lines), else addr2line, else the --nm table (function granularity only).
Only the top-40 pcs of each window are logged, so the table shows where the time is by the biggest instructions, summed per function; "unlisted"
is the share of samples below the top-40 cut (the window's samples less the listed ones).
"""
import argparse, bisect, collections, json, re, shutil, subprocess, sys

RE_ANCHOR = re.compile(r"anchor msc_sampler_anchor at 0x([0-9A-Fa-f]+)")
RE_WIN = re.compile(r"\[sampler\] window (\d+) target (\d+) \"([^\"]*)\" ([0-9.]+) s: (\d+) samples \((\d+) running, (\d+) waiting")
RE_PC = re.compile(r"\[sampler\]\s+pc 0x([0-9A-Fa-f]+) (\d+) ([0-9.]+)")
RE_LR = re.compile(r"\[sampler\]\s+lr 0x([0-9A-Fa-f]+) (\d+) ([0-9.]+)")
RE_WAIT = re.compile(r"\[sampler\]\s+wait type 0x([0-9A-Fa-f]+) id 0x([0-9A-Fa-f]+) (\d+)")
RE_COST = re.compile(r"\[sampler\] cost: (.*)")


def parse(path):
    anchor = None
    targets = collections.OrderedDict()   # idx -> dict(name, windows, samples, running, waiting, pcs Counter, lrs Counter)
    waits = collections.Counter()
    costs = []
    cur = None
    with open(path, errors="replace") as f:
        for line in f:
            m = RE_ANCHOR.search(line)
            if m:
                anchor = int(m.group(1), 16)
                continue
            m = RE_WIN.search(line)
            if m:
                idx = int(m.group(2))
                t = targets.setdefault(idx, dict(name=m.group(3), windows=0, secs=0.0, samples=0, running=0, waiting=0, pcs=collections.Counter(), lrs=collections.Counter()))
                t["windows"] += 1
                t["secs"] += float(m.group(4))
                t["samples"] += int(m.group(5))
                t["running"] += int(m.group(6))
                t["waiting"] += int(m.group(7))
                cur = t
                continue
            m = RE_PC.search(line)
            if m and cur is not None:
                cur["pcs"][int(m.group(1), 16)] += int(m.group(2))
                continue
            m = RE_LR.search(line)
            if m and cur is not None:
                cur["lrs"][int(m.group(1), 16)] += int(m.group(2))
                continue
            m = RE_WAIT.search(line)
            if m:
                waits[(int(m.group(1), 16), int(m.group(2), 16))] += int(m.group(3))
                continue
            m = RE_COST.search(line)
            if m:
                costs.append(m.group(1).strip())
    return anchor, targets, waits, costs


class NmTable:
    """Function table from nm text (address type name), sorted."""
    def __init__(self, lines):
        self.addr, self.name = [], []
        rows = []
        for l in lines:
            p = l.split()
            if len(p) >= 3 and re.fullmatch(r"[0-9A-Fa-f]+", p[0]) and p[1] in "tTwWiI":
                rows.append((int(p[0], 16) & ~1, p[2]))
        rows.sort()
        for a, n in rows:
            self.addr.append(a)
            self.name.append(n)

    def find(self, a):
        i = bisect.bisect_right(self.addr, a) - 1
        return self.name[i] if i >= 0 else None

    def lookup_name(self, name):
        for a, n in zip(self.addr, self.name):
            if n == name:
                return a
        return None


def demangle(names):
    """names -> demangled names (llvm-cxxfilt / c++filt when present, else unchanged)."""
    tool = shutil.which("llvm-cxxfilt-18") or shutil.which("llvm-cxxfilt") or shutil.which("c++filt")
    if not tool or not names:
        return {n: n for n in names}
    out = subprocess.run([tool], input="\n".join(names) + "\n", capture_output=True, text=True).stdout.split("\n")
    return {n: (out[i] if i < len(out) and out[i] else n) for i, n in enumerate(names)}


def nm_of(elf):
    for tool in ("llvm-nm-18", "llvm-nm", "arm-vita-eabi-nm", "nm"):
        if shutil.which(tool):
            out = subprocess.run([tool, "-n", elf], capture_output=True, text=True).stdout
            return NmTable(out.splitlines())
    sys.exit("no nm tool found; pass --nm with a text file")


def symbolize(elf, addrs, lines):
    """address -> (function, file:line or '') through llvm-symbolizer or addr2line."""
    res = {}
    uniq = sorted(set(addrs))
    tool = shutil.which("llvm-symbolizer-18") or shutil.which("llvm-symbolizer")
    if tool:
        cmd = [tool, "--obj=" + elf, "--demangle", "--functions=short", "--inlining=false"] + ([] if lines else ["--no-inlines"])
        out = subprocess.run(cmd, input="\n".join("0x%x" % a for a in uniq) + "\n", capture_output=True, text=True).stdout
        recs = out.strip("\n").split("\n\n")
        for a, r in zip(uniq, recs):
            ls = r.split("\n")
            fn = ls[0] if ls else "??"
            loc = ls[1] if len(ls) > 1 else ""
            res[a] = (fn, loc if "??" not in loc else "")
        return res
    tool = shutil.which("addr2line") or shutil.which("arm-vita-eabi-addr2line")
    if tool:
        out = subprocess.run([tool, "-e", elf, "-f", "-C"] , input="\n".join("0x%x" % a for a in uniq) + "\n", capture_output=True, text=True).stdout.split("\n")
        for i, a in enumerate(uniq):
            fn = out[2 * i] if 2 * i < len(out) else "??"
            loc = out[2 * i + 1] if 2 * i + 1 < len(out) else ""
            res[a] = (fn, loc if "??" not in loc else "")
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", required=True)
    ap.add_argument("--elf", help="the ELF with symbols that the package was built from")
    ap.add_argument("--nm", help="text of `llvm-nm -n` instead of an ELF (function granularity)")
    ap.add_argument("--base", help="runtime load base minus the ELF's link address, hex (overrides the anchor)")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--lines", action="store_true", help="also give file:line of the biggest pc of each function (needs --elf and debug info)")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    if not a.elf and not a.nm:
        sys.exit("give --elf or --nm")
    anchor, targets, waits, costs = parse(a.log)
    if not targets:
        sys.exit("no [sampler] window lines in the log (was MSC_SAMPLER=1 set, and did the layout calibrate? see the [sampler] lines)")
    nm = nm_of(a.elf) if a.elf else NmTable(open(a.nm).read().splitlines())
    if a.base:
        delta = int(a.base, 16)
    elif anchor is not None:
        elf_anchor = nm.lookup_name("msc_sampler_anchor")
        if elf_anchor is None:
            sys.exit("msc_sampler_anchor is not in the ELF/nm text: pass --base")
        delta = (anchor & ~1) - elf_anchor
    else:
        sys.exit("the log has no anchor line: pass --base")
    print("load delta (runtime - ELF) = 0x%X" % (delta & 0xFFFFFFFF))
    out_json = {}
    for idx, t in targets.items():
        listed = sum(t["pcs"].values())
        run = t["running"] or 1
        print("\n=== target %d \"%s\": %d windows, %.0f s, %d samples (%d running with a pc, %d waiting) ===" % (idx, t["name"], t["windows"], t["secs"], t["samples"], t["running"], t["waiting"]))
        elf_pcs = {pc: pc - delta for pc in t["pcs"]}
        names = {}
        locs = {}
        if a.elf:
            sy = symbolize(a.elf, list(elf_pcs.values()), a.lines)
            for pc, e in elf_pcs.items():
                names[pc], locs[pc] = sy.get(e, (None, ""))
        for pc, e in elf_pcs.items():
            if not names.get(pc) or names[pc] == "??":
                names[pc] = nm.find(e) or "??"
        dm = demangle(sorted(set(names.values())))
        names = {pc: dm.get(n, n) for pc, n in names.items()}
        by = collections.Counter()
        best = {}
        for pc, n in t["pcs"].items():
            by[names[pc]] += n
            if names[pc] not in best or n > t["pcs"][best[names[pc]]]:
                best[names[pc]] = pc
        print("%-6s %-7s %s" % ("share", "count", "function (largest pc)"))
        for fn, n in by.most_common(a.top):
            pc = best[fn]
            print("%5.1f%% %7d %s  [0x%X%s]" % (100.0 * n / run, n, fn, pc - delta, (" " + locs[pc]) if locs.get(pc) else ""))
        print("%5.1f%% %7d (unlisted: below the top-40 cut of each window)" % (100.0 * max(0, run - listed) / run, max(0, run - listed)))
        callers = collections.Counter()
        for lr, n in t["lrs"].items():
            callers[nm.find(lr - delta) or "??"] += n
        dmc = demangle(sorted(callers))
        callers = collections.Counter({dmc[k]: v for k, v in callers.items()})
        if callers:
            print("callers (lr of the sampled pc; for leaf functions the caller):")
            for fn, n in callers.most_common(12):
                print("%5.1f%% %7d %s" % (100.0 * n / run, n, fn))
        out_json[str(idx)] = dict(name=t["name"], samples=t["samples"], running=t["running"], waiting=t["waiting"], functions=by.most_common(a.top))
    if waits:
        print("\nwaits (type = kernel object class, id = its uid), all targets:")
        for (ty, wid), n in waits.most_common(10):
            print("  type 0x%X id 0x%X %d" % (ty, wid, n))
    for c in costs[-1:]:
        print("\nsampler cost, last window: " + c)
    if a.json:
        print(json.dumps(out_json))


if __name__ == "__main__":
    main()
