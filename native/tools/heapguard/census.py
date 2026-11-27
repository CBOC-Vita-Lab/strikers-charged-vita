#!/usr/bin/env python3
"""MSC_HEAP_GUARD=2 census: which code allocates from the game's heaps through operator new on the game thread.

Usage: census.py <run.log> <executable> [--all] [--top N]

The game (NL/nlMemory.cpp) prints one "[heapcensus] <calls> <bytes> <off> <off> ..." line per distinct call stack at exit: the
callers of operator new, innermost first, as offsets from the executable's load base. This script symbolises them with
llvm-symbolizer (line tables: build with DEBUG=-gline-tables-only) and attributes each stack to its first frame outside the C++
library, operator new and the port's own container helpers. A stack whose attributed frame is in port code (renderer/, native/platform/,
native/include/np/, or an NP_NATIVE file of the game listed in PORT_GAME_FILES) is a finding: port storage in a game heap.
Game code allocating from its own heap is what the game was built for and is only counted.
"""
import collections
import os
import re
import shutil
import subprocess
import sys

PORT_DIRS = ("/renderer/", "/native/platform/", "/native/include/np/", "/native/tools/")
LIB_FUNCS = re.compile(r"^(std::|__gnu_cxx::|operator new|__cxx|void std::|.*std::(vector|basic_string|_Rb_tree|deque|_Hashtable|__detail|allocator|_Vector_base|unique_ptr|function)|.*\bnew_allocator\b)")


def load_base_adjust(exe):
    """EXEC images: dladdr's base is the first PT_LOAD's page; the offsets need it added back. DYN (PIE) images: 0."""
    out = subprocess.run(["readelf", "-lW", exe], capture_output=True, text=True).stdout
    if "EXEC (Executable file)" not in out:
        return 0
    for line in out.splitlines():
        p = line.split()
        if p and p[0] == "LOAD":
            return int(p[2], 16) & ~0xFFF
    return 0


def symbolizer():
    for name in ("llvm-symbolizer-18", "llvm-symbolizer"):
        if shutil.which(name):
            return name
    sys.exit("no llvm-symbolizer")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_all = "--all" in sys.argv
    top = 40
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
        args = [a for a in args if a != str(top)]
    log, exe = args[0], args[1]
    stacks = []
    total = None
    for line in open(log, errors="replace"):
        if not line.startswith("[heapcensus]"):
            continue
        if "total:" in line:
            total = line.strip()
            continue
        f = line.split()
        stacks.append((int(f[1]), int(f[2]), [int(x, 16) for x in f[3:]]))
    if not stacks:
        sys.exit("no [heapcensus] lines (MSC_HEAP_GUARD=2 on a host build?)")
    adj = load_base_adjust(exe)
    addrs = sorted({a for _, _, pcs in stacks for a in pcs})
    # return addresses point after the call: ask for the call instruction (address - 1)
    inp = "\n".join("0x%x" % (a + adj - 1) for a in addrs) + "\n"
    res = subprocess.run([symbolizer(), "--obj=" + exe, "--functions=linkage", "--demangle", "--inlines"], input=inp,
                         capture_output=True, text=True).stdout
    blocks = res.strip("\n").split("\n\n")
    sym = {}
    for a, b in zip(addrs, blocks):
        frames = []
        ls = b.split("\n")
        for i in range(0, len(ls) - 1, 2):
            frames.append((ls[i], ls[i + 1]))
        sym[a] = frames  # inlined frames first (innermost), then the real function
    by_site = collections.defaultdict(lambda: [0, 0, None])
    port_calls = port_bytes = 0
    for calls, nbytes, pcs in stacks:
        site = None
        chain = []
        for a in pcs:
            for fn, loc in sym.get(a, []):
                chain.append((fn, loc))
        for fn, loc in chain:
            if LIB_FUNCS.match(fn) or "/c++/" in loc or "nlMemory.cpp" in loc or "host_alloc.h" in loc:
                continue
            site = (fn, loc)
            break
        if site is None:
            site = chain[0] if chain else ("?", "?")
        is_port = any(d in site[1] for d in PORT_DIRS)
        key = (is_port, site[0], re.sub(r":\d+$", "", site[1]))
        e = by_site[key]
        e[0] += calls
        e[1] += nbytes
        if e[2] is None:
            e[2] = chain[:6]
        if is_port:
            port_calls += calls
            port_bytes += nbytes
    print(total or "")
    print("port code allocating from a game heap: %d calls, %d bytes" % (port_calls, port_bytes))
    rows = sorted(by_site.items(), key=lambda kv: (not kv[0][0], -kv[1][1]))
    shown = 0
    for (is_port, fn, loc), (calls, nbytes, chain) in rows:
        if not is_port and not show_all and shown >= top:
            continue
        print("%s %8d calls %10d bytes  %s  (%s)" % ("PORT" if is_port else "game", calls, nbytes, fn[:110], loc.replace(os.getcwd() + "/", "")))
        if is_port:
            for cfn, cloc in chain:
                print("        <- %s  %s" % (cfn[:100], cloc.split("/")[-1]))
        shown += 1


if __name__ == "__main__":
    main()
