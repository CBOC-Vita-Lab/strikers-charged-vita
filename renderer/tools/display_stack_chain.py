#!/usr/bin/env python3
"""display_stack_chain.py DIR: worst-case stack of sceGxm's display-queue callback chain (gxvita14).
DIR holds *.o and *.su (clang -fstack-usage) of the units on the chain. Roots: _ZN12_GLOBAL__N_115DisplayCallbackEPv.
Call graph = the relocations of each function's code (llvm-objdump -dr); frame = its .su bytes; a callee that is in no unit is looked up in EXT.
Indirect calls (blx reg) are counted and reported, not followed. Sum along the deepest path + SYS_RESERVE must stay <= LIMIT (4096) - MARGIN.
Exit 0 pass, 1 over, 2 inconclusive."""
import glob, os, re, subprocess, sys
ROOT = "_ZN12_GLOBAL__N_115DisplayCallbackEPv"
LIMIT, MARGIN, SYS_RESERVE = 4096, int(os.environ.get("MARGIN", 1024)), 512
NEWLIB_PRINTF = 800   # vsnprintf's worst frames in newlib (Galaxy's disassembly of the linked eboot: 796)
# frames of what no unit here defines (bytes)
EXT = {n: 0 for n in ("sceDisplayGetVcount", "sceDisplayWaitVblankStart", "sceKernelGetProcessTimeWide", "getenv", "__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8",
                      "__aeabi_memset", "__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8", "memcpy", "memset", "sceKernelLockMutex", "sceKernelUnlockMutex",
                      "sceKernelGetThreadId", "sceKernelDelayThread", "sceKernelSignalCond", "sceKernelSignalSema", "sceKernelWaitSema")}
EXT.update({"__aeabi_ul2d": 16, "__aeabi_uldivmod": 24, "__aeabi_d2ulz": 16, "__aeabi_dmul": 16, "__aeabi_ddiv": 24,
            "np_display_last_vblank": 32,   # np_plat_vita.cpp (does not compile standalone with these flags; read: a leaf, 8 atomics loads, no calls)
            "floor": 16, "ceil": 16,   # libm leaves (vgx_smooth.h in SyncLog; newlib: no calls, a few registers)
            "snprintf": NEWLIB_PRINTF, "vsnprintf": NEWLIB_PRINTF, "fprintf": None, "malloc": 128, "free": 64})
# cold / error / absent-on-console names: noreturn or weak harness hooks (null on the Vita): 0. Small leaf helpers measured or read: np_time_* (a clock read), pthread_mutex_* (pthread-embedded stubs), atoi.
EXT.update({n: 0 for n in ("NpHostHeapExhausted", "NpVirtualNowNs", "NpVirtualTimeActive", "_ZSt20__throw_length_errorPKc", "_ZSt20__throw_system_errori", "_ZSt9terminatev",
                           "__cxa_begin_catch", "__cxa_end_cleanup", "abort", "__aeabi_memmove4", "__aeabi_memmove")})
EXT.update({"np_time_ns": 32, "np_time_real_ns": 32, "pthread_mutex_lock": 64, "pthread_mutex_unlock": 64, "__cxa_guard_acquire": 64, "__cxa_guard_release": 32,
            "__aeabi_l2d": 16, "atoi": 64, "strtol": 64, "_ZNSt6chrono3_V212steady_clock3nowEv": 32, "_Z11NpLanActivev": 32, "_Z17NpLanActiveSharedv": 16})   # NpLanActive: np_lan (a flag read)
# __real_vfprintf is the MSC_STDIO_LOCK=0 path (newlib FILE: the 1.2 behaviour, ~1 KiB __sbprintf buffer + a second vfprintf): off by default, reported, not counted
EXT["__real_vfprintf"] = 0; EXT["__real_fputc"] = 0; EXT["sceDisplaySetFrameBuf"] = 0
ALIAS = {"fputc": "__wrap_fputc", "fprintf": "__wrap_fprintf", "vfprintf": "__wrap_vfprintf", "printf": "__wrap_printf"}   # the logging build links with --wrap (vita.mk)

def main(d):
    frames, calls, indirect, dyn = {}, {}, {}, set()
    for su in glob.glob(os.path.join(d, "*.su")):
        for line in open(su):
            p = line.rstrip("\n").split("\t")
            if len(p) >= 3:
                name = p[0].rsplit(":", 1)[-1]
                frames[name] = max(frames.get(name, 0), int(p[1]))
                if p[2] != "static": dyn.add(name)
    for o in glob.glob(os.path.join(d, "*.o")):
        out = subprocess.run(["llvm-objdump", "-dr", "--no-show-raw-insn", o], capture_output=True, text=True).stdout
        cur = None
        for l in out.splitlines():
            m = re.match(r"^[0-9a-f]+ <(.+)>:$", l)
            if m: cur = m.group(1); calls.setdefault(cur, set()); continue
            if cur is None: continue
            m = re.search(r"R_ARM_(?:THM_)?(?:CALL|JUMP24|JUMP19|PC22|THM_JUMP\d+)\s+(\S+)", l)
            if m: calls[cur].add(m.group(1))
            if re.search(r"\bblx?\s+r\d+\b", l): indirect[cur] = indirect.get(cur, 0) + 1
    # the two indirect calls in the root: Services()->bootCompose (np_vita_wire.cpp VgxBootCompose -> VitaBootScreen::Compose, tail-called, +16 for the thunk) and a diag hook (NpDiagEvent, direct)
    calls[ROOT].add("_ZN14VitaBootScreen7ComposeEPv"); frames.setdefault("VgxBootComposeThunk", 16)
    memo, stack = {}, []
    def depth(n):
        n = ALIAS.get(n, n)
        if n in memo: return memo[n]
        if n in stack: return (0, [n + " (recursion)"])
        if n not in frames:
            if n in EXT and EXT[n] is not None: return (EXT[n], [f"{n}[ext {EXT[n]}]"])
            if n in calls: return (0, [n + "[no .su: inlined/empty]"])
            miss.add(n); return (0, [n + "[UNKNOWN]"])
        if n in dyn: print('dynamic frame on the chain:', n); sys.exit(1)
        stack.append(n)
        best = (0, [])
        for c in sorted(calls.get(n, ())):
            if c == n: continue
            r = depth(c)
            if r[0] > best[0]: best = r
        stack.pop()
        res = (frames[n] + best[0], [f"{n}[{frames[n]}]"] + best[1])
        memo[n] = res
        return res
    miss = set()
    if ROOT not in frames: print("root not found"); return 2
    total, path = depth(ROOT)
    print("worst chain:", " > ".join(re.sub(r"^_ZN12_GLOBAL__N_", "", x) for x in path))
    print(f"our code + libc along it: {total} bytes; + system reserve {SYS_RESERVE} = {total + SYS_RESERVE} of {LIMIT} (margin wanted {MARGIN})")
    if indirect.get(ROOT): print(f"note: {indirect[ROOT]} indirect call(s) in the root are not followed")
    if miss: print("UNKNOWN callees (add a unit or an EXT frame):", sorted(miss)); return 2
    return 0 if total + SYS_RESERVE <= LIMIT - MARGIN else 1
sys.exit(main(sys.argv[1]))
