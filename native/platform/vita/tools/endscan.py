#!/usr/bin/env python3
"""Finds functions of a linked Vita image that END in something that is not a terminator: the signature of a call that GCC proved never
returns (an __builtin_unreachable() it derived from undefined behaviour in the decomp), after which execution runs into the literal pool or the
next function. The first Vita build died that way in RegistryOwner::Relocate (np/docs/wave2-NPV2.md 2.3).
    arm-vita-eabi-objdump -d -C --no-show-raw-insn <elf> > image.dis ; endscan.py image.dis
Padding (nop, movs r0,r0) and the known noreturn callees are skipped; what is printed needs a look. It cannot see an unreachable in the MIDDLE of a function."""
import re, sys
hdr = re.compile(r'^([0-9a-f]{8}) <(.*)>:$')
ins_re = re.compile(r'^\s*([0-9a-f]+):\t(.*)$')
noret = ('__cxa_throw', 'abort', 'exit', '_Exit', '__cxa_end_cleanup', '__cxa_call_unexpected', 'terminate', '__throw', '__assert', 'NpFatal', '_exit',
         'longjmp', 'pthread_exit', 'sceKernelExit', '__cxa_pure_virtual', '__stack_chk_fail', '__cxa_rethrow', 'Die(', 'EndRun', 'RealExit', 'thread_finish',
         'NpPlatformExit', 'Finish(', '_Unwind_', '__restore_core_regs', 'unwind_phase2', '__unexpected', '__terminate')
cur = None; last = None; res = []
for line in open(sys.argv[1], errors='replace'):
    line = line.rstrip('\n')
    m = hdr.match(line)
    if m:
        if cur and last: res.append((cur, last))
        cur = (m.group(1), m.group(2)); last = None; continue
    m = ins_re.match(line)
    if m:
        ins = re.sub(r'\s+', ' ', m.group(2).strip())
        if ins.startswith('.') or ins.startswith('nop') or ins == 'movs r0, r0': continue
        last = ins
if cur and last: res.append((cur, last))
bad = []
for (a, n), ins in res:
    op = ins.split()[0]
    if op in ('b', 'b.n', 'b.w', 'bx', 'udf', 'ldr.w', 'ldr') and (op != 'ldr' and op != 'ldr.w' or 'pc' in ins): continue
    if op.startswith('pop') or op.startswith('ldm'):
        if 'pc' in ins: continue
    if op in ('bl', 'blx') and any(x in ins for x in noret): continue
    if '_from_thumb' in n or 'add pc' in ins or 'mov pc' in ins: continue
    bad.append((a, n, ins))
print(f"{len(res)} functions, {len(bad)} end in a non-terminator")
for a, n, i in bad[:200]: print(a, n[:110], '::', i[:80])
