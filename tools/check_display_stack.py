#!/usr/bin/env python3
"""check_display_stack.py: the display-queue callback thread's stack gate for Strikers (22 Nov 2026, from Galaxy's tools/check_display_stack.py, stackgate31).

Why: sceGxm's display queue runs vitaGL's display callback (vglSetDisplayCallback: DisplayCallback in renderer/vita/vgx_host_vita.cpp) on a SYSTEM thread whose
stack is 4 KiB. A few hundred bytes of buffers in that call chain overflow it (Galaxy's console died twice in newlib's __ssputs_r / memmove; Strikers' DisplayCallback
had a 1152-byte frame in integ/next: `char line[900]`). Nothing in a build notices. This gate does.

Unlike Galaxy's gate (a hand-written table of functions and chains that a new callee escapes) this one READS THE LINKED ELF: it takes the callback's symbol, disassembles it,
finds every direct call (bl / b.w to a symbol), does the same for each callee (recursively), computes each function's frame from its prologue (pushes, vpush, `sub sp`) and
reports the worst chain. A new callee is seen by itself. Limits: calls through a pointer (blx rN, a vtable) are counted and listed but not followed; the C library
(newlib) functions are not walked: a call to printf / snprintf / vsnprintf ... adds NEWLIB_PRINTF (800 bytes, from Galaxy's disassembly of the linked eboot) once; a
dynamic frame (a `sub sp, sp, rN` or alloca) cannot be bounded and is a failure; the frame of a function is the static prologue only (a callee's own pushes are its frame).

  check_display_stack.py ELF [--root-regex RX ...] [--budget 2560] [--hard-only] [--show N] [--json]
      ELF    the UNSTRIPPED final link of the Vita build (the .elf next to eboot.bin), the same build that is tested
  check_display_stack.py --self-test      the parser and the walk on a synthetic disassembly, with deliberately broken variants that must fail
Exit: 0 pass, 1 a budget is breached, 2 inconclusive (no ELF, the root symbol not found, a tool is missing): never a pass.
Budgets: the thread's stack is 4096 bytes; the system's share (display_queue_callback and the kernel's entry) SYS_RESERVE 512; our chain (frames summed, newlib not
counted) must stay within --budget (default 2560, Galaxy's figure; --hard-only checks only the 4096-byte limit).
Needs arm-vita-eabi-nm / -objdump (VitaSDK) on PATH or /opt/vitasdk/bin, and c++filt through nm -C. Pure Python 3.
"""
import argparse, json, os, re, subprocess, sys

STACK = 4096
OUR_BUDGET = 2560
NEWLIB_PRINTF = 800
SYS_RESERVE = 512
DEFAULT_ROOTS = [r"\(anonymous namespace\)::DisplayCallback\(void\*\)$", r"(^|::)DisplayCallback\(void\*\)$"]
# C library / runtime entry points that are not walked (their frames come from newlib, not from our code). printf family: +NEWLIB_PRINTF once.
LIBC_PRINTF = re.compile(r"^(v?f?s?n?printf|vsnprintf|vfprintf|fprintf|snprintf|sprintf|puts|fputs|fwrite|fflush|_vfprintf_r|_svfprintf_r|port_snprintf|port_printf)$")
LIBC_LEAF = re.compile(r"^(mem(cpy|set|move|cmp|chr)|str(len|cpy|ncpy|cmp|ncmp|chr|cat)|__aeabi_\w+|__gnu_\w+|__\w*div\w*|sce[A-Z]\w*|_?_?(ldiv|ldivmod)\w*)$")

# error / exit paths are not part of a normal pass of the callback (a throw, an allocation failure report, an assert): not walked, so that the worst chain is the one that runs
ERROR_PATH = re.compile(r"(__throw|__cxa_|_error\b|ReportAllocationFailure|AllocFailed|HeapExhausted|(^|::)abort$|terminate|__assert|NpFatal|Fatal|_Unwind)")
PUSH = re.compile(r"^(push|stmdb\s+sp!|stmfd\s+sp!)(?:\.w)?\s*,?\s*\{([^}]*)\}")
VPUSH = re.compile(r"^(vpush|vstmdb\s+sp!)\s*\{([^}]*)\}")
SUBSP = re.compile(r"^sub(?:w)?(?:\.w)?\s+sp,\s*(?:sp,\s*)?#(\d+)")
SUBSP_REG = re.compile(r"^sub(?:\.w)?\s+sp,\s*(?:sp,\s*)?r\d+")
CALL = re.compile(r"^(bl|b\.w|b|blx)(?:\.w)?\s+([0-9a-f]+)\s+<([^>+]+)(?:\+0x[0-9a-f]+)?>")
BLX_REG = re.compile(r"^blx\s+(r\d+|ip|lr)")


def reg_count(spec):
    n = 0
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-")
            n += int(re.sub(r"\D", "", b)) - int(re.sub(r"\D", "", a)) + 1
        else:
            n += 1
    return n


def parse_function(lines):
    """-> (frame bytes, dynamic?, [callee names], indirect call count) from the disassembly lines of ONE function (objdump -d --no-show-raw-insn)."""
    frame, dynamic, callees, indirect = 0, False, [], 0
    first = True
    for raw in lines:
        m = re.match(r"^\s*([0-9a-f]+):\s+(.*)$", raw)
        if not m:
            continue
        ins = m.group(2).split("@")[0].strip()
        ins = re.sub(r"\s+", " ", ins)
        if first and re.match(r"^<", ins):
            continue
        p = PUSH.match(ins)
        if p:
            frame += 4 * reg_count(p.group(2)); continue
        p = VPUSH.match(ins)
        if p:
            frame += 8 * reg_count(p.group(2)); continue
        p = SUBSP.match(ins)
        if p:
            frame += int(p.group(1)); continue
        if SUBSP_REG.match(ins):
            dynamic = True; continue
        if BLX_REG.match(ins):
            indirect += 1; continue
        c = CALL.match(ins)
        if c:
            kind, _addr, name = c.groups()
            if kind in ("bl", "blx") or (kind.startswith("b") and name and not re.search(r"\+0x", raw) and not raw.rstrip().endswith("]")):
                if kind in ("bl", "blx") or True:
                    callees.append((kind, name))
    return frame, dynamic, callees, indirect


class Elf:
    def __init__(self, path, tooldir=None):
        self.path = path
        env = dict(os.environ)
        env["PATH"] = env.get("PATH", "") + os.pathsep + "/opt/vitasdk/bin" + (os.pathsep + tooldir if tooldir else "")
        self.env = env
        self.syms = {}      # mangled name -> (addr, size)
        self.demangled = {}  # mangled -> demangled
        self.cache = {}

    def load(self):
        out = subprocess.run(["arm-vita-eabi-nm", "-S", "-n", self.path], capture_output=True, text=True, env=self.env)
        outd = subprocess.run(["arm-vita-eabi-nm", "-S", "-n", "-C", self.path], capture_output=True, text=True, env=self.env)
        if out.returncode != 0:
            raise RuntimeError("nm failed: " + out.stderr[-200:])
        a, b = out.stdout.splitlines(), outd.stdout.splitlines()
        for l, d in zip(a, b):
            f = l.split(None, 3)
            if len(f) < 4 or f[2] not in "tTwW":
                continue
            addr, size, name = int(f[0], 16), int(f[1], 16), f[3]
            self.syms[name] = (addr, size)
            fd = d.split(None, 3)
            self.demangled[name] = fd[3] if len(fd) == 4 else name

    def lines(self, name):
        if name in self.cache:
            return self.cache[name]
        addr, size = self.syms[name]
        r = subprocess.run(["arm-vita-eabi-objdump", "-d", "--no-show-raw-insn", f"--start-address={addr & ~1:#x}", f"--stop-address={(addr & ~1) + max(size, 2):#x}", self.path],
                           capture_output=True, text=True, env=self.env)
        self.cache[name] = r.stdout.splitlines()
        return self.cache[name]


def walk(get, root, libc_printf=LIBC_PRINTF, libc_leaf=LIBC_LEAF, max_depth=40):
    """get(name) -> (frame, dynamic, callees, indirect) or None for an unknown symbol. Returns (worst chain bytes, path, printf_seen, dynamic list, indirect total, visited count, callees without code)."""
    best = {}      # name -> (bytes, path, printf)
    dyn, ind, unk = [], [0], []
    stack = []

    def go(name, depth):
        if name in best:
            return best[name]
        if name in stack or depth > max_depth:
            return (0, [name + " (cycle)"], False)
        info = get(name)
        if info is None:
            if name not in unk:
                unk.append(name)
            return (0, [], False)
        frame, dynamic, callees, indirect = info
        if dynamic:
            dyn.append(name)
        ind[0] += indirect
        stack.append(name)
        w = (0, [], False)
        pf = False
        for _kind, c in callees:
            base = re.sub(r"\(.*", "", c.split("::")[-1])
            if libc_printf.match(c) or libc_printf.match(base):
                pf = True; continue
            if libc_leaf.match(c) or libc_leaf.match(base) or ERROR_PATH.search(c):
                continue
            sub = go(c, depth + 1)
            if sub[0] > w[0]:
                w = sub
            pf = pf or sub[2]
        stack.pop()
        res = (frame + w[0], [f"{name}:{frame}"] + w[1], pf or w[2])
        best[name] = res
        return res

    r = go(root, 0)
    return r[0], r[1], r[2], dyn, ind[0], len(best), unk


def evaluate(total, printf, budget=OUR_BUDGET, hard_only=False):
    full = total + (NEWLIB_PRINTF if printf else 0) + SYS_RESERVE
    fails = []
    if total > budget and not hard_only:
        fails.append(f"our frames along the worst chain sum to {total} bytes > the budget {budget}")
    if full > STACK:
        fails.append(f"{total} + newlib {NEWLIB_PRINTF if printf else 0} + system {SYS_RESERVE} = {full} bytes > the {STACK}-byte stack")
    return full, fails


SYNTH = {
    "Root": ["00000010 <Root>:", "10: push {r4, r5, lr}", "12: sub sp, #20", "14: bl 20 <Mid>", "18: bl 30 <snprintf>", "1c: pop {r4, r5, pc}"],
    "Mid": ["00000020 <Mid>:", "20: stmdb sp!, {r4, r5, r6, r7, r8, lr}", "24: sub.w sp, sp, #200", "28: bl 40 <Leaf>", "2c: blx r3", "30: add.w sp, sp, #200"],
    "Leaf": ["00000040 <Leaf>:", "40: push {r4, lr}", "42: vpush {d8-d9}", "46: sub sp, #64", "48: bx lr"],
}


def self_test():
    def table(over=None):
        t = {k: parse_function(v) for k, v in SYNTH.items()}
        if over:
            t.update(over)
        return lambda n: t.get(n)
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + name)
        ok = ok and cond
    f = parse_function(SYNTH["Leaf"])
    check("prologue: push 2 regs + vpush 2 doubles + sub 64 = 8 + 16 + 64", f[0] == 88 and not f[1])
    f = parse_function(SYNTH["Mid"])
    check("stmdb of six registers + sub.w 200, one indirect call", f[0] == 224 and f[3] == 1 and f[2] == [("bl", "Leaf")])
    total, path, pf, dyn, ind, n, unk = walk(table(), "Root")
    check("worst chain Root 32 + Mid 224 + Leaf 88 = 344, printf seen, 1 indirect", total == 344 and pf and ind == 1 and n == 3)
    full, fails = evaluate(total, pf)
    check("passes the 4 KiB limit: 344 + 800 + 512", full == 1656 and not fails)
    # mutants: a 1100-byte buffer in the leaf (the Strikers DisplayCallback bug), and a dynamic frame
    big = dict(Leaf=parse_function(["00000040 <Leaf>:", "40: push {r4, lr}", "46: sub.w sp, sp, #2600", "48: bx lr"]))
    total2, _, pf2, _, _, _, _ = walk(table(big), "Root")
    _, fails2 = evaluate(total2, pf2)
    check("MUTANT: a 2600-byte buffer in the leaf is caught", bool(fails2))
    dynf = dict(Leaf=parse_function(["00000040 <Leaf>:", "40: push {r4, lr}", "42: sub.w sp, sp, r3", "48: bx lr"]))
    _, _, _, dyn3, _, _, _ = walk(table(dynf), "Root")
    check("MUTANT: a dynamic frame is reported", dyn3 == ["Leaf"])
    missing = dict(Leaf=None)
    _, _, _, _, _, _, unk4 = walk(table(missing), "Root")
    check("a callee without code is named, not guessed", unk4 == ["Leaf"])
    print("self-test " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="?")
    ap.add_argument("--root-regex", action="append", help="demangled name of the callback (default: DisplayCallback(void*))")
    ap.add_argument("--budget", type=int, default=OUR_BUDGET)
    ap.add_argument("--hard-only", action="store_true")
    ap.add_argument("--show", type=int, default=12, help="lines of the worst chain to print")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.elf or not os.path.exists(a.elf):
        print("stack INCONCLUSIVE: give the unstripped ELF of the Vita build"); return 2
    elf = Elf(a.elf)
    try:
        elf.load()
    except (OSError, RuntimeError) as e:
        print(f"stack INCONCLUSIVE: {e}"); return 2
    rxs = [re.compile(r) for r in (a.root_regex or DEFAULT_ROOTS)]
    roots = [n for n, d in elf.demangled.items() if any(r.search(d) for r in rxs) and n in elf.syms]
    if not roots:
        print("stack INCONCLUSIVE: no symbol matches " + " | ".join(r.pattern for r in rxs) + " (a stripped ELF, or the callback was renamed)"); return 2

    def get(name):
        if name not in elf.syms:
            return None
        return parse_function(elf.lines(name))
    worst = None
    for root in roots:
        total, path, pf, dyn, ind, n, unk = walk(get, root)
        full, fails = evaluate(total, pf, a.budget, a.hard_only)
        if dyn:
            fails.append("dynamic frame (alloca / VLA) in: " + ", ".join(elf.demangled.get(d, d) for d in dyn))
        r = dict(root=elf.demangled[root], chain_bytes=total, printf=pf, full=full, margin=STACK - full, functions=n, indirect_calls=ind, path=[elf.demangled.get(p.split(":")[0], p.split(":")[0]) + ":" + p.split(":")[-1] if ":" in p else p for p in path], unknown=unk, fails=fails)
        if worst is None or r["full"] > worst["full"]:
            worst = r
    if a.json:
        print(json.dumps(worst, indent=1))
    else:
        print(f"display thread: {worst['root']}: {worst['functions']} functions walked, {worst['indirect_calls']} indirect calls not followed")
        print(f"worst chain: our frames {worst['chain_bytes']} bytes" + (f" + newlib printf {NEWLIB_PRINTF}" if worst["printf"] else "") + f" + system {SYS_RESERVE} = {worst['full']} of {STACK} (margin {worst['margin']})")
        for p in worst["path"][:a.show]:
            print("   " + p)
        if worst["unknown"]:
            print("callees with no code in the ELF (PLT / imports / not linked; not counted): " + ", ".join(worst["unknown"][:12]) + (" ..." if len(worst["unknown"]) > 12 else ""))
    if worst["fails"]:
        for f in worst["fails"]:
            print("stack FAIL: " + f)
        print("stack FAIL: the display-queue callback thread has a 4 KiB stack: make buffers static (thread-confined) or hand the work to another thread")
        return 1
    print(f"stack ok: worst display-thread chain {worst['full']} of {STACK} bytes (margin {worst['margin']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
