#!/usr/bin/env python3
"""sampler_report.py: top functions from the sampler's ux0:data/mscharged/sampler-<n>.bin files (MSC_SAMPLER=1, native/platform/vita/src/vita_sampler.cpp, np_sampler_bin.h).

usage: python3 -I tools/console-1.2/sampler_report.py ELF sampler-1.bin [sampler-2.bin ...] [--thread NAME] [--top N] [--src DIR] [--nm PATH] [--nm-text FILE]

ELF is the unstripped mscharged-np.elf of the SAME build that ran (the one next to eboot.bin in the build output; eboot.bin has no symbols). Several .bin files are merged (add up).
Addresses in the files are run-time addresses; the load offset is (run-time msc_sampler_anchor) - (its ELF address), else (text base - the ELF's first LOAD vaddr, 0x81000000).
Per thread: the run/wait split, then top functions SELF (pc), top functions INCLUSIVE-BY-LR (samples whose pc is in the function or whose lr, the caller or the return
address of a leaf, is in it: one level, not a full stack), and the same two grouped by subsystem (Game/<dir>, NL, ode, platform/<dir>, renderer, libc/libm ...). Names are mapped to
a subsystem by the class / namespace / function names found in the source tree (--src, default: the repo this script is in); a name found nowhere is listed under ? and printed
by --unmapped. The sampler's own cost (how long it stopped each thread, its CPU share) is printed from the file headers. Written for this port from the Galaxy port's report script (same owner)."""
import argparse, bisect, collections, os, re, shutil, struct, subprocess, sys

NM_DEFAULT = "/opt/vitasdk/bin/arm-vita-eabi-nm"
MAGIC = b"MSCSAMP1"
MAXSLOTS = 4
HDR_FIXED = struct.Struct("<8s18I32s32s")
SLOT = struct.Struct("<6I24s")
ENTRY = struct.Struct("<4I")
WAIT_NAMES = {0: "none", 1: "thread", 2: "semaphore", 3: "eventflag", 4: "mbox", 5: "vpl", 6: "fpl", 7: "msgpipe", 8: "callback", 9: "thread-event",
              10: "alarm", 11: "vtimer", 64: "sleep", 65: "delay", 66: "suspend", 67: "dormant"}

def cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace")

def read_bin(path):
    data = open(path, "rb").read()
    need = HDR_FIXED.size + MAXSLOTS * SLOT.size
    if len(data) < need or data[:8] != MAGIC:
        raise SystemExit("%s: not a sampler .bin (magic MSCSAMP1)" % path)
    f = HDR_FIXED.unpack_from(data, 0)
    (_m, version, index, period, interval, window, tbase, tsize, anchor, nent, dropped, runs, waits, failed, pcw, lrw, ticks, late, samplerus, stage, aname) = f
    if version != 1:
        raise SystemExit("%s: version %d unknown" % (path, version))
    slots = {}
    for i in range(MAXSLOTS):
        uid, used, stops, hold, holdmax, fails, name = SLOT.unpack_from(data, HDR_FIXED.size + i * SLOT.size)
        if used:
            slots[i] = dict(uid=uid, name=cstr(name), stops=stops, hold=hold, holdmax=holdmax, fails=fails)
    ents = []
    off = need
    if len(data) < off + nent * ENTRY.size:
        print("warning: %s is truncated (%d of %d entries)" % (path, (len(data) - off) // ENTRY.size, nent), file=sys.stderr)
        nent = (len(data) - off) // ENTRY.size
    for i in range(nent):
        ents.append(ENTRY.unpack_from(data, off + i * ENTRY.size))
    return dict(path=path, index=index, period=period, interval=interval, window=window, tbase=tbase, tsize=tsize, anchor=anchor, aname=cstr(aname),
                dropped=dropped, runs=runs, waits=waits, failed=failed, ticks=ticks, late=late, samplerus=samplerus, stage=cstr(stage), tag=cstr(stage), slots=slots, ents=ents)

class Symbols:
    def __init__(self, text):
        rows = []
        for ln in text.splitlines():
            p = ln.split(None, 3)
            if len(p) == 4 and re.fullmatch(r"[0-9a-fA-F]+", p[0]) and re.fullmatch(r"[0-9a-fA-F]+", p[1]) and len(p[2]) == 1:
                addr, size, typ, name = int(p[0], 16) & ~1, int(p[1], 16), p[2], p[3]
            elif len(p) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", p[0]) and len(p[1]) == 1:
                addr, size, typ, name = int(p[0], 16) & ~1, 0, p[1], " ".join(p[2:])
            else:
                continue
            if typ in "TtWwVvIi":
                rows.append((addr, size, name))
        rows.sort(key=lambda r: r[0])
        self.addrs = [r[0] for r in rows]; self.rows = rows
    def find(self, name):
        for a, s, n in self.rows:
            if n == name or n.startswith(name + "("):
                return a
        return None
    def lookup(self, addr):
        a = addr & ~1
        i = bisect.bisect_right(self.addrs, a) - 1
        if i < 0:
            return None
        _, size, name = self.rows[i]
        if size and a >= self.addrs[i] + size and a - (self.addrs[i] + size) > 64:
            return None
        return name

# ---- subsystem mapping ----
def qualname(name):
    """'bool port::x<T>(args)' / 'LiveActor::calcAnim()' / 'MR::getFoo(int)' -> 'port::x' / 'LiveActor::calcAnim' / 'MR::getFoo': no return type, template args or parameters."""
    s = name.replace("(anonymous namespace)", "anon")
    out, depth = [], 0
    for ch in s:                      # drop <...> template arguments (operator< and operator<< are rare enough in hot code to ignore)
        if ch == "<": depth += 1
        elif ch == ">": depth = max(0, depth - 1)
        elif depth == 0: out.append(ch)
    s = "".join(out)
    i = s.find("(")
    if i >= 0 and s[max(0, i - 8):i] == "operator":
        i = s.find("(", i + 1)
    if i >= 0:
        s = s[:i]
    s = s.strip()
    return s.split(" ")[-1] if s else name

DEF_RE = re.compile(r"^[A-Za-z_~][\w:<>,\*&~ ]*?\b(?:(\w+)::)?(~?\w+)\s*\(")
class Groups:
    """name -> subsystem, from the source tree: the directory (two levels for Game/, one for NL, ode, renderer, platform/<dir>) of the file that defines the class / namespace / function."""
    def __init__(self, src_root):
        self.cls, self.fn = {}, {}
        self.count = collections.defaultdict(collections.Counter)
        roots = [("native/src", None), ("native/platform", None), ("renderer/src", "renderer"), ("native/include", None)]
        for sub, fixed in roots:
            base = os.path.join(src_root, sub)
            for d, dirs, files in os.walk(base):
                rel = os.path.relpath(d, base).split(os.sep)
                rel = [] if rel == ["."] else rel
                if "third_party" in rel or "vitagl" in rel or "tests" in rel or "test" in rel:
                    continue
                if fixed:
                    group = fixed
                elif sub == "native/src":
                    group = "/".join(rel[:2]) if rel and rel[0] == "Game" and len(rel) > 1 else (rel[0] if rel else "src")
                elif sub == "native/platform":
                    group = "platform/" + "/".join(rel[:1]) if rel else "platform"
                else:
                    group = "np/include"
                self._scan(d, files, group)
        for name, c in self.count.items():
            self.cls[name] = c.most_common(1)[0][0]
    def _scan(self, d, files, group):
        for f in files:
            if not f.endswith((".cpp", ".h", ".hpp", ".c", ".cc")):
                continue
            stem = f.rsplit(".", 1)[0]
            self.count[stem][group] += 1
            try:
                text = open(os.path.join(d, f), errors="replace").read()
            except OSError:
                continue
            for m in re.finditer(r"^\s*(?:class|struct|namespace)\s+(?:\w+\s+)?(\w+)\b", text, re.M):
                self.count[m.group(1)][group] += 1
            if f.endswith((".cpp", ".cc", ".c")):
                for line in text.splitlines():
                    m = DEF_RE.match(line)
                    if m and not line.rstrip().endswith(";"):
                        if m.group(1):
                            self.count[m.group(1)][group] += 1
                        self.fn.setdefault(m.group(2), group)
    PREFIX = [(("vgx", "vgxb", "Vgx"), "renderer"), (("Np", "np", "NpSampler", "VitaSampler"), "platform/port"),
              (("std", "__gnu_cxx", "__cxxabiv1", "__aeabi", "__cxa", "operator new", "operator delete"), "libc++/libc")]
    def group(self, name):
        q = qualname(name)
        parts = q.split("::")
        if len(parts) >= 2:
            if parts[0] in self.cls:
                return self.cls[parts[0]]
            for pre, g in self.PREFIX:
                if parts[0] in pre:
                    return g
            if parts[-2] in self.cls:
                return self.cls[parts[-2]]
        else:
            n = parts[0]
            if n in self.fn:
                return self.fn[n]
            if re.match(r"^(mem|str|__aeabi|__|_|abs|sin|cos|sqrt|atan|pow|exp|log|floor|ceil|fmod|printf|snprintf|malloc|free|calloc|realloc|qsort|rand|vfp|__vfp)", n):
                return "libc/libm"
            if n.startswith(("XXH", "stb_", "mbedtls_", "vglMem", "gpu_alloc", "reload_ffp")):
                return "third-party/vitaGL"
            if n.startswith(("sce", "vgl", "gl", "np_", "vita", "msc_")):
                return "platform/" + ("sce" if n.startswith("sce") else "port")
            if n.startswith(("nl", "NL")):
                return "NL"
        return "?"

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf"); ap.add_argument("bins", nargs="+")
    ap.add_argument("--thread"); ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--src", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--nm", default=NM_DEFAULT); ap.add_argument("--nm-text", help="read `nm -C -S -n` text from this file instead of running nm on ELF")
    ap.add_argument("--unmapped", action="store_true", help="list the top functions that no subsystem rule matched")
    a = ap.parse_args(argv)
    bins = [read_bin(p) for p in a.bins]
    if a.nm_text:
        text = open(a.nm_text).read()
    else:
        try:
            nm = a.nm if os.path.exists(a.nm) else (shutil.which("arm-vita-eabi-nm") or shutil.which("llvm-nm") or shutil.which("nm") or a.nm)
            text = subprocess.run([nm, "-C", "-S", "-n", "--defined-only", a.elf], check=True, capture_output=True, text=True).stdout
        except (OSError, subprocess.CalledProcessError) as e:
            raise SystemExit("cannot run %s on %s: %s" % (a.nm, a.elf, e))
    syms = Symbols(text)
    b0 = bins[0]
    delta = 0
    elf_anchor = syms.find(b0["aname"])
    if elf_anchor is not None and b0["anchor"]:
        delta = b0["anchor"] - elf_anchor
        how = "%s run time 0x%08x, ELF 0x%08x" % (b0["aname"], b0["anchor"], elf_anchor)
    elif b0["tbase"]:
        delta = b0["tbase"] - 0x81000000
        how = "text base 0x%08x vs 0x81000000 (anchor symbol not in the ELF: is it the ELF of this build?)" % b0["tbase"]
    else:
        how = "none known; offset taken as 0"
    print("load offset %+#x (%s)" % (delta, how))
    groups = Groups(os.path.abspath(a.src))
    secs = sum(b["interval"] for b in bins) / 1000.0
    print("files %s: tag %s, %.1f s in all, period %d us, dropped %d, failed reads %d" % (", ".join(os.path.basename(b["path"]) for b in bins), b0["tag"] or "?", secs,
          b0["period"], sum(b["dropped"] for b in bins), sum(b["failed"] for b in bins)))
    sus = sum(b["samplerus"] for b in bins)
    print("sampler cost: its own thread used %.2f%% of one core over the files; %d wake-ups, %d came 3 periods late" % (100.0 * sus / (secs * 1e6) if secs else 0, sum(b["ticks"] for b in bins), sum(b["late"] for b in bins)))
    names = {}
    cache = {}
    def fn(addr):
        if addr == 0:
            return "(no lr)"
        r = cache.get(addr)
        if r is None:
            r = cache[addr] = syms.lookup(addr - delta) or "?? 0x%08x" % addr
        return r
    def short(n):
        return n if len(n) <= 110 else n[:107] + "..."
    # merge by thread name (the uids / slots differ between files only if a thread restarted)
    per = collections.OrderedDict()
    over = collections.defaultdict(lambda: [0, 0, 0, 0, 0.0])
    for b in bins:
        for sl, info in b["slots"].items():
            t = per.setdefault(info["name"], dict(run=collections.Counter(), pair=collections.Counter(), wait=collections.Counter(), fails=0))
            t["fails"] += info["fails"]
            o = over[info["name"]]
            o[0] += info["stops"]; o[1] += info["hold"]; o[2] = max(o[2], info["holdmax"]); o[4] += b["interval"]
        for pc, lr, meta, n in b["ents"]:
            slot, kind, wtype = meta & 0xFF, (meta >> 8) & 1, meta >> 16
            if slot not in b["slots"]:
                continue
            t = per[b["slots"][slot]["name"]]
            if kind == 1:
                t["wait"][wtype] += n
            else:
                t["pair"][(pc, lr)] += n
    for name, t in per.items():
        if a.thread and name != a.thread:
            continue
        run = sum(t["pair"].values()); wsum = sum(t["wait"].values()); tot = run + wsum
        o = over[name]
        print("\n=== thread %s: %d samples, running %d (%.1f%%), waiting %d (%.1f%%)%s" % (name, tot, run, 100.0 * run / tot if tot else 0, wsum, 100.0 * wsum / tot if tot else 0,
              ("; sampler stopped it %.2f%% of the time (%d stops, avg %d us, max %d us)" % (100.0 * o[1] / (o[4] * 1000.0), o[0], o[1] // o[0], o[2])) if o[0] and o[4] else ""))
        for k, n in t["wait"].most_common(6):
            print("    wait %-12s %6d  %5.1f%%" % (WAIT_NAMES.get(k, str(k)), n, 100.0 * n / tot))
        if not run:
            continue
        selfc, inclc, gself, gincl = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
        for (pc, lr), n in t["pair"].items():
            f, c = fn(pc), fn(lr)
            selfc[f] += n; inclc[f] += n
            if c != f and lr:
                inclc[c] += n
            gs, gc = groups.group(f), groups.group(c) if lr else None
            gself[gs] += n; gincl[gs] += n
            if gc and gc != gs:
                gincl[gc] += n
        def table(title, c, top, fmt=short, denom=run):
            print("  %s" % title)
            for k, n in c.most_common(top):
                print("    %7d %5.1f%%  %s" % (n, 100.0 * n / denom if denom else 0, fmt(k)))
        table("subsystems, SELF (pc), of %d running samples" % run, gself, 40)
        table("subsystems, INCLUSIVE-BY-LR (pc's subsystem, plus the caller's when different)", gincl, 40)
        table("top functions, SELF", selfc, a.top, lambda k: "[%s] %s" % (groups.group(k), short(k)))
        table("top functions, INCLUSIVE-BY-LR (self + as the immediate caller)", inclc, a.top, lambda k: "[%s] %s" % (groups.group(k), short(k)))
        if a.unmapped:
            um = collections.Counter({k: v for k, v in selfc.items() if groups.group(k) == "?"})
            table("unmapped functions (self)", um, a.top)
    print("\n(inclusive-by-lr sees one caller level only: the lr of a leaf is its caller, the lr of a function that has called another is a return address inside itself.)")

if __name__ == "__main__":
    main()
