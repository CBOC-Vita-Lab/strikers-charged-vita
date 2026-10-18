#!/usr/bin/env python3
"""MSP: classify the 535 patches of yannicksuter/mscharged-port (a79182a3) against the native port np.
   Input: digest.tsv (faithful series: number, subject, +lines, -lines, files, file list) and digest-tweaks.tsv.
   Output: markdown for np/docs/wave4-MSP-catalogue.md (group tables + the per-patch table).
   The rules are title/path keywords with a manual override table for the patches that were read in full."""
import re, sys, os, collections

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'catalogue-data')
rows = []
for kind, name in (('F', 'digest.tsv'), ('T', 'digest-tweaks.tsv')):
    for line in open(f'{SRC}/{name}', errors='replace'):
        p = line.rstrip('\n').split('\t')
        if len(p) < 6: continue
        rows.append(dict(kind=kind, n=p[0], title=p[1].strip() or '(no subject; see files)', add=int(p[2]), rem=int(p[3]), nf=int(p[4]), files=p[5].split('|')))

# category codes
CAT = {
 'A': '64-bit only', 'B': 'endian+data', 'C': 'compiler+UB', 'D': 'SDK or hardware hand-over', 'E': 'renderer (Aurora)',
 'F': 'game bug', 'G': 'tweak', 'S': 'integration scaffolding'}

# groups: id -> (category, name, np status, evidence). Dynamic ones (nature-topic) are created by group_for().
G = {}
def grp(gid, cat, name, status, evidence):
    G[gid] = (cat, name, status, evidence)

NAT = {
 'A': ('64-bit widths and pointer carriers', 'not needed (ILP32)',
       'np builds are 32-bit (host32 = clang -m32, armhf and Vita = 32-bit ARM): pointers, long and size_t are 4 bytes as on the Wii, so the Wii layouts hold without carriers, 4-byte cells or LLP64 casts. platform/assets/np_layout_asserts.cpp and platform/sdk/save/np_save_game.cpp pin the sizes the converters assume; docs/vita.md section 4 lists the one layout difference found (FEInput::m_InputLockDepth, fixed).'),
 'B': ('endian and serialized data read at the byte boundary', 'already have (NPA / vgx / codecs)',
       'np converts disc structures to host order once at load (NPA, NP_DISC_HOST_ORDER; docs/assets.md) and keeps GX-consumed data (vertex arrays, display lists, texels, palettes) big-endian for vgx (docs/render.md section 4: per-stream host-order flag, NP_GX_SETARRAY_STREAM); CPU readers of GX data use npa_rdf32/npa_rd16 (DrawableModel::GetAABBDimensions, NetMeshEdge/NetMeshModelLoader = NPA patch 0009). Bit-fields: include/port/bitfield.h and the bit-field headers; save data: platform/sdk/save codec.'),
 'C': ('compiler dialect, declarations and undefined behaviour of the host toolchain', 'already have / not needed',
       'np flags (-std=gnu++11 -fno-exceptions -fno-rtti -fno-strict-aliasing -fwrapv -fshort-wchar -funsigned-char -fno-short-enums -fno-strict-return -Wno-c++11-narrowing) and include/np/compat.h (MSL/MWCC names: __abs, __fabs, strcmpi ...) absorb these; np builds with clang -m32 / ARM, the problems of a 64-bit GCC/MSVC/macOS toolchain do not arise.'),
 'E': ('Aurora GX transport (materials, pipelines, shadows, lighting, particles, text, EFB, array transport)', 'not needed (vgx)',
       'np renders through vgx (platform/gx, the vgx repository), which consumes the GX stream the game writes; Aurora-specific transports (SetGameGraphicsArray, PublishGameGraphicsNativeBytes, automatic pipeline preparation, native lifetime trackers) do not apply. Items for VRS are listed in wave4-MSP.md.'),
 'S': ('staged enabling of the original source, diagnostics and native-module plumbing', 'not needed',
       'The port compiles the decomp piece by piece behind MSCHARGED_DIAGNOSTIC_* switches ("Select / Share / Restore / Admit original ...") and re-routes services through its own runtime and allocation trackers. np compiles the whole decomp (Makefile SRCS_ALL) with no diagnostic gating, in one process with the Wii allocators on its own arenas; the code a patch "selects" is simply there.'),
}
TOP_D = {
 'audio': ('AX / DSP / audio hardware path', 'not needed (our HLE)',
           'np runs the real RVL_SDK AX code (platform/sdk/src/ax, generated patches) over a DSP HLE (dsp_hle.cpp) and its own mixer (audio_out.cpp); the port emulates the DSP microcode (dsp_instruction_core) and uses its own windowed-sinc filter. Nothing to adopt; wave4-MSP.md section 3.'),
 'input': ('Input: WPAD/KPAD pointer, desktop DPD, key and gamepad bindings', 'already have (own design)',
           'np emulates PAD/WPAD/KPAD from the Vita pad (platform/sdk/src/input_native.cpp, input/np_game_input.cpp, np_menu_nav.cpp, np_touch.cpp): right stick or a finger places the pointer (KPAD pos -1..1), touch minigames for the Mega Strike. The port feeds a calibrated virtual sensor bar (wiimote_ir.cpp KpadPointerCentre) to the real KPAD code; np writes KPAD.pos directly. Pointer extents in 4:3 versus 16:9 are on the "needs testing" list.'),
 'movie': ('Movies and credits (THP, movie writer, credits phase)', 'already have (platform/sdk/src/thp_native.cpp)',
           'np decodes THP natively with lanes (NPP2: MSC_THP_DEPTH) over the real THP player on the HLE; the port validates the stream in a bounded profile (thp_movie.cpp) and decodes through Aurora. Movies/credits are on the "needs testing" list.'),
 'hbm': ('HOME Button Menu, RFL (Mii), HOME layout', 'not needed (no HOME menu natively)',
         'np has no HOME Button Menu (platform/sdk/src/stubs_rfl_hbm.cpp); Mii data is not used by the offline modes.'),
 'sdk': ('Other SDK and hardware: DVD/FS/IPC/STM/NAND/OS/RTC/interrupts/threads/boot memory/SC', 'not needed (our SDK layer)',
         'np implements the SDK calls on the host (platform/sdk/src: os_*, dvd_native, nand_native, sc_native, vi_native, input_native; NpFs). The port adapts the original SDK sources to its own hardware devices (IOS, STM, IPC boot buffers), which np does not have.'),
}
def group_for(nat, top):
    if nat == 'D':
        top = top if top in TOP_D else 'sdk'
        gid = 'D-' + top
        if gid not in G:
            nm, st, ev = TOP_D[top]
            grp(gid, 'D', nm, st, ev)
        return gid
    gid = nat
    if gid not in G:
        nm, st, ev = NAT[nat]
        grp(gid, nat, nm, st, ev)
    return gid
# fixed groups
grp('F1', 'F', 'Native memory-safety bug found in the original code (goal clock terminator)', 'MISSING -> ported', 'OverlayHandlerGoal.cpp UpdateGoalInfo: [16] arrays, nlSNPrintf(size 32) writes buffer[size-1] (index 31, or 32 after the leading 0). np\'s wide nlSNPrintf (src/NL/nlPrint.cpp) does the same write. Ported 8511a03: arrays [33].')
grp('F2', 'F', 'Time-up / HUD clock float to unsigned (sudden death, shot live at the whistle)', 'MISSING -> ported', 'x86 wraps a negative float converted to unsigned (clang -m32: (unsigned)-3.0f = 4294967293, measured), MWCC and ARM VFP saturate to 0 (armhf under qemu-arm: 0, measured). Five time-up tests and three HUD clock conversions use port_f2u32 (d9abcbf); Variant::GetHash too (0c020c9, from the port\'s mwcc_runtime_helpers.cpp).')
grp('A2', 'A', 'Raw byte-offset reads that only break with 8-byte pointers (PhysicsEventQueue, banner captain 0x24)', 'not needed (ILP32); ported the guard',
    'Offsets 4/0x1C/0x2C/0x3C/0x48/0x62/0x65/0xA4/0xF0 (and 0x24, 0x17E-0x182) of the decomp are right in ILP32 because the layout equals the Wii\'s; a 64-bit build reads other fields there (the port crashed in Birdo\'s egg and drew wrong banners). Nothing asserted it: platform/compat/np_offset_asserts.cpp now does, for host32 and armhf (02da873, new code; the list of sites is from patches 0699 and 0703).')
grp('B2', 'B', 'Bit-field order and packed words (MWCC allocates from the MSB, clang/GCC little-endian from the LSB)', 'already have',
    'include/port/bitfield.h plus the bit-field headers RenderSnapshot.h, DrawableBall.h, XSoundHandle.h, Plat3dSoundSrc.h; Warble.cpp builds the row word with shifts (row.word |= (y & 3) << 2); the save codec reverses bit-field units (np_save_codec.cpp kBitUnits).')
grp('B3', 'B', 'Save data (Wii byte order, bit-field records, checksum words)', 'already have',
    'platform/sdk/save: np_save_codec converts the host image to the Wii image and back (NpSavePayloadToWii/FromWii, settings and online payloads, bit-field units); np_save_game.cpp pins the structure sizes with static_assert and, since 22e2192, the field offsets the codec walks (TeamStats, PlayerStats, CupProgressRecord, CupHistory, the CupManager progress span: from the port\'s save_data.cpp). The port\'s save_data.cpp transforms the same layouts (UserInfo 0x80, TeamStats 0x70, CupProgressRecord 0x37C, history/date bit-field words): equivalent. The layout table regenerated from the DEC headers (host32 and armhf) equals the committed one.')
grp('C2', 'C', 'Enum underlying types (MWCC enums are signed ints; clang gives unsigned for enums without negative values)', 'checked: no case found; see wave4-MSP.md section 8',
    'The port forces `: int` / `: s32` on seven enums. np: eCharacterClass has -1 (clang picks int); the audio/effects/challenge enums are only stored and compared for equality or used as table indices in the code that was grepped; the signedness audit (clang -Wtautological-* over the 770 sources, tools/msp/audit_warn.sh) found no comparison that an unsigned enum type makes constant: of its 15 distinct locations none is an enum (wave4-MSP.md section 8).')
grp('E2', 'E', 'Shadow volumes, shadow blend and per-character shadow colour', 'already have',
    'GXShadowVolumeMaterialProgram binds its arrays with NP_GX_SETARRAY_STREAM (per-stream byte order); the shadowLevel colour is produced and consumed as a memory cast of the same four bytes (DrawableCharacter.cpp / SkinAnimatedNPC.cpp produce, GameObjectLighting.cpp consume) - consistent on a little-endian host; the port\'s numeric ColourFromWiiWord is not needed.')
grp('G1', 'G', 'Port tweaks (gameplay and display options)', 'see per-patch status', 'The 17 tweak patches: two implemented here (pacing, Performance Mode), three original-game bug fixes (ported as an option), the rest owner-choice options.')
grp('X1', 'S', 'Debug LAN lobby, LAN series and disconnect handling of the port', 'not needed (own multiplayer)', 'np has its own LAN multiplayer (platform/sdk/net, docs/multiplayer.md; lockstep over UDP).')
grp('S3', 'S', 'Start-up facts: common.ini, datetime.ini, random seed, ODE set-up', 'already have', 'src/Game/main.cpp loads common.ini and datetime.ini (about line 502) and seeds/inits ODE as original code.')

# ---- nature (A..S) from the diff features, topic from the paths; overrides afterwards
import json, os
FEATS = json.load(open(os.path.join(SRC, 'feats.json')))
def dir_topic(path):
    p = path.lower()
    if re.search(r'/hbm/|/rfl/|rvlfacelib|/rfl_|hbm', p): return 'hbm'
    if re.search(r'/audio/|/ax/|/ax[a-z_]*\.|/dsp/|/mix/|/aux|axfx|adpcm|wenc', p): return 'audio'
    if re.search(r'/thp/|/tpl/|thp', p): return 'movie'
    if re.search(r'wpad|kpad|/pad/|/pad\.|padclamp|/wud|/wbc|/kpad|platpad|input', p): return 'input'
    if re.search(r'/nl/glx?/|/nl/gl[a-z]*\.|game/render/|game/gl/|game/effects/|game/drawable/|game/font/|nlfont|revolution/gx|/gx/', p): return 'graphics'
    if re.search(r'game/fe/|game/sh/|game/transitions/|game/overlay|feplat', p): return 'frontend'
    if 'game/camera' in p: return 'camera'
    if 'game/ai/' in p: return 'ai'
    if re.search(r'game/physics|game/world|game/sanim|stadium', p): return 'world'
    if re.search(r'game/db/|save', p): return 'save'
    if re.search(r'network|/net/|lobby|matching', p): return 'network'
    if re.search(r'rvl_sdk|revolution/|libs/msl|src/msl|libs/rvl', p): return 'sdk'
    if re.search(r'game/task|game/sys|game/main|asyncloading|game/debug', p): return 'boot'
    return 'core'

def topic(r):
    # plurality of the directory areas of the patch's files ('core' only when nothing else); the title keywords break ties and cover
    # patches whose files are all in generic places
    votes = collections.Counter(dir_topic(f) for f in r['files'])
    non = {k: v for k, v in votes.items() if k != 'core'}
    kw = topic_kw(r)
    if non:
        best = max(non.values())
        top = [k for k, v in non.items() if v == best]
        return kw if kw in top else sorted(top)[0]
    return kw

def topic_kw(r):
    f = ' '.join(r['files']); t = r['title'].lower()
    fl = f.lower()
    if 'hbm' in fl or 'hbm' in t or 'home ' in t or 'rlan' in t or 'rfl' in fl or ' mii' in t: return 'hbm'
    if re.search(r'audio|/ax|ax/|dsp|sound|wenc|reverb|adpcm', fl) or re.search(r'audio|dsp|\bax\b|adpcm', t): return 'audio'
    if re.search(r'thp|movie|credits', fl) or re.search(r'thp|movie|credits', t): return 'movie'
    if re.search(r'wpad|kpad|pad|input|pointer|dpd|wiimote|controller', fl) or re.search(r'input|pointer|controller|kpad|wpad', t): return 'input'
    if re.search(r'save|gameinfo|/db/', fl) or re.search(r'save|checksum', t): return 'save'
    if re.search(r'lan|network|netw|matching|/net/', fl) or re.search(r'lan\b|lobby|network', t): return 'network'
    if re.search(r'rvl_sdk|dolphin|nand|dvd|/os|ipc|stm|interrupt|thread|rtc|sc/|mem', fl) or re.search(r'dvd|ipc|stm\b|nand|\bos\b|rtc|interrupt|thread|sdk|\bmem\b|retrace', t): return 'sdk'
    if re.search(r'nl/gl|nl/glx|render|drawable|effects|material|shadow|light|font|particle|emission|rlg|texture|gl/', fl) or re.search(r'gx|material|pipeline|shadow|lighting|texture|graphics|render|efb|mesh|vertex|particle|specular|font|text|quad|skin|effects|emission', t): return 'graphics'
    if re.search(r'/fe/|/sh/|frontend|menu|overlay|presentation|title|nav', fl) or re.search(r'frontend|menu|options|title|nav|popup', t): return 'frontend'
    if re.search(r'camera', fl) or 'camera' in t: return 'camera'
    if re.search(r'/ai/|desire|goalie|fielder', fl): return 'ai'
    if re.search(r'loading|async|boot|main', fl) or re.search(r'loading|boot|main', t): return 'boot'
    if re.search(r'world|stadium|replay|physics|ode', fl): return 'world'
    return 'core'

def nature(r):
    t = r['title'].lower(); f = FEATS.get(r['n'], {})
    bs, pt, dg, gx, au, sd, cp = (f.get(k, 0) for k in ('bswap', 'ptr', 'diag', 'gx', 'audio', 'sdk', 'compiler'))
    staged = bool(re.search(r'\badmit|diagnostic|select(ed)? (the )?(original|native|temporary|checked|bounded|portable)|\bselect\b|\bshare(d)? (the )?(original|canonical|native)|retain legacy|restore (the )?(whole|complete|original)|^restore|\bscope\b|named |retire', t))
    if re.search(r'hardware|mailbox|\\bdsp\\b|\\bstm\\b|\\bipc\\b|\\bnand\\b|\\bdvd\\b|interrupt|\\bcsr\\b|power removal|\\bios\\b|\\brtc\\b|reset task|\\bsc\\b', t) and not staged: return 'D'
    if bs >= 3 and bs >= pt: return 'B'
    if pt >= 3 and pt > bs: return 'A'
    if re.search(r'endian|byte order|wii32|wii16|\bbe16|serialized|decode|wire|word', t) and not staged: return 'B'
    if re.search(r'pointer|llp64|lp64|size_t|width|carrier|alignment|stride|handle|address|key|context|four-byte|32-bit|cells', t) and not staged: return 'A'
    if staged or dg > 0: return 'S'
    if bs >= 1: return 'B'
    if pt >= 1: return 'A'
    if gx >= 1 or topic(r) == 'graphics': return 'E'
    if au >= 1 or sd >= 1 or topic(r) in ('audio', 'sdk', 'input', 'movie', 'hbm'): return 'D'
    if cp >= 1: return 'C'
    return 'S'

def classify(r):
    if r['kind'] == 'T': return 'G1'
    nat = nature(r); top = topic(r)
    if re.search(r'lan\\b|lobby|debug lan', r['title'].lower()): return 'X1'
    return group_for(nat, top)

OVER0 = {  # patch -> group (read in full or titled unambiguously)
 '0647': 'F1', '0700': 'F2', '0702': 'F2',
 '0699': 'A2', '0703': 'A2', '0332': 'A2',
 '0300': 'B2', '0444': 'B2', '0619': 'B2', '0586': 'B2', '0511': 'C2', '0681': 'C2', '0682': 'C2', '0683': 'C2', '0684': 'C2', '0512': 'C1',
 '0601': 'E2', '0617': 'B1', '0386': 'B1', '0704': 'B1', '0434': 'B1', '0360': 'B1', '0371': 'B1', '0447': 'B1',
 '0686': 'S3', '0687': 'S3',
 '0690': 'A1', '0701': 'A1', '0698': 'A1', '0704': 'B1', '0714': 'E1', '0618': 'E1',
 '0053': 'C1', '0204': 'C1', '0243': 'S1', '0402': 'C1', '0608': 'C1', '0609': 'D2', '0504': 'S1', '0498': 'S1', '0081': 'D2',
 '0300': 'B2', '0369': 'B3', '0303': 'B3', '0289': 'B3', '0376': 'D2', '0113': 'B3', '0290': 'B3',
 '0555': 'S1',
}
SPECIFIC = {  # per-patch note (what exactly np has / did)
 '0647': 'ported 8511a03 (arrays [33])',
 '0700': 'ported d9abcbf (port_f2u32 at Game::Update, StatsTracker x2, Presentation x2)',
 '0702': 'ported d9abcbf (HUDClock::Update x3)',
 '0699': 'offsets right in ILP32; compile-time guard ported (02da873, np_offset_asserts.cpp)',
 '0703': 'offset 0x24 right in ILP32 (cCharacter::m_DetChar.m_eCharacterClass); guard ported (02da873)',
 '0511': 'enum underlying type: np builds -fno-short-enums; eCharacterClass has -1 so clang already picks int; see section "enum signedness"',
 '0681': 'enum underlying type, see section "enum signedness"', '0682': 'typed u32 members: np keeps unsigned long = 4 bytes (ILP32)',
 '0683': 'enum underlying type, see section "enum signedness"', '0684': 'enum underlying type, see section "enum signedness"',
 '0300': 'Warble.cpp builds the word with shifts (row.word |= (y & 3) << 2)',
 '0444': 'RenderSnapshot.h / DrawableBall.h bit-field headers (include/port/bitfield.h)',
 '0619': 'XSoundHandle.h / Plat3dSoundSrc.h bit-field headers',
 '0617': 'NetMesh* = NPA patch 0009; character physics elements = NPA conversion; shadowLevel colour: not needed (see E2)',
 '0601': 'GXShadowVolumeMaterialProgram.cpp: NP_GX_SETARRAY_STREAM on all three arrays',
 '0386': 'LoadMatrices: NPA converts the matrix floats / vgx reads big-endian matrices (docs/render.md section 4)',
 '0704': 'DrawableModel::GetAABBDimensions reads the stream with npa_rdf32 (issue #18 does not exist natively)',
 '0714': 'VRS: vgx must release a destroyed target\'s EFB copy textures (lifetime of copied textures)',
 '0618': 'VRS: rewound GL resource-pool storage must stay valid until the pool allocates over it (second DrawableNetMesh pass per frame): check vgx\'s buffer/draw lifetime',
 '0686': 'main.cpp ~502 loads common.ini and datetime.ini', '0687': 'main.cpp seeds and sets up ODE',
 '0053': 'np uses signed int noise arithmetic: defined with -fwrapv; Update zeroes mDisplacement first',
 '0204': 'compat.h provides __fabs; same value',
 '0243': 'diagnostic-only extraction; np uses the original direct C_MTX44Inverse path',
 '0402': 'compat.h __abs',
 '0608': 'clang accepts the narrowing with -Wno-c++11-narrowing; same values',
 '0690': 'long is 32 bits natively: the test is already on 32-bit codes', '0701': 'pointer member is 4 bytes',
 '0698': 'event word 32 bits natively', '0498': 'diagnostic gating only', '0504': 'diagnostic gating only',
 '0081': 'port-side failure drain of its own whole-file layer; np\'s nlFileGC paths run on its DVD HLE (NPS)',
 '0555': 'main already calls EnableAllStadiums as original code',
}
TWEAK = {
 '0001': ('n/a', 'their hook ABI (mscharged_tweaks.h); np uses np/framepace.h and np/perfmode.h'),
 '0002': ('not ported', 'option: unlock everything (useful for testing the modes below; owner decision)'),
 '0003': ('not ported', 'option: no Mega Strikes'),
 '0004': ('not ported', 'option: skip the intro (np nav scripts press A; no need)'),
 '0005': ('not ported', 'option: fast menus'),
 '0006': ('MISSING -> ported as option', 'goal net redraw (issue #19): 8284f0a, MSC_GAME_FIXES=net'),
 '0008': ('not ported', 'option: CPU vs CPU'),
 '0009': ('MISSING -> ported as option', 'electrocution texture (issue #33): 2594b34, MSC_GAME_FIXES=electro'),
 '0010': ('MISSING -> ported as option', 'NK bug (issue #38): 35873c2, MSC_GAME_FIXES=nk'),
 '0011': ('not ported', 'boot loading message'),
 '0012': ('not ported', 'option: fast terrain'),
 '0013': ('ported (below the original rate)', 'np/framepace.h, 4dd60f3; theirs raises the rate above 60'),
 '0014': ('ported (below the original rate)', 'np/framepace.h hook sites, 4dd60f3 / c8b0f78; the FE impostor flicker needs no hook below the original rate'),
 '0015': ('not ported', 'option: mirror match'),
 '0016': ('not ported', 'option: Thunder Island walls'),
 '0017': ('not ported', 'option: blue Peach'),
 '0018': ('ported, default off', 'np/perfmode.h, 626e50e, MSC_PERFORMANCE_MODE=1'),
}
MAPOV = {'C1': ('C', 'sdk'), 'B1': ('B', 'core'), 'E1': ('E', 'graphics'), 'D2': ('D', 'sdk'), 'A1': ('A', 'core'), 'S1': ('S', 'core')}
OVER = {}
for k, v in OVER0.items():
    if v in MAPOV: nat, top = MAPOV[v]; OVER[k] = (lambda nat=nat, top=top: group_for(nat, top))
    else: OVER[k] = v
out_rows = []
for r in rows:
    n = r['n']; k = r['kind']
    gid = OVER.get(n) if k == 'F' else None
    if callable(gid): gid = gid()
    if gid is None: gid = classify(r)
    r['gid'] = gid
    cat, gname, status, ev = G[gid]
    if k == 'T':
        status, note = TWEAK[n]; cat = 'G'
    else:
        note = SPECIFIC.get(n, '')
        if gid in ('F1', 'F2'): cat = 'F'
    r['cat'] = cat; r['status'] = status; r['note'] = note; r['topic'] = 'tweak' if k == 'T' else topic(r)
    out_rows.append(r)

HEADER = """# Wave 4 MSP: the 535 patches of yannicksuter/mscharged-port against np

Agent MSP, 16 Oct 2026. Source: yannicksuter/mscharged-port, origin/main a79182a3 (licence CC0), read with `git show origin/main:<path>` in a local clone; np main 1722d1e
(the branch MSP, rebased onto DEC d31eae0). The summary and the decisions are in `wave4-MSP.md`; this file is the table.

**Counts.** The brief says 520 + 19; the repository has **518** patches in `patches/mscharged-decomp` (numbered 0001 to 0703, with gaps) and **17** in
`patches/mscharged-decomp-tweaks` (0001 to 0018, with gaps): 535. The tweak series is prefixed `T` below.

**What the port is.** The decomp (the same one np compiles) made to run natively on 64-bit desktops: a staged enabling of the original source behind diagnostic switches,
widening fixes for 8-byte pointers, byte-order fixes at the data boundary, an Aurora (GX) renderer, SDK and DSP emulation, and at the end the first tweaks. Most of it solves
problems np does not have because np is 32-bit and reuses the original SDK code on a host layer (`docs/vita.md`, `docs/assets.md`, `docs/render.md`).

**How a patch was classified.** `catalogue-data/digest.tsv` and `digest-tweaks.tsv` hold, per patch, the subject (for the 42 patches without a `Subject:` the first comment line of
the patch file), added and removed lines, and the files touched, taken from the patch files; `feats.json` has counts of features found in the added lines (byte-swap helpers,
pointer-width casts, GX calls, audio and SDK calls, diagnostic macros). `tools/msp/catalogue.py` assigns a *nature* (the group letter) from those counts and from title keywords,
a *topic* from the directories of the files (the plurality; title keywords break ties), and then applies a table of **47 faithful patches and all 17 tweaks that were read by hand**
(the diff or the message; the table is `OVER0`, `SPECIFIC` and `TWEAK` in the script). The np status of a patch is the status of its group, with the evidence in the group
paragraph, unless the last column says otherwise.

**What this table is not.** The group of the 471 patches that were not read by hand comes from keywords, so a few are in a neighbouring group (for example a width fix filed as
scaffolding); the np status is the same for all the groups that "np does not need", and none of the groups I checked had a status that would change. The topic column is a hint
(directories), not a claim. The part that matters is the **MISSING** column. To find behaviour fixes I read the title of every patch and listed, for the 518 faithful patches, the
changed lines that are outside the native-only guards (those are the lines that also change the Wii build, where a fix of the original would be); every patch that looked like a fix
was read in full. This is a scan, not a proof: a fix hidden in a patch titled like scaffolding could have been missed, and the unread patches are listed with their group only.

**np status vocabulary.** *already have*: np does the same thing, the evidence names the file or commit. *not needed*: the problem does not exist in np (32-bit build, own SDK layer,
vgx instead of Aurora) with the reason. *MISSING -> ported*: np had the defect or lacked the feature, a commit on this branch fixes it. *ported / ported, default off*: a tweak that
np implements as its own code (the tweak ABI of the port is not used). *not ported*: a tweak option that is no bug fix and no wave-4 task; listed so the owner can choose.

**Verification.** The statuses were read from code, not run, except where the evidence says "measured" (`wave4-MSP.md` has the commands and what each number is). Nothing here ran on a console or in Vita3K.

"""

def md():
    L = []
    cnt = collections.Counter((r['gid']) for r in out_rows)
    L.append('## Counts: group by topic\n')
    tops = sorted(set(r['topic'] for r in out_rows))
    L.append('| group | ' + ' | '.join(tops) + ' | all |')
    L.append('|---|' + '---:|' * (len(tops) + 1))
    for gid in sorted(G):
        c = collections.Counter(r['topic'] for r in out_rows if r['gid'] == gid)
        if not sum(c.values()): continue
        L.append(f'| {gid} | ' + ' | '.join(str(c[t]) if c[t] else '' for t in tops) + f' | {sum(c.values())} |')
    L.append('')
    L.append('## Groups\n')
    L.append('| group | category | what | patches | np status |')
    L.append('|---|---|---|---:|---|')
    for gid in sorted(G):
        cat, name, status, ev = G[gid]
        L.append(f'| {gid} | {CAT[cat]} | {name} | {cnt[gid]} | {status} |')
    L.append('')
    for gid in sorted(G):
        cat, name, status, ev = G[gid]
        L.append(f'**{gid} {name}** ({CAT[cat]}; {cnt[gid]} patches; np: {status}). {ev}\n')
    L.append('## Every patch\n')
    L.append('Faithful series `patches/mscharged-decomp` (518) numbered as in the repository, tweak series `patches/mscharged-decomp-tweaks` (17) prefixed `T`. Lines are `+added/-removed`. The status of a patch is its group\'s unless the last column says otherwise.\n')
    L.append('| patch | title | +/- | files | topic | group | np |')
    L.append('|---|---|---|---:|---|---|---|')
    for r in out_rows:
        pre = 'T' if r['kind'] == 'T' else ''
        t = r['title'].replace('|', '/')
        t = re.sub(r'^\[PATCH\]\s*', '', t)[:110]
        note = r['note'].replace('|', '/')
        st = r['status'] if (r['kind'] == 'T' or r['gid'] in ('F1', 'F2')) else ''
        np_col = (st + (': ' if st and note else '') + note) if (st or note) else 'group'
        L.append(f"| {pre}{r['n']} | {t} | +{r['add']}/-{r['rem']} | {r['nf']} | {r['topic']} | {r['gid']} | {np_col} |")
    return '\n'.join(L)

if __name__ == '__main__':
    print(HEADER + md())
    cnt = collections.Counter(r['gid'] for r in out_rows)
    print('\n<!-- counts', dict(cnt), len(out_rows), '-->', file=sys.stderr)
