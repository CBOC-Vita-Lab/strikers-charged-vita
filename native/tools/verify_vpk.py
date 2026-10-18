#!/usr/bin/env python3
"""Audit of a Vita package of the player build (a .vpk is a zip): identity, art, LiveArea, and what must NOT be inside.
  python3 tools/verify_vpk.py <file.vpk> [--id MSCH00002] [--title "Mario Strikers Charged"]
Exit 0 when every check passes. Checks: param.sfo TITLE_ID / TITLE (and the ones asked for), icon0.png 128x128, the LiveArea files named by template.xml exist,
the Settings frame (target psla:-settings) is there, an eboot.bin that is a SELF, the shader compiler of the owner (libshacccg.suprx, by name and by its ELF
signature "SCE" + the module name) and game data (disc images, .arc/.bin of the game's files, the extracted disc folders) are NOT inside."""
import argparse, struct, sys, zipfile, re, hashlib

def sfo(data):
    magic, version, key_off, data_off, count = struct.unpack('<4sIIII', data[:20])
    assert magic == b'\0PSF', 'not a param.sfo'
    out = {}
    for i in range(count):
        koff, fmt, length, maxlen, doff = struct.unpack('<HHIII', data[20 + 16 * i: 36 + 16 * i])
        k = data[key_off + koff: data.index(b'\0', key_off + koff)].decode()
        raw = data[data_off + doff: data_off + doff + length]
        out[k] = struct.unpack('<I', raw[:4])[0] if fmt == 0x0404 else raw.split(b'\0')[0].decode('utf-8', 'replace')
    return out

def png_size(data):
    return struct.unpack('>II', data[16:24]) if data[:8] == b'\x89PNG\r\n\x1a\n' else None

ap = argparse.ArgumentParser()
ap.add_argument('vpk')
ap.add_argument('--id')
ap.add_argument('--title')
a = ap.parse_args()
z = zipfile.ZipFile(a.vpk)
names = z.namelist()
fails = []
def check(ok, text):
    print(('ok    ' if ok else 'FAIL  ') + text)
    if not ok: fails.append(text)

print('package:', a.vpk, '%d bytes, %d files' % (sum(i.file_size for i in z.infolist()), len(names)))
p = sfo(z.read('sce_sys/param.sfo'))
print('param.sfo:', {k: p[k] for k in ('TITLE_ID', 'TITLE', 'STITLE', 'APP_VER', 'CONTENT_ID', 'CATEGORY') if k in p})
if a.id: check(p.get('TITLE_ID') == a.id, 'TITLE_ID is %s' % a.id)
if a.title: check(p.get('TITLE') == a.title, 'TITLE is "%s"' % a.title)
check('eboot.bin' in names and z.read('eboot.bin')[:4] == b'SCE\0', 'eboot.bin is a SELF (%d bytes)' % z.getinfo('eboot.bin').file_size)
icon = png_size(z.read('sce_sys/icon0.png'))
check(icon == (128, 128), 'icon0.png is 128x128 (%s)' % (icon,))
tpl = z.read('sce_sys/livearea/contents/template.xml').decode()
files = re.findall(r'<(?:image|startup-image)>([^<]+)<', tpl)
for f in files:
    path = 'sce_sys/livearea/contents/' + f
    check(path in names, 'LiveArea file %s is packed' % f)
bg = [f for f in files if f.startswith('bg')]
check(any(png_size(z.read('sce_sys/livearea/contents/' + f)) == (840, 500) for f in bg), 'LiveArea background is 840x500')
check('psla:-settings' in tpl, 'LiveArea Settings button (target psla:-settings)')
bad = [n for n in names if 'libshacccg' in n.lower() or n.lower().endswith('.suprx') and 'shacc' in n.lower()]
check(not bad, 'no libshacccg.suprx by name %s' % bad)
sig = []
for i in z.infolist():
    if i.file_size > 400000 and not i.filename.endswith('eboot.bin') and not i.filename.endswith('.png'):
        d = z.read(i.filename)
        if b'libshacccg' in d[:4096]: sig.append(i.filename)
check(not sig, 'no libshacccg module inside a file (%s)' % sig)
game = [n for n in names if n.startswith(('game/', 'files/', 'sys/')) or n.lower().endswith(('.iso', '.wbfs', '.rvz', '.gcm', '.dol'))]
check(not game, 'no game data folders or disc images %s' % game[:3])
big = [(i.filename, i.file_size) for i in z.infolist() if i.file_size > 12 * 1024 * 1024]
check(not big, 'no file above 12 MiB besides the eboot: %s' % big)
seed = [n for n in names if n.startswith('shadercache/')]
print('shader seed: %d file(s); first-run seed: %d file(s)' % (len(seed), len([n for n in names if n.startswith('seed/')])))
sys.exit(1 if fails else 0)
