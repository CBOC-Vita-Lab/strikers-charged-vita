#!/usr/bin/env python3
"""lblcheck.py <source.cpp>: (needs the flat DOL image of NPG's tools, dol.flat) compares every `<type> lbl_XXXXXXXX = value;` of the source with the DOL's initial value."""
import re, struct, sys
img = open('/home/claude/msc/npw/NPG-tools/dol.flat', 'rb').read(); B = 0x80004000
bad = 0; n = 0; uninit = []
for line in open(sys.argv[1]):
    m = re.match(r'^\s*(?:static\s+|const\s+)*(float|double|int|unsigned int|unsigned char|u8|u16|u32|s32|short|unsigned short|char|bool|long|unsigned long)\s+lbl_([0-9A-Fa-f]{8})\s*(?:=\s*([^;]+))?;', line)
    if not m: continue
    ty, addr, val = m.group(1), int(m.group(2), 16), m.group(3)
    if addr - B >= len(img):
        print('beyond flat image (bss?)', line.strip()); continue
    if val is None:
        sz = {'unsigned char':1,'u8':1,'char':1,'bool':1,'short':2,'unsigned short':2,'u16':2}.get(ty,4)
        uninit.append((addr, ty)); 
        raw = img[addr-B:addr-B+sz]
        if any(raw): print('UNINIT but DOL data nonzero: %08x %s raw=%s' % (addr, ty, raw.hex()))
        continue
    n += 1
    val = val.strip()
    try:
        if ty == 'float':
            v = float(val.rstrip('fF')); d = struct.unpack('>f', img[addr-B:addr-B+4])[0]
            ok = abs(struct.unpack('>f', struct.pack('>f', v))[0] - d) <= 1e-6 * max(1, abs(d)) or (d != d and v != v)
        elif ty == 'double':
            v = float(val.rstrip('fF')); d = struct.unpack('>d', img[addr-B:addr-B+8])[0]; ok = abs(v - d) <= 1e-12 * max(1, abs(d))
        elif ty in ('unsigned char', 'u8', 'char', 'bool'):
            v = int(val, 0) if val not in ('true','false') else int(val == 'true'); d = img[addr-B]; ok = (v & 0xff) == d
        elif ty in ('short', 'unsigned short', 'u16'):
            v = int(val, 0); d = struct.unpack('>H', img[addr-B:addr-B+2])[0]; ok = (v & 0xffff) == d
        else:
            v = int(val, 0); d = struct.unpack('>I', img[addr-B:addr-B+4])[0]; ok = (v & 0xffffffff) == d
    except Exception as e:
        print('cannot parse', line.strip(), e); continue
    if not ok:
        bad += 1; print('MISMATCH %08x %s src=%s dol=%s' % (addr, ty, val, d))
print('checked %d initialised labels, %d mismatches, %d uninitialised labels' % (n, bad, len(uninit)))
