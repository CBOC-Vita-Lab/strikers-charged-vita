#!/usr/bin/env python3
"""Generates platform/sdk/save/np_save_layout.inc from clang's record layouts (tools/save_layouts.sh).

The game's save files are memory images of its structures, written on a big-endian machine by MWCC. The native game
writes the same structures from little-endian memory, so every multi-byte field (and every bit-field unit, whose
fields MWCC allocates from the most significant bit, clang from the least) has to be converted when a save is written
and read. This script turns the host layout of each structure into a table of runs:

    {offset, count, kind, bf}   kind 2 / 4 / 8: `count` consecutive 16 / 32 / 64-bit values to byte-swap
                                kind 'h' / 'w': one 16 / 32-bit bit-field unit (field widths in kBitUnits[bf])

Usage: gen_save_layout.py <layout dump> <output .inc>
Run on host32 and armhf (tools/save_layouts.sh): the tables are identical (the script does not know the target; compare the outputs).
"""
import re
import sys

SCALAR = {
    'u8': 1, 's8': 1, '_Bool': 1, 'bool': 1, 'char': 1, 'unsigned char': 1, 'signed char': 1,
    'u16': 2, 's16': 2, 'short': 2, 'unsigned short': 2,
    'u32': 4, 's32': 4, 'int': 4, 'unsigned int': 4, 'unsigned long': 4, 'long': 4, 'float': 4,
    'u64': 8, 's64': 8, 'unsigned long long': 8, 'long long': 8, 'double': 8,
}


class Rec:
    def __init__(self, name):
        self.name = name
        self.fields = []   # (offset, kind, text, indent, bitinfo)
        self.size = None


def parse(text):
    recs = {}
    cur = None
    for line in text.splitlines():
        m = re.match(r'^\s+0 \| (struct|class|union) (.+?)\s*$', line)
        if m and not line.startswith('  ' * 20):
            cur = Rec(m.group(2))
            recs.setdefault(cur.name, cur)
            recs[cur.name] = cur
            continue
        if cur is None:
            continue
        m = re.match(r'^\s*\| \[sizeof=(\d+)', line)
        if m:
            cur.size = int(m.group(1))
            cur = None
            continue
        m = re.match(r'^\s*(\d+)(?::(\d+)-(\d+))? \|( +)(.*)$', line)
        if m:
            off, b0, b1, ind, txt = m.groups()
            cur.fields.append((int(off), txt, len(ind), (int(b0), int(b1)) if b0 is not None else None))
    return recs


def split_type_name(txt):
    # "u16 mNumLowChargeShots", "eSidekickID[3] mValues", "struct CupSidekicks mUserSelectedSidekick",
    # "CupHistoryRecord[9][12] mRecords", "u8[64][12] unknown_0x040", "unsigned int mCaptain"
    toks = txt.rsplit(' ', 1)
    if len(toks) != 2:
        return txt, ''
    return toks[0], toks[1]


def type_info(t):
    """(base type name, dims) from "u16[3][4]" / "struct X" / "class Y"."""
    t = re.sub(r'^(struct|class|union) ', '', t.strip())
    dims = [int(x) for x in re.findall(r'\[(\d+)\]', t)]
    base = re.sub(r'\[\d+\]', '', t).strip()
    return base, dims


def flatten(recs, name, base_off=0, out=None, bf_units=None):
    """Appends (offset, kind, widths) entries for record `name` at byte offset base_off. kind: 1 bytes, 2, 4, 8 scalars,
    ('bf', unit_bytes, widths) bit-field units. Entries of one level are those whose indent is the field level of the
    record (direct members); nested structs are expanded from their own record."""
    rec = recs[name]
    if out is None:
        out = []
    fields = rec.fields
    # direct members: indent == minimal indent of the record's lines
    level = min(f[2] for f in fields) if fields else 0
    i = 0
    pending_bf = {}   # unit start byte -> (unit bytes, [(bitpos, width)])
    while i < len(fields):
        off, txt, ind, bit = fields[i]
        i += 1
        if ind != level:
            continue   # a nested member expanded below through its own record
        if txt.startswith('(') and 'vtable' in txt:
            continue
        if 'primary base' in txt or 'base)' in txt:
            continue
        t, n = split_type_name(txt)
        if '*' in t:
            out.append((base_off + off, 0, 'pointer %s' % n))   # a pointer: never inside a saved region (checked below)
            continue
        base, dims = type_info(t)
        count = 1
        for d in dims:
            count *= d
        if bit is not None:
            # a bit-field of the declared type: its storage unit is the declared size aligned at the field's byte
            unit = SCALAR.get(base, 4)
            b0, b1 = bit
            start_bit = off * 8 + b0
            width = b1 - b0 + 1
            # clang prints byte:bit0-bit1 with bit1 counted from the byte `off` (it can exceed 7)
            unit_start = (off // unit) * unit
            bitpos = (off - unit_start) * 8 + b0
            pending_bf.setdefault(unit_start, (unit, []))[1].append((bitpos, width))
            continue
        if base in SCALAR:
            sz = SCALAR[base]
            for k in range(count):
                out.append((base_off + off + k * sz, sz, None))
        elif base in recs:
            sub = recs[base]
            for k in range(count):
                flatten(recs, base, base_off + off + k * sub.size, out)
        else:
            # an enum (-fno-short-enums: 4 bytes): names starting with e + capital, or anything unknown but 4 bytes wide
            if base.startswith('e') or base in ('RECORDTYPE',):
                if base == 'RECORDTYPE':
                    for k in range(count):
                        out.append((base_off + off + k * 4, 4, None))
                else:
                    for k in range(count):
                        out.append((base_off + off + k * 4, 4, None))
            else:
                raise SystemExit('unknown type %r in %s (%s)' % (base, name, txt))
    for unit_start, (unit, flds) in sorted(pending_bf.items()):
        flds.sort()
        # fields must be packed from the least significant bit in declaration order, without gaps
        pos = 0
        widths = []
        for bitpos, width in flds:
            if bitpos != pos:
                raise SystemExit('bit-field gap in %s unit %d: %r' % (name, unit_start, flds))
            widths.append(width)
            pos += width
        if pos != unit * 8:
            raise SystemExit('bit-field unit of %s at %d is not full (%d of %d bits): %r' % (name, unit_start, pos, unit * 8, flds))
        out.append((base_off + unit_start, unit, tuple(widths)))
    return out


def runs_for(entries):
    """Coalesces scalar entries of equal size that are adjacent. Bit-field units stay single runs."""
    seen = set()
    uniq = []
    for e in entries:   # union members share an offset
        if e[1] == 0 or (e[0], e[1], e[2]) in seen:
            continue
        seen.add((e[0], e[1], e[2]))
        uniq.append(e)
    entries = sorted(uniq, key=lambda e: e[0])
    runs = []   # [off, count, kind, widths]
    for off, sz, widths in entries:
        if sz == 1 and widths is None:
            continue
        if widths is not None:
            runs.append([off, 1, 'h' if sz == 2 else 'w', widths])
            continue
        if runs and runs[-1][3] is None and runs[-1][2] == sz and runs[-1][0] + runs[-1][1] * sz == off:
            runs[-1][1] += 1
        else:
            runs.append([off, 1, sz, None])
    return runs


def main():
    recs = parse(open(sys.argv[1]).read())
    roots = [
        ('UserInfo', 'UserInfo', 0, None),
        ('BasicGameInfo', 'BasicGameInfo', 0, None),
        ('TeamStats', 'TeamStats', 0, None),
        ('CupProgress', 'CupManager', None, ('mState', 'mCurrentMode')),
        ('ChallengeUnlockRecord', 'ChallengeUnlockRecord', 0, None),
        ('GameInfoSaveSlot', 'GameInfoSaveSlot', 0, None),
    ]
    units = []   # distinct bit-field width tuples
    lines = []
    sizes = {}
    for label, rname, base, sl in roots:
        rec = recs.get(rname)
        if rec is None:
            raise SystemExit('record %s not in the dump' % rname)
        if sl is None:
            entries = flatten(recs, rname, 0)
            sizes[label] = rec.size
        else:
            # a slice of a big class: the direct members between two names
            lo = hi = None
            for off, txt, ind, bit in rec.fields:
                t, n = split_type_name(txt)
                if n == sl[0]:
                    lo = off
                if n == sl[1]:
                    hi = off + 4
            if lo is None or hi is None:
                raise SystemExit('slice %r not found in %s' % (sl, rname))
            full = flatten(recs, rname, 0)
            for (o, s, w) in full:
                if s == 0 and lo <= o < hi:
                    raise SystemExit('%s inside the saved slice of %s' % (w, rname))
            entries = [(o - lo, s, w) for (o, s, w) in full if lo <= o < hi and s != 0]
            sizes[label] = hi - lo
        runs = runs_for(entries)
        arr = []
        for off, count, kind, widths in runs:
            bf = 0
            if widths is not None:
                if widths not in units:
                    units.append(widths)
                bf = units.index(widths)
            k = kind if isinstance(kind, str) else str(kind)
            arr.append("{%d, %d, %s, %d}" % (off, count, ("'%s'" % k) if isinstance(kind, str) else k, bf))
        lines.append((label, sizes[label], arr))
    out = []
    out.append('// GENERATED by tools/gen_save_layout.py from tools/save_layouts.sh (clang record layouts). Do not edit.')
    out.append('// Byte-swap runs of the structures inside the game\'s save files; see platform/sdk/save/np_save_format.cpp.')
    out.append('static const uint8_t kBitUnits[][12] = {')
    for u in units:
        w = list(u) + [0] * (12 - len(u))
        out.append('    {%s},' % ', '.join(str(x) for x in w))
    out.append('};')
    for label, size, arr in lines:
        out.append('static const unsigned k%sSize = %d;' % (label, size))
        out.append('static const NpSaveRun kRuns%s[] = {' % label)
        row = []
        for a in arr:
            row.append(a)
            if len(row) == 6:
                out.append('    ' + ', '.join(row) + ',')
                row = []
        if row:
            out.append('    ' + ', '.join(row) + ',')
        out.append('};')
    open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
    for label, size, arr in lines:
        print('%-24s size %5d  runs %d' % (label, size, len(arr)))
    print('bit-field units:', len(units))


main()
