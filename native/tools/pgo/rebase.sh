#!/bin/bash
# rebase.sh <in.profdata> <recorded tree root> <build tree root> <out.profdata>
# Internal-linkage functions are named "<source path>;<name>" in a profile. The renderer library is compiled from absolute paths, so a profile
# recorded in one checkout (tools/pgo/record.sh with VGX_PGO=1) names them under that checkout. This rewrites those paths to the tree a build
# compiles from (the Vita build's staged copy), through the text form of the profile. Game objects use relative paths and are not affected.
# 1.3 gxcpu14 (22 Nov; from Galaxy's tools/pgo, "merged with --sparse"): the same text pass also (a) drops the records of the renderer's byte-clearing loops
# (vgx::CmdBuf::clear, vector<u8/float, NoInitAlloc>::_M_default_append: 40 % of ALL the counts of msc-arm-1122, pure loop iterations) so the profile summary's
# hot threshold is the game's (99 % cut: 398,340 -> 199,120; cold: 14 -> 7), and (b) writes the profile --sparse: a function that no route ran gets NO record
# (compiled as unknown, -O2) instead of an all-zero record (which marks it `cold`: size-optimised, .text.unlikely, never inlined: 9,328 of the 17,582 functions of
# msc-arm-1122, among them Goalie::FumbleBall / StartStun / InitiatePanicGrab, which every real match reaches). PGO_SPARSE=0 keeps the old behaviour (A/B);
# PGO_DROP='regex' replaces the drop list (default below; empty = none).
set -e
IN=${1:?in}; FROM=${2:?recorded root}; TO=${3:?build root}; OUT=${4:?out}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
llvm-profdata-18 merge --text "$IN" -o "$T/p.txt"
PGO_DROP_RX=${PGO_DROP-'^_ZN3vgx6CmdBuf5clearEv$|^_ZNSt6vectorI[a-z]N3vgx11NoInitAllocI[a-z]EEE17_M_default_appendEj$|^_ZNSt3__16vectorI[a-z]N3vgx11NoInitAllocI[a-z]EEE8__appendEj$'}
python3 - "$T/p.txt" "${FROM%/}/" "${TO%/}/" "$PGO_DROP_RX" <<'P'
import sys, re
p, a, b, drop = sys.argv[1:]
s = open(p).read(); n = s.count(a)
s = s.replace(a, b); print(f'rebase: {n} names moved from {a} to {b}')
if drop:
    rx = re.compile(drop)
    blocks = s.strip('\n').split('\n\n')
    keep = [x for x in blocks if not rx.search(x.split('\n', 1)[0])]
    print(f'rebase: {len(blocks) - len(keep)} summary-skewing records dropped ({drop})')
    s = '\n\n'.join(keep) + '\n'
open(p, 'w').write(s)
P
llvm-profdata-18 merge $([ "${PGO_SPARSE:-1}" = 1 ] && echo --sparse) "$T/p.txt" -o "$OUT"
