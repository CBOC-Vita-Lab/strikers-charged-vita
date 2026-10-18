#!/usr/bin/env python3
"""API coverage table: every SDK function the game calls, who provides it in the native build, and what differs.

  gx_coverage.py --calls calls.tsv --glue libnpgx.a --vgx libvgx-native.a [--abi abi_functions.tsv] > table.md

provider: glue = defined by platform/gx (Wii signature / native semantics); vgx = vgx_api.cpp's own definition is linked
straight into the game (same ABI, checked by gx_abi.py); sdk = the SDK layer (NPS): VI and OS pieces; MISSING = nobody.
"""
import argparse, re, subprocess, sys

NOTES = {
 'GXInit': 'starts the renderer when the platform code has not (np_gx_start); the FIFO memory is not used',
 'GXInitFifoLimits': 'no hardware FIFO: no-op',
 'GXBegin': 'opens a raw primitive sized from the shadow vertex descriptor/format; the vertex bytes follow through WGPIPE',
 'GXSetArray': 'the SDK form (no size, no byte order) is the glue\'s: by reference, size unbounded, byte order = np_gx_set_array_default_le (big-endian); the 186 material-program sites use NP_GX_SETARRAY_STREAM (patch 0003): the glModelStream says host-order or big-endian',
 'GXSetVtxDesc': 'indexed attributes stay indexed (INDEX8/16); matrix-index attributes direct',
 'GXSetVtxAttrFmt': '',
 'GXClearVtxDesc': '',
 'GXCallDisplayList': 'host pointer; the list must be big-endian bytes (NPA patch 0006); by reference + display-list cache when the heap is registered (np_gx_register_ram), else copied',
 'GXInvalidateVtxCache': 'no-op',
 'GXInitTexObj': 'compact 32-byte object (game embeds u32[8]); vgx object is expanded at GXLoadTexObj',
 'GXInitTexObjCI': 'compact object',
 'GXInitTexObjLOD': 'compact object; LOD limits/bias quantised like the SDK (1/16, 1/32)',
 'GXInitTexObjWrapMode': 'compact object',
 'GXInitTexObjTlut': 'compact object',
 'GXLoadTexObj': 'expands the compact object into vgx\'s texture description; data stays big-endian console format',
 'GXInitTlutObj': 'compact 12-byte object',
 'GXLoadTlut': 'palette re-validated on every load',
 'GXInvalidateTexAll': 'with registered heap: no re-hash (write generations decide); else textures are re-hashed on bind',
 'GXGetTexBufferSize': '',
 'GXCopyDisp': 'queues the display copy AND presents the frame (vgxn_end_frame), like the recompiled build\'s HLE; then np_gx_present_hook',
 'GXCopyTex': 'queues the EFB->texture copy; remembers the destination so a data-cache flush over it retires the GPU texture (np_gx_notify_write)',
 'GXSetCopyClear': 'BP 0x4F/0x50/0x51',
 'GXSetCopyFilter': 'no-op (picture is not filtered on the way to the frame buffer)',
 'GXSetDispCopyYScale': 'returns the XFB line count the SDK would compute; no vertical scaling pass',
 'GXGetYScaleFactor': 'SDK arithmetic',
 'GXAdjustForOverscan': 'SDK arithmetic',
 'GXSetFog': 'SDK argument order (type, colour, start, end, near, far) mapped to vgx\'s Dolphin order',
 'GXSetMisc': 'no-op',
 'GXDrawDone': 'no-op: the frame\'s commands are consumed at present',
 'GXFlush': 'no-op',
 'GXSetDrawDone': 'completes at once and calls the GXSetDrawDoneCallback callback (as the recompiled build does)',
 'GXSetDrawDoneCallback': '',
 'GXWaitDrawDone': 'nothing to wait for',
 'GXReadDrawSync': 'last GXSetDrawSync token',
 'GXGetGPStatus': 'idle processor',
 'GXAbortFrame': 'no-op',
 'GXPokeARGB': 'no-op (the recompiled build stubs it too)',
 'GXPokeBlendMode': 'no-op', 'GXPokeColorUpdate': 'no-op',
 'GXPeekARGB': 'reads black',
 'GXInitLightAttnA': 'SDK arithmetic on the hardware light layout', 'GXInitLightDistAttn': 'SDK arithmetic', 'GXInitSpecularDir': 'SDK arithmetic',
 'GXGetProjectionv': 'from the XF register shadow', 'GXSetProjectionv': 'rebuilt as GXSetProjection',
 'GXGetViewportv': 'rebuilt from the XF register shadow', 'GXProject': 'SDK arithmetic (the game declares it itself)',
 'GXSetTexCoordScaleManually': 'no-op in vgx (coordinates are normalised)',
 'TPLBind': 'SDK layer (NPS: ref tpl/TPL.c, structure words swapped in place by NpTplFixEndian); not in the glue',
 'TPLGet': 'SDK layer (NPS)',
 'VIConfigure': 'SDK layer; may call np_gx_set_efb_size(fbWidth, efbHeight) (optional: the glue also takes the size from GXSetDispCopySrc)',
 'GXSetDispCopySrc': 'vgx\'s, plus: a rectangle at the top-left corner sets the renderer\'s EFB size unless the SDK layer reported one',
 'VIGetRetraceCount': 'SDK layer', 'VIWaitForRetrace': 'SDK layer: the game paces itself with it', 'VIFlush': 'SDK layer', 'VISetNextFrameBuffer': 'SDK layer',
 'VISetBlack': 'SDK layer', 'VIGetTvFormat': 'SDK layer', 'VIGetDTVStatus': 'SDK layer', 'VIInit': 'SDK layer',
 'VISetPreRetraceCallback': 'SDK layer', 'VISetPostRetraceCallback': 'SDK layer', 'VISetTimeToDimming': 'SDK layer',
}

def syms(lib):
    out = subprocess.run(['nm', '-g', '--defined-only', lib], capture_output=True, text=True).stdout
    return set(l.split()[-1] for l in out.splitlines() if re.match(r'^[0-9a-f]+ [TDBR] ', l))

ap = argparse.ArgumentParser()
ap.add_argument('--calls', required=True); ap.add_argument('--glue', required=True); ap.add_argument('--vgx', required=True); ap.add_argument('--abi')
a = ap.parse_args()
glue, vgx = syms(a.glue), syms(a.vgx)
abi = {}
if a.abi:
    for l in open(a.abi).read().splitlines()[1:]:
        f = l.split('\t')
        if len(f) >= 2: abi[f[0]] = f[1]
rows = []
for l in open(a.calls):
    f = l.rstrip('\n').split('\t')
    name, n, hdr = f[0], int(f[1]), f[2]
    if name in glue: prov = 'glue'
    elif name in vgx: prov = 'vgx'
    elif name.startswith('VI') or name.startswith('TPL'): prov = 'sdk'
    else: prov = 'MISSING'
    rows.append((name, n, prov, hdr, NOTES.get(name, '')))
rows.append(('GXProject', 2, 'glue' if 'GXProject' in glue else 'MISSING', '(declared by the game)', NOTES['GXProject']))
rows.sort()
cnt = {}
for r in rows: cnt[r[2]] = cnt.get(r[2], 0) + 1
print('| function | call sites | provider | notes |')
print('|---|---:|---|---|')
for name, n, prov, hdr, note in rows:
    extra = ''
    if prov == 'vgx' and abi.get(name) not in (None, 'same'):
        extra = ' (ABI-identical; header types differ in name only)'
    print(f'| `{name}` | {n} | {prov} | {note}{extra} |')
sys.stderr.write('providers: ' + ', '.join(f'{k} {v}' for k, v in sorted(cnt.items())) + '\n')
