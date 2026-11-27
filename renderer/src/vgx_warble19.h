// vgx_warble19.h: Lava Pit / Sand Tomb heat shimmer with fewer scenes (w8 warble19). Not a stand-alone header: it is included in sections, in the middle of
// vgx_draw.cpp (DRAW_A before MarkDeadCopies, DRAW_B before IsolatePasses) and of vgx_backend_gl.cpp (BACKEND before EfbSparesTrim), where the names it uses are in scope.
// Switches (code default 0): MSC_VGX_CLEARVEH, MSC_VGX_EFBSWAP_NEXT, MSC_VGX_PASSISO_COVER (exact: pictures and det trace identical on the host), MSC_WARBLE_TIGHT, MSC_WARBLE_LITE.
#ifdef W19_SECTION_DRAW_A
// MSC_VGX_CLEARVEH=0|1 (1.3 w8 sandtomb; code default 0): the game clears a target (GLXTarget::ClearBuffers: the warble's offset map starts from grey, and so do other targets)
// by copying the whole EFB into a scratch array (clearz_mem, 320 x 224, GX_CTF_R8) with the clear flag set. Nothing ever reads that array (it has no other use in the game's
// source), so the copy is only its clear. The first ones of a frame were already found dead by MarkDeadCopies (a later copy into the same texture before any draw samples it);
// the last one of a frame never has a later copy in the frame, so it stayed a real copy: a full-screen copy quad (a scene of its own) in the middle of the EFB's scene.
static void MarkClearVehicles() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_CLEARVEH", 0) != 0));
    if (!on) return;
    for (const Cmd& c : g_buf.cmds) {
        if (c.type != CMD_COPY_TEX) continue;
        CopyTexCmd& t = g_buf.copyTex[c.arg];
        if (t.skip || t.fmt != 0x28 || t.depth || t.fromPass || t.srcTex || t.dstTexW || t.still || !t.clear.enable || t.dstW != 320 || t.dstH != 224) continue;
        t.skip = true;
        static const bool brk = EnvInt("MSC_VGX_CLEARVEH_BREAK", 0) != 0;   // test only: its clear is dropped too (the check must see the warble's offset map go wrong)
        if (brk) t.clear.enable = false;
        ++stats::Cur().deadCopies;
    }
}


// MSC_WARBLE_LITE=<bits> (1.3 w8 warble19; code default 0; owner 15 Nov: a cheaper heat shimmer of the same kind). The shimmer is the same everywhere: the finished picture is copied (or handed
// over), the blobs are drawn into the grey-cleared corner (layer 31: the offset map), the corner is copied 2:1 into a 160x112 texture, the composite (layer 32) reads picture and map.
// Bit 1: the offset map is made every second picture while the game runs at 60 (NpFpsPolicy 2; MSC_WARBLE_LITE_EVERY=n forces a period for the host): the blob draw and the map copy
// are left out of the other pictures and the composite reads the map of the picture before; the pattern moves at the 30 a second it has at 30. Bit 2: the offset map is drawn at
// 160x112 straight into its texture (DownPasses; needs MSC_VGX_PASSISO_COVER and MSC_VGX_PASSDOWN): no 320x224 pass, no copy, one scene less. Players, ball and HUD are not
// touched by either: only what the map holds changes (pictures differ inside the blobs' displaced picture; see w8/warble19.md).
extern "C" int NpFpsPolicy(void) __attribute__((weak));
constexpr uint8_t kLayerWarbleBlobs = 31;
static uint32_t g_w19MapTex = 0;
static uint64_t g_w19Seen = 0, g_w19Skipped = 0;
static long Warble19Bits() {
    static const long v = static_cast<long>(VGX_KNOB(EnvInt("MSC_WARBLE_LITE", 0)));
    return v;
}
static int Warble19Period() {
    static const long every = EnvInt("MSC_WARBLE_LITE_EVERY", 0);
    if (every > 1) return static_cast<int>(every);
    return NpFpsPolicy && NpFpsPolicy() == 2 ? 2 : 1;
}
static void WarbleLite() {
    const long bits = Warble19Bits();
    if (!(bits & 1)) return;
    std::vector<Cmd>& cmds = g_buf.cmds;
    static const long why = EnvInt("MSC_WARBLE_LITE_WHY", 0);   // (diagnostic: from this frame on, what the search found)
    for (size_t i = 0; i + 1 < cmds.size(); ++i) {
        // (the map's copy is found by its shape, not by the draw's layer mark: RGBA4444 (fmt 6), 320x224 of the EFB into 160x112; the one draw before it is the blobs)
        if (cmds[i].type != CMD_DRAW || cmds[i + 1].type != CMD_COPY_TEX) continue;
        CopyTexCmd& t = g_buf.copyTex[cmds[i + 1].arg];
        if (t.skip || t.depth || t.fromPass || t.srcTex || t.dstTexW || t.fmt != 0x06 || t.dstW != 160 || t.dstH != 112 || t.src.w != 320 || t.src.h != 224 || t.src.x != 0 || t.src.y != 0 || !t.texId) continue;
        if (why > 0 && static_cast<long>(g_frame) >= why) { static int m = 0; if (m++ < 4) std::fprintf(stderr, "[warble19] f%llu map copy at command %d: blob draw layer %d, tex %u\n", static_cast<unsigned long long>(g_frame), static_cast<int>(i), static_cast<int>(g_buf.draws[cmds[i].arg].layer), static_cast<unsigned>(t.texId)); }
        ++g_w19Seen;
        const int period = Warble19Period();
        const bool made = g_w19MapTex == t.texId;   // (the map texture has its first picture)
        static const bool brk = EnvInt("MSC_WARBLE_LITE_BREAK", 0) != 0;   // test only: the map is never made again after its first picture (the picture check must see the shimmer freeze)
        if ((period > 1 || brk) && made && (brk || g_w19Seen % static_cast<uint64_t>(period) != 0)) {
            t.skip = true;   // (the texture keeps the map of the picture before)
            cmds.erase(cmds.begin() + static_cast<std::ptrdiff_t>(i));
            ++g_w19Skipped;
            if (g_w19Skipped == 1 || g_w19Skipped % 600 == 0) std::fprintf(stderr, "[warble19] MSC_WARBLE_LITE: %llu offset maps of %llu left to the picture before (period %d)\n", static_cast<unsigned long long>(g_w19Skipped), static_cast<unsigned long long>(g_w19Seen), period);
        } else g_w19MapTex = t.texId;
        return;
    }
}
#endif
#ifdef W19_SECTION_DRAW_B
// MSC_VGX_PASSISO_COVER=0|1 (1.3 w8 sandtomb; code default 0): a second chance for LeftoversUnseen. The warble's composite (layer eCLV_WarbleBlend = 32; Warble.cpp RenderWarbleQuad draws one
// quad from (sWarbleLeft, sWarbleTop) = (0, 0) over the orthographic width and height, opaque, no alpha test) overwrites every pixel of the picture, so what the offset-map pass left in
// the EFB corner is never read: the pass can be drawn in a target of its own. Proof used here, per later draw that overlaps the rectangle: it does not read a plane that still holds
// leftovers (DrawReads); a draw that is on that layer, a quad (at most 6 indices), blend off, colour written, no depth test, not culled away (fixed winding), whose scissor holds the rectangle, and whose fragment
// shader text has no discard, overwrites the planes it writes; the display copy reads colour only and its clear wipes the rest. Anything else ends the search (not proved).
constexpr uint8_t kLayerWarbleBlend = 32;
// What a draw needs from the planes it finds in the EFB, for this proof only (DrawReads is wider: any blend counts as reading alpha). Colour: a blend that writes colour. Alpha: a blend
// whose factors name the destination's alpha, or one that writes alpha through a blend that keeps the destination (destination factor other than zero). Depth: as DrawReads.
inline uint8_t DrawReadsExact(const RenderState& rs) {
    uint8_t m = 0;
    if (rs.depthTest && rs.depthFunc != CMP_ALWAYS) m |= CH_Z;
    if (rs.blend) {
        if (rs.colorWrite) m |= CH_C;
        const bool dstA = rs.blendSrc == BF_DST_ALPHA || rs.blendSrc == BF_INV_DST_ALPHA || rs.blendDst == BF_DST_ALPHA || rs.blendDst == BF_INV_DST_ALPHA;
        if (dstA || ((rs.alphaWrite || rs.dstAlphaPass) && rs.blendDst != BF_ZERO)) m |= CH_A;
    }
    return m;
}
uint64_t g_coverIso = 0;
bool CoverNote() {   // (the visible action counter of MSC_VGX_PASSISO_COVER: a line at the first use and every 200th)
    if (++g_coverIso == 1 || g_coverIso % 200 == 0) std::fprintf(stderr, "[passiso] MSC_VGX_PASSISO_COVER: %llu passes isolated on the proof that a later opaque warble quad overwrites what they leave\n", static_cast<unsigned long long>(g_coverIso));
    return true;
}
bool CoverDraw(const DrawRec& d, const EfbRect& r) {
    const RenderState& rs = d.rs;
    if (d.layer != kLayerWarbleBlend || d.prim != PRIM_TRIANGLES || d.idxCount > 6 || rs.blend || !rs.colorWrite) return false;
    if ((rs.depthTest && rs.depthFunc != CMP_ALWAYS) || rs.cull == CULL_ALL) return false;   // (the quad's winding is fixed by Warble.cpp: it is drawn with front culling and the game's picture is the composite, so both triangles pass)
    if (!Inside(r, EfbRect{rs.scX, rs.scY, rs.scW, rs.scH})) return false;
    if (d.fsId >= g_fsReg.sources.size() || g_fsReg.sources[d.fsId]->text.find("discard") != std::string::npos) return false;
    return true;
}
static bool CoverWhy(const char* why, size_t j, int a = 0) {   // (MSC_VGX_PASSISO_COVER_WHY=1: why the proof stopped, the first 8 times)
    static int n = 0;
    static const long on = EnvInt("MSC_VGX_PASSISO_COVER_WHY", 0);   // the frame from which the reasons are printed (0: never)
    if (on > 0 && static_cast<long>(g_frame) >= on && n++ < 24) std::fprintf(stderr, "[passiso] cover proof refused at command %llu: %s %d\n", static_cast<unsigned long long>(j), why, a);
    return false;
}
bool LeftoversCovered(size_t from, const EfbRect& r, uint8_t written) {
    static const bool brk = EnvInt("MSC_VGX_PASSISO_COVER_BREAK", 0) != 0;   // test only: the proof says yes whatever follows
    if (brk) return true;
    uint8_t live = written;
    for (size_t j = from + 1; j < g_buf.cmds.size() && live; ++j) {
        const Cmd& c = g_buf.cmds[j];
        if (c.type == CMD_DRAW) {
            const DrawRec& d = g_buf.draws[c.arg];
            if (!Overlap(EfbRect{d.rs.scX, d.rs.scY, d.rs.scW, d.rs.scH}, r)) continue;
            if (DrawReadsExact(d.rs) & live) return CoverWhy("a draw reads leftovers, layer", j, d.layer);
            if (CoverDraw(d, r)) live = static_cast<uint8_t>(live & ~DrawWrites(d.rs));
            else if (d.layer == kLayerWarbleBlend) CoverWhy("warble draw is not a cover: prim|idx|blend|cw|dt|cull|scX|scY|scW|scH|fs", j, static_cast<int>(d.prim * 1000000 + d.idxCount)), CoverWhy("  ", j, d.rs.blend * 100000 + d.rs.colorWrite * 10000 + d.rs.depthTest * 1000 + d.rs.cull * 100 + d.rs.depthFunc), CoverWhy("  scissor", j, d.rs.scX * 1000000 + d.rs.scY * 10000 + d.rs.scW * 10 + (d.rs.scH == 448)), CoverWhy("  fs has discard", j, g_fsReg.sources[d.fsId]->text.find("discard") != std::string::npos);
        } else if (c.type == CMD_COPY_TEX) {
            const CopyTexCmd& t = g_buf.copyTex[c.arg];
            if (!Overlap(t.src, r)) continue;
            if (!t.skip && (t.depth ? CH_Z : CopyReads(t)) & live) return CoverWhy("a copy reads leftovers", j);
            if (t.clear.enable && Inside(r, t.src)) live = static_cast<uint8_t>(live & ~ClearMask(t.clear));
        } else if (c.type == CMD_COPY_DISP) {
            const CopyDispCmd& t = g_buf.copyDisp[c.arg];
            if (!Overlap(t.src, r)) continue;
            if (live & CH_C) return CoverWhy("display copy sees colour, live", j, live);
            if (t.clear.enable && Inside(r, t.src)) live = static_cast<uint8_t>(live & ~ClearMask(t.clear));
            break;
        } else if (c.type == CMD_SET_TARGET) {
            return CoverWhy("target switch", j);
        }
    }
    return live == 0 ? true : CoverWhy("end with live planes", g_buf.cmds.size(), live);
}


// MSC_WARBLE_LITE bit 2: the offset map's one draw (layer 31) of a pass whose copy is 2:1 is drawn at half scale into the map's texture (as a blur step): the four-pixel mean of the copy becomes one read.
static bool Warble19PassDown(const std::vector<Cmd>& cmds, size_t i, size_t j, const CopyTexCmd& t) {
    if (!(Warble19Bits() & 2) || j != i + 2) return false;
    const DrawRec& d = g_buf.draws[cmds[i + 1].arg];
    return d.layer == kLayerWarbleBlobs && d.texMask && d.tex[0].texId != t.texId;
}
#endif
#ifdef W19_SECTION_BACKEND
// MSC_VGX_EFBSWAP_NEXT=0|1 (1.3 w8 sandtomb; code default 0): a whole-EFB 1:1 copy with no clear of its own, directly followed by a copy that is only a clear (skip) of every colour and
// alpha pixel of the target, is a swap too: the colour buffer becomes the copy's texture, the EFB goes on with a spare, and the next command's clear gives that every pixel. Sand Tomb's
// warble: the copy of the finished picture (target/warblecolour, RGB565) is followed at once by the clear of the offset map (MSC_VGX_CLEARVEH makes that copy a skip). The plain swap (a copy with a clear of its own) stays behind MSC_VGX_EFBSWAP.
std::vector<const CopyTexCmd*> g_swapNext;   // the copies of this buffer that qualify (EfbSwapScan)
bool EfbSwapNextOn() {
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_EFBSWAP_NEXT", 0) != 0));
    return on;
}
// True when the draw reads texture map m in no way that depends on its alpha: no indirect stage or Z texture on the map, in every stage that uses it the texture's colour swap is the
// identity and neither a colour nor an alpha input of the TEV is TEXA (then a texture that has the right colour and any alpha gives the same picture).
bool SwapColourOnly(const FsKey& fk, int m, bool alphaDead) {
    if (fk.ztex) return false;
    for (int k = 0; k < fk.numIndStages && k < 4; ++k)
        if (fk.ind[k].texMap == m) return false;
    bool uses = false;
    for (int s = 0; s < fk.numStages && s < 16; ++s) {
        const FsKey::Stage& st = fk.stage[s];
        if (!st.texEnable || st.texMap != m) continue;
        uses = true;   // (a stage with an indirect offset reads the map at displaced coordinates: still its colour only)
        if ((st.texSwap & 3) != 0 || ((st.texSwap >> 2) & 3) != 1 || ((st.texSwap >> 4) & 3) != 2) return false;
        for (int k = 0; k < 4; ++k)
            if (((st.color >> (4 * k)) & 15) == 9) return false;   // (TEXA as a colour input)
        if (!alphaDead)   // (alphaDead: the draw writes no alpha, has no blend and no alpha test, so what its alpha would be is never seen)
            for (int k = 0; k < 4; ++k)
                if (((st.alpha >> (13 - 3 * k)) & 7) == 4) return false;   // (TEXA as an alpha input)
    }
    return uses;
}
// Does every draw of the buffer that samples texture id read it colour-only?
bool SwapTexColourOnly(const CmdBuf& buf, uint32_t id) {
    bool any = false;
    for (const Cmd& c : buf.cmds) {
        if (c.type != CMD_DRAW) continue;
        const DrawRec& d = buf.draws[c.arg];
        for (int m = 0; m < 8; ++m) {
            if (!(d.texMask & (1u << m)) || d.tex[m].texId != id) continue;
            const ShaderSource* fs = d.fsId < g_fsSrc.size() ? g_fsSrc[d.fsId] : nullptr;
            if (!fs || !fs->fkey) return false;
            const bool alphaDead = !d.rs.blend && !d.rs.alphaWrite && !d.rs.dstAlphaPass && !fs->cAlphaTest;
            if (!SwapColourOnly(*fs->fkey, m, alphaDead)) return false;
            any = true;
        }
    }
    return any;
}
std::vector<const CopyTexCmd*> g_swapNextAlpha;   // of those, the copies whose alpha is a constant (RGB565: the copy matrix gives alpha 1)
void EfbSwapScan(const CmdBuf& buf) {
    g_swapNext.clear();
    g_swapNextAlpha.clear();
    if (!EfbSwapNextOn()) return;
    const size_t n = buf.cmds.size();
    for (size_t i = 0; i + 1 < n; ++i) {
        if (buf.cmds[i].type != CMD_COPY_TEX || buf.cmds[i + 1].type != CMD_COPY_TEX) continue;
        const CopyTexCmd& a = buf.copyTex[buf.cmds[i].arg];
        const CopyTexCmd& b = buf.copyTex[buf.cmds[i + 1].arg];
        if (a.clear.enable || !b.skip || b.fromPass || b.srcTex || b.dstTexW || !b.clear.enable || !b.clear.color || !b.clear.alpha) continue;
        if (b.src.x != 0 || b.src.y != 0 || b.src.w != g_efbW || b.src.h != g_efbH) continue;
        g_swapNext.push_back(&a);
        // colour identity, alpha row free: allowed when no draw reads the copy's alpha
        bool colourId = a.add[0] == 0 && a.add[1] == 0 && a.add[2] == 0;
        for (int r = 0; r < 3 && colourId; ++r)
            for (int col = 0; col < 4; ++col)
                if (a.mat[r][col] != (r == col ? 1.0f : 0.0f)) colourId = false;
        if (colourId && SwapTexColourOnly(buf, a.texId)) g_swapNextAlpha.push_back(&a);
    }
}
#endif
