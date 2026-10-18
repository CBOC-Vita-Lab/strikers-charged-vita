// Register file, command-stream parser and vertex decoding (front end).
#include "vgx_state.h"
#include "vgx_ext.h"

#include <cmath>

namespace vgx {

uint64_t g_coPlanarOn = 0; // times GX Z-freeze was switched on (genMode bit 19; MSC_VGX_ZFTRACE)

State g;

uint32_t g_gxd = 0x3FFFFFFFu; // (GXD_VERIFY, bit 30, is a diagnostic: off unless asked for)
void GxdInit() { g_gxd = static_cast<uint32_t>(EnvInt("MSC_VGX_GXD", 0x3FFFFFFF)); }

namespace {
inline int sext11(uint32_t v) { return static_cast<int>(v << 21) >> 21; }

// GX attribute numbering (GXAttr)
enum { A_PNMTXIDX = 0, A_TEX0MTXIDX = 1, A_POS = 9, A_NRM = 10, A_CLR0 = 11, A_CLR1 = 12, A_TEX0 = 13, A_TEX7 = 20,
       A_POSMTXARRAY = 21, A_NBT = 25 };
enum { T_NONE = 0, T_DIRECT = 1, T_INDEX8 = 2, T_INDEX16 = 3 };
enum { C_U8 = 0, C_S8 = 1, C_U16 = 2, C_S16 = 3, C_F32 = 4 };
} // namespace

// Which part of the resolved draw state a changed register invalidates. Most draws change
// only values (TEV colours, a matrix index, material colours); rebuilding and comparing the
// shader keys and the render state for each of those was a third of the player's time.
// Which part of a draw's resolved state a BP register feeds (ResolveState in vgx_draw.cpp;
// the lists are exactly the g.bp[] reads of BuildFsKey / BuildRenderState / BuildTev there:
// keep them in step). A register that none of them reads marks nothing: the texture-coordinate
// scale registers 0x30-0x3F, which change with every texture, used to cost a rebuild of
// both shader keys (145 of 202 rebuilds per open-play frame changed nothing).
static inline void MarkBP(uint8_t reg) {
    // Fragment key (and with it render state and TEV block): genMode, indirect stages and
    // their order, TEV order, blend mode, destination alpha, pixel format, TEV stages, fog
    // type, alpha test, Z-texture mode, swap tables / konst selection.
    if (reg == 0x00 || (reg >= 0x10 && reg <= 0x1F) || (reg >= 0x27 && reg <= 0x2F) || (reg >= 0x41 && reg <= 0x43) || (reg >= 0xC0 && reg <= 0xDF) ||
        reg == 0xF1 || reg == 0xF3 || (reg >= 0xF5 && reg <= 0xFD)) {
        g.bpKeyDirty = true;
        g.bpDirty = true;
        return;
    }
    // Render state only: scissor and its offset, line width, depth mode.
    if (reg == 0x20 || reg == 0x21 || reg == 0x22 || reg == 0x40 || reg == 0x59) {
        g.bpRsDirty = true;
        return;
    }
    // Values of the TEV uniform block only: TEV / konst colours, fog parameters and colour,
    // Z-texture bias, indirect matrices and scales.
    if ((reg >= 0xE0 && reg <= 0xE7) || reg == 0xEE || reg == 0xEF || reg == 0xF0 || reg == 0xF2 || reg == 0xF4 || (reg >= 0x06 && reg <= 0x0E) ||
        reg == 0x25 || reg == 0x26)
        g.bpDirty = true;
    // (everything else: copy set-up 0x49-0x54, read when a copy is made; texture and TLUT
    // registers, bound through GXLoadTexObj; coordinate scales; performance counters)
}
static inline void MarkXFReg(uint32_t r) { // r = address - 0x1000
    if (r == 0x18 || r == 0x19) return;                   // matrix indices: xfMtxDirty (set by the caller)
    if (r >= 0x1A && r <= 0x1F) g.xfViewDirty = true;
    else if (r >= 0x20 && r <= 0x26) g.xfProjDirty = true;
    else if (r >= 0x0A && r <= 0x0D) g.xfColorDirty = true;
    else if (r == 0x09 || (r >= 0x0E && r <= 0x12) || r == 0x3F || (r >= 0x40 && r <= 0x4F)) g.xfKeyDirty = true;
    else if (r >= 0x50 && r <= 0x57) { g.xfKeyDirty = true; g.xfMtxDirty = true; } // post-transform: key and matrix rows
    else g.xfRegDirty = true;
}

// ------------------------------------------------------------------ BP
void WriteBP(uint32_t rv) {
    const uint8_t reg = static_cast<uint8_t>(rv >> 24);
    const uint32_t val = rv & 0x00FFFFFFu;
    if (reg == 0xFE) { // mask for the next write only
        g.bpMask = val;
        return;
    }
    const uint32_t nv = (g.bp[reg] & ~g.bpMask) | (val & g.bpMask);
    g.bpMask = 0x00FFFFFFu;

    if (reg >= 0xE0 && reg <= 0xE7) {
        // TEV colour registers and konst colours share addresses; bit 23 selects.
        const int idx = (reg - 0xE0) >> 1;
        int (*dst)[4] = (nv & 0x800000u) ? g.tevKonst : g.tevReg;
        const int lo = sext11(nv & 0x7FF), hi = sext11((nv >> 12) & 0x7FF);
        int old[4];
        std::memcpy(old, dst[idx], sizeof old);
        if (reg & 1) { dst[idx][2] = lo; dst[idx][1] = hi; } // BG
        else { dst[idx][0] = lo; dst[idx][3] = hi; }         // RA
        if (std::memcmp(old, dst[idx], sizeof old) != 0) MarkBP(reg);
        g.bp[reg] = nv;
        return;
    }
    switch (reg) {
    case 0x45: case 0x47: case 0x48: // PE done / tokens: frame sync is handled by the HLE
        return;
    case 0x52:
        VGX_LOG_N(4, "raw BP EFB-copy trigger 0x%06X ignored (copies arrive through GXCopyTex/GXCopyDisp)\n", nv);
        break;
    default:
        if ((reg >= 0x64 && reg <= 0x67) || (reg >= 0x80 && reg <= 0xBB)) {
            // Texture / TLUT hardware registers: textures are bound through GXLoadTexObj here.
            VGX_LOG_N(8, "raw BP texture register 0x%02X = 0x%06X ignored\n", reg, nv);
        }
        break;
    }
    if (reg == 0x00 && (nv & ~g.bp[0x00] & (1u << 19))) ++g_coPlanarOn;
    if (g.bp[reg] != nv) {
        g.bp[reg] = nv;
        MarkBP(reg);
    }
}

void StoreBP(uint8_t reg, uint32_t nv) {
    if (reg >= 0xE0 && reg <= 0xE7) {
        // (GXD_BP_FAST: the same decision as the memcmp below, taken on the two values this register feeds)
        const int idx = (reg - 0xE0) >> 1;
        int (*dst)[4] = (nv & 0x800000u) ? g.tevKonst : g.tevReg;
        const int lo = sext11(nv & 0x7FF), hi = sext11((nv >> 12) & 0x7FF);
        if (Gxd(GXD_BP_FAST)) {
            const int* d = dst[idx];
            bool same;
            if (reg & 1) same = d[2] == lo && d[1] == hi; // BG
            else same = d[0] == lo && d[3] == hi;         // RA
            g.bp[reg] = nv;
            if (same) return;
            if (reg & 1) { dst[idx][2] = lo; dst[idx][1] = hi; }
            else { dst[idx][0] = lo; dst[idx][3] = hi; }
            MarkBP(reg);
            return;
        }
        int old[4];
        std::memcpy(old, dst[idx], sizeof old);
        if (reg & 1) { dst[idx][2] = lo; dst[idx][1] = hi; } // BG
        else { dst[idx][0] = lo; dst[idx][3] = hi; }         // RA
        if (std::memcmp(old, dst[idx], sizeof old) != 0) MarkBP(reg);
        g.bp[reg] = nv;
        return;
    }
    if (reg == 0x00 && (nv & ~g.bp[0x00] & (1u << 19))) ++g_coPlanarOn; // (MSC_VGX_ZFTRACE: how often the game sets GX Z-freeze)
    if (g.bp[reg] != nv) {
        g.bp[reg] = nv;
        MarkBP(reg);
    }
}

void SetBPField(uint8_t reg, int shift, int count, uint32_t value) {
    const uint32_t mask = ((1u << count) - 1u) << shift;
    const uint32_t nv = (g.bp[reg] & ~mask) | ((value << shift) & mask);
    if (nv != g.bp[reg]) {
        g.bp[reg] = nv & 0x00FFFFFFu;
        MarkBP(reg);
    }
}

// ------------------------------------------------------------------ XF
static void WriteXFWord(uint32_t a, uint32_t w) {
    if (a < 0x100) {
        float& f = g.xfPos[a >> 2][a & 3];
        if (f2u(f) != w) { f = u2f(w); g.xfMtxDirty = true; }
    } else if (a >= 0x400 && a < 0x460) {
        const uint32_t o = a - 0x400;
        float& f = g.xfNrm[o / 3][o % 3];
        if (f2u(f) != w) { f = u2f(w); g.xfMtxDirty = true; }
    } else if (a >= 0x500 && a < 0x600) {
        const uint32_t o = a - 0x500;
        float& f = g.xfPost[o >> 2][o & 3];
        if (f2u(f) != w) { f = u2f(w); g.xfMtxDirty = true; }
    } else if (a >= 0x600 && a < 0x680) {
        const uint32_t o = a - 0x600;
        uint32_t& d = g.xfLight[o >> 4][o & 15];
        if (d != w) { d = w; g.xfLightDirty = true; }
    } else if (a >= 0x1000 && a < 0x1058) {
        uint32_t& d = g.xfReg[a - 0x1000];
        if (a == 0x1018) { if (g.matIdxA != w) { g.matIdxA = w; g.xfMtxDirty = true; } }
        else if (a == 0x1019) { if (g.matIdxB != w) { g.matIdxB = w; g.xfMtxDirty = true; } }
        if (d != w) { d = w; MarkXFReg(a - 0x1000); }
    }
}
void WriteXF(uint32_t addr, uint32_t count, const uint32_t* w) {
    // Whole matrices are the common case (12 words of position / texture
    // matrix memory, 9 of normal matrix memory): one compare and one copy
    // instead of a classification per word.
    if (count > 1) {
        if (addr < 0x100 && addr + count <= 0x100) {
            float* dst = &g.xfPos[0][0] + addr; // rows of 4 are contiguous in address order
            if (!SameWords(dst, w, count)) { CopyWords(dst, w, count); g.xfMtxDirty = true; }
            return;
        }
        if (addr >= 0x400 && addr + count <= 0x460) {
            float* dst = &g.xfNrm[0][0] + (addr - 0x400);
            if (!SameWords(dst, w, count)) { CopyWords(dst, w, count); g.xfMtxDirty = true; }
            return;
        }
        if (addr >= 0x500 && addr + count <= 0x600) {
            float* dst = &g.xfPost[0][0] + (addr - 0x500);
            if (!SameWords(dst, w, count)) { CopyWords(dst, w, count); g.xfMtxDirty = true; }
            return;
        }
    }
    for (uint32_t i = 0; i < count; ++i) WriteXFWord(addr + i, w[i]);
}
void WriteXFBE(uint32_t addr, uint32_t count, const uint8_t* be) {
    for (uint32_t i = 0; i < count; ++i) WriteXFWord(addr + i, be32(be + i * 4));
}

// ------------------------------------------------------------------ CP
void SetVcd(int attr, int type) {
    // GX_VA_NBT is not a separate vertex component: the runtime's HLE folds it
    // into GX_VA_NRM (descriptor) and the normal count (format) and republishes
    // both before every draw, so the NBT pseudo-attribute itself is ignored.
    if (attr < 0 || attr > A_TEX7) return;
    if (g.vcd[attr] != type) {
        g.vcd[attr] = static_cast<uint8_t>(type);
        g.layoutDirty = true;
    }
}
void SetVat(int fmt, int attr, int cnt, int type, int frac) {
    if (fmt < 0 || fmt > 7 || attr < A_POS || attr > A_TEX7) return;
    VtxAttrFmt& f = g.vat[fmt][attr];
    if (f.cnt != cnt || f.type != type || f.frac != frac) {
        f.cnt = static_cast<uint8_t>(cnt);
        f.type = static_cast<uint8_t>(type);
        f.frac = static_cast<uint8_t>(frac);
        g.layoutDirty = true;
    }
}
void WriteCP(uint8_t reg, uint32_t v) {
    if (reg == 0x30) { if (g.matIdxA != v) { g.matIdxA = v; g.xfMtxDirty = true; } return; }
    if (reg == 0x40) { if (g.matIdxB != v) { g.matIdxB = v; g.xfMtxDirty = true; } return; }
    if (reg == 0x50) {
        for (int i = 0; i <= 8; ++i) SetVcd(i, (v >> i) & 1);
        SetVcd(A_POS, (v >> 9) & 3);
        SetVcd(A_NRM, (v >> 11) & 3);
        SetVcd(A_CLR0, (v >> 13) & 3);
        SetVcd(A_CLR1, (v >> 15) & 3);
        return;
    }
    if (reg == 0x60) {
        for (int t = 0; t < 8; ++t) SetVcd(A_TEX0 + t, (v >> (t * 2)) & 3);
        return;
    }
    const int fmt = reg & 7;
    if (reg >= 0x70 && reg <= 0x77) {
        SetVat(fmt, A_POS, v & 1, (v >> 1) & 7, (v >> 4) & 0x1F);
        SetVat(fmt, A_NRM, (v & 0x80000000u) ? 2 : ((v >> 9) & 1), (v >> 10) & 7, 0);
        SetVat(fmt, A_CLR0, (v >> 13) & 1, (v >> 14) & 7, 0);
        SetVat(fmt, A_CLR1, (v >> 17) & 1, (v >> 18) & 7, 0);
        SetVat(fmt, A_TEX0, (v >> 21) & 1, (v >> 22) & 7, (v >> 25) & 0x1F);
        return;
    }
    if (reg >= 0x80 && reg <= 0x87) {
        for (int t = 1; t <= 3; ++t) {
            const int s = (t - 1) * 9;
            SetVat(fmt, A_TEX0 + t, (v >> s) & 1, (v >> (s + 1)) & 7, (v >> (s + 4)) & 0x1F);
        }
        SetVat(fmt, A_TEX0 + 4, (v >> 27) & 1, (v >> 28) & 7, g.vat[fmt][A_TEX0 + 4].frac);
        return;
    }
    if (reg >= 0x90 && reg <= 0x97) {
        const VtxAttrFmt t4 = g.vat[fmt][A_TEX0 + 4];
        SetVat(fmt, A_TEX0 + 4, t4.cnt, t4.type, v & 0x1F);
        for (int t = 5; t <= 7; ++t) {
            const int s = 5 + (t - 5) * 9;
            SetVat(fmt, A_TEX0 + t, (v >> s) & 1, (v >> (s + 1)) & 7, (v >> (s + 4)) & 0x1F);
        }
        return;
    }
    // 0xA0..0xBF: array base / stride. Bases are guest addresses; the runtime
    // binds the arrays with GXSetArray(host pointer) instead.
}

void InitState(State& g) {
    g = State{};
    g.bpMask = 0x00FFFFFFu;
    g.bp[0x00] = 0; // genMode: one TEV stage, no texgens
    g.bp[0xF3] = 0x3F0000; // alpha compare: ALWAYS / ALWAYS
    for (int f = 0; f < 8; ++f)
        for (int a = 0; a < 26; ++a) g.vat[f][a] = VtxAttrFmt{};
    g.vcd[A_POS] = T_DIRECT;
    // Identity in the matrix slots the SDK defaults to.
    for (int r = 0; r < 64; ++r)
        for (int c = 0; c < 4; ++c) g.xfPos[r][c] = (r % 3 == c) ? 1.0f : 0.0f;
    for (int r = 0; r < 64; ++r)
        for (int c = 0; c < 4; ++c) g.xfPost[r][c] = (r % 3 == c) ? 1.0f : 0.0f;
    for (int r = 0; r < 32; ++r)
        for (int c = 0; c < 3; ++c) g.xfNrm[r][c] = (r % 3 == c) ? 1.0f : 0.0f;
    g.matIdxA = 0x3CF3CF00u;
    g.matIdxB = 0x00F3CF3Cu;
    for (int i = 0; i < 8; ++i) g.xfReg[0x50 + i] = 61; // post matrix = identity slot
    g.xfReg[0x1A] = f2u(320.0f); g.xfReg[0x1B] = f2u(-240.0f); g.xfReg[0x1C] = f2u(16777215.0f);
    g.xfReg[0x1D] = f2u(320.0f + 342.0f); g.xfReg[0x1E] = f2u(240.0f + 342.0f); g.xfReg[0x1F] = f2u(16777215.0f);
    g.xfReg[0x26] = 1;
    g.xfReg[0x20] = f2u(1.0f); g.xfReg[0x22] = f2u(1.0f); g.xfReg[0x24] = f2u(1.0f);
}
void ResetState() { InitState(g); }

// ------------------------------------------------------- vertex decoding
namespace {

PlanCache g_plans;

inline int CompSize(int type) { return type == C_F32 ? 4 : (type >= C_U16 ? 2 : 1); }
inline int ColorSize(int type) {
    switch (type) {
    case 0: return 2; // RGB565
    case 1: return 3; // RGB8
    case 2: return 4; // RGBX8
    case 3: return 2; // RGBA4
    case 4: return 3; // RGBA6
    default: return 4; // RGBA8
    }
}
inline int NrmFrac(int type) {
    switch (type) { case C_U8: return 7; case C_S8: return 6; case C_U16: return 15; case C_S16: return 14; default: return 0; }
}

} // namespace

const Plan& GetPlanIn(PlanCache& cache, bool& dirty, const uint8_t* vcd, const VtxAttrFmt (*vat)[26], int fmt, bool recordStats) {
    if (dirty) {
        for (auto& p : cache.p) p.valid = false;
        dirty = false;
    }
    Plan& p = cache.p[fmt & 7];
    if (p.valid) return p;
    p = Plan{};
    uint32_t mask = 0;
    if (vcd[A_NRM]) mask |= VL_NRM;
    if (vcd[A_CLR0]) mask |= VL_COL0;
    if (vcd[A_CLR1]) mask |= VL_COL1;
    for (int t = 0; t < 8; ++t) {
        if (vcd[A_TEX0 + t]) mask |= VL_TEX0 << t;
        if (vcd[A_TEX0MTXIDX + t]) mask |= VL_TMI;
    }
    p.layoutMask = mask;
    p.layout = MakeLayout(mask);
    const VtxLayout& L = p.layout;
    for (int attr = 0; attr <= A_TEX7; ++attr) {
        const int mode = vcd[attr];
        if (mode == T_NONE) continue;
        AttrPlan ap{};
        ap.attr = static_cast<uint8_t>(attr);
        ap.mode = static_cast<uint8_t>(mode);
        const VtxAttrFmt& f = vat[fmt & 7][attr];
        ap.type = f.type; ap.cnt = f.cnt; ap.frac = f.frac;
        int direct = 0;
        if (attr <= 8) {
            ap.mode = T_DIRECT;
            direct = 1;
            ap.outOff = attr == 0 ? 12 : static_cast<uint8_t>(L.offTmi + (attr - 1));
            p.callsPerVertex += 1;
        } else if (attr == A_POS) {
            ap.comps = f.cnt ? 3 : 2;
            direct = ap.comps * CompSize(f.type);
            ap.outOff = 0;
            p.callsPerVertex += 1;
        } else if (attr == A_NRM) {
            ap.comps = f.cnt ? 9 : 3;
            ap.frac = static_cast<uint8_t>(NrmFrac(f.type));
            direct = ap.comps * CompSize(f.type);
            ap.outOff = L.offNrm;
            p.callsPerVertex += f.cnt ? 3 : 1;
        } else if (attr <= A_CLR1) {
            direct = ColorSize(f.type);
            ap.outOff = attr == A_CLR0 ? L.offCol0 : L.offCol1;
            p.callsPerVertex += 1;
        } else {
            ap.comps = f.cnt ? 2 : 1;
            direct = ap.comps * CompSize(f.type);
            ap.outOff = L.offTex[attr - A_TEX0];
            p.callsPerVertex += 1;
        }
        ap.elemSize = static_cast<uint8_t>(direct);
        if (ap.mode == T_DIRECT) ap.srcSize = static_cast<uint8_t>(direct);
        else {
            const int idx = ap.mode == T_INDEX8 ? 1 : 2;
            // NBT3: three indices, each selecting one 3-component vector.
            ap.srcSize = static_cast<uint8_t>((attr == A_NRM && f.cnt == 2) ? idx * 3 : idx);
        }
        p.streamSize += ap.srcSize;
        p.a[p.n++] = ap;
        if (recordStats) stats::VertexFormat(attr, mode, f);
    }
    p.valid = true;
    return p;
}

namespace {
inline const Plan& GetPlan(int fmt) { return GetPlanIn(g_plans, g.layoutDirty, g.vcd, g.vat, fmt, true); }

inline float ReadComp(const uint8_t* s, int type, bool le) {
    switch (type) {
    case C_U8: return static_cast<float>(s[0]);
    case C_S8: return static_cast<float>(static_cast<int8_t>(s[0]));
    case C_U16: return static_cast<float>(le ? static_cast<uint16_t>(s[0] | (s[1] << 8)) : be16(s));
    case C_S16: return static_cast<float>(static_cast<int16_t>(le ? static_cast<uint16_t>(s[0] | (s[1] << 8)) : be16(s)));
    default:
        if (le) { float f; std::memcpy(&f, s, 4); return f; }
        return bef32(s);
    }
}
inline void DecodeColor(const uint8_t* s, int type, uint8_t* o, bool le = false) {
    switch (type) {
    case 0: { // RGB565
        const uint16_t v = le ? le16(s) : be16(s);
        const int r = (v >> 11) & 0x1F, gg = (v >> 5) & 0x3F, b = v & 0x1F;
        o[0] = static_cast<uint8_t>((r << 3) | (r >> 2)); o[1] = static_cast<uint8_t>((gg << 2) | (gg >> 4));
        o[2] = static_cast<uint8_t>((b << 3) | (b >> 2)); o[3] = 255;
        break;
    }
    case 1: case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
    case 3: { // RGBA4
        const uint16_t v = le ? le16(s) : be16(s);
        o[0] = static_cast<uint8_t>(((v >> 12) & 0xF) * 17); o[1] = static_cast<uint8_t>(((v >> 8) & 0xF) * 17);
        o[2] = static_cast<uint8_t>(((v >> 4) & 0xF) * 17); o[3] = static_cast<uint8_t>((v & 0xF) * 17);
        break;
    }
    case 4: { // RGBA6
        const uint32_t v = (static_cast<uint32_t>(s[0]) << 16) | (static_cast<uint32_t>(s[1]) << 8) | s[2];
        const int r = (v >> 18) & 0x3F, gg = (v >> 12) & 0x3F, b = (v >> 6) & 0x3F, a = v & 0x3F;
        o[0] = static_cast<uint8_t>((r << 2) | (r >> 4)); o[1] = static_cast<uint8_t>((gg << 2) | (gg >> 4));
        o[2] = static_cast<uint8_t>((b << 2) | (b >> 4)); o[3] = static_cast<uint8_t>((a << 2) | (a >> 4));
        break;
    }
    default: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3]; break;
    }
}

const uint8_t kZero[64] = {};

// Resolves an index into an array element pointer; out-of-range reads see zeros.
inline const uint8_t* ArrayElem(int attr, uint32_t idx, uint32_t elemSize, bool& le) {
    const ArrayRef& ar = g.arrays[attr];
    le = ar.le;
    const uint64_t off = static_cast<uint64_t>(idx) * ar.stride;
    if (!ar.data || off + elemSize > ar.size) {
        VGX_LOG_N(16, "vertex array read out of range: attr %d idx %u stride %u size %u\n", attr, idx, ar.stride, ar.size);
        return kZero;
    }
    return ar.data + off;
}

} // namespace

uint32_t StreamVertexSize(int fmt) { return GetPlan(fmt).streamSize; }
const Plan& CurrentPlan(int fmt) { return GetPlan(fmt); }

// Attribute-major vertex decode: one tight loop per vertex component over the
// whole primitive instead of a per-vertex walk over the components. `src` is
// the big-endian vertex stream (count * p.streamSize bytes), `out` receives
// count * p.layout.stride bytes in the decoded layout. `spans` (optional)
// receives, per indexed attribute, the byte range of its array that was read.
static uint32_t g_lastPosMtxMax = 0;
uint32_t LastDecodedPosMtxMax() { return g_lastPosMtxMax; }

namespace {
// One attribute of `count` vertices in a loop of its own, compiled once for every combination
// of: where an element comes from (IDX 0: the stream itself, 1 / 2: an 8- / 16-bit index into
// an array, with the 32-bit offset test of DecodeVertices), the component type, the number of
// components, and whether a missing second texture coordinate is zeroed. The general loop
// below it keeps all of those in variables; on the console's processor, with its fourteen
// registers, it then reloads eight values from the stack for every vertex and attribute
// (35 instructions where this takes about 20; found in the per-block profile of round 5).
// Same arithmetic, expression for expression, as the general loop: results are bit-identical.
// Returns non-zero when an index pointed outside its array (the element then reads as zeros).
template <int IDX>
inline __attribute__((always_inline)) const uint8_t* AttrElem(const uint8_t* s, const uint8_t* adata, uint32_t astride, uint32_t maxOff, uint32_t& oob) {
    if (IDX == 0) return s;
    const uint32_t idx = IDX == 2 ? be16(s) : s[0];
    const uint32_t off = idx * astride;
    if (off > maxOff) { oob = 1; return kZero; }
    return adata + off;
}
// LE: the array holds host-order (little-endian) values (the native port's own arrays; a recompiled build never
// binds one). Only array elements can be little-endian: the index bytes of the stream and direct data stay big-endian.
template <int TYPE, bool LE> inline __attribute__((always_inline)) uint32_t AttrComp(const uint8_t* e, int c, float scale) {
    // The bit pattern of component c as a float in the decoded layout.
    switch (TYPE) {
    case C_F32: return LE ? le32(e + 4 * c) : be32(e + 4 * c); // (same value as "float * 1.0f" for every input that is not a NaN)
    case C_S16: return f2u(static_cast<float>(static_cast<int16_t>(LE ? le16(e + 2 * c) : be16(e + 2 * c))) * scale);
    case C_U16: return f2u(static_cast<float>(LE ? le16(e + 2 * c) : be16(e + 2 * c)) * scale);
    case C_S8: return f2u(static_cast<float>(static_cast<int8_t>(e[c])) * scale);
    default: return f2u(static_cast<float>(e[c]) * scale);
    }
}
template <int IDX, int TYPE, int N, bool PAD, bool LE>
__attribute__((noinline)) uint32_t DecodeAttr(const uint8_t* s, uint8_t* o, uint32_t count, uint32_t ss, uint32_t os, const uint8_t* adata,
                                              uint32_t astride, uint32_t maxOff, float scale) {
    uint32_t oob = 0;
    for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
        const uint8_t* e = AttrElem<IDX>(s, adata, astride, maxOff, oob);
        const uint32_t w0 = AttrComp<TYPE, LE>(e, 0, scale);
        std::memcpy(o, &w0, 4);
        if (N > 1) { const uint32_t w1 = AttrComp<TYPE, LE>(e, 1, scale); std::memcpy(o + 4, &w1, 4); }
        if (N > 2) { const uint32_t w2 = AttrComp<TYPE, LE>(e, 2, scale); std::memcpy(o + 8, &w2, 4); }
        if (PAD) std::memset(o + 4, 0, 4);
    }
    return oob;
}
template <int IDX>
__attribute__((noinline)) uint32_t DecodeRgba8(const uint8_t* s, uint8_t* o, uint32_t count, uint32_t ss, uint32_t os, const uint8_t* adata, uint32_t astride,
                                               uint32_t maxOff, float) {
    uint32_t oob = 0;
    for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
        const uint8_t* e = AttrElem<IDX>(s, adata, astride, maxOff, oob);
        std::memcpy(o, e, 4);
    }
    return oob;
}
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
// GXD_DEC_NEON. Lane loads through these types carry no alignment hint: array elements of 16-bit data are only 2-aligned, and a NEON load
// through a pointer to a 4-byte type asks for 4-byte alignment (a data abort otherwise: found by the Vita port's pool bug, see W5-common.md).
typedef uint32_t __attribute__((aligned(1), may_alias)) U32u;
typedef uint16_t __attribute__((aligned(1), may_alias)) U16u;

// 16-bit index, signed 16-bit fixed-point components (texture coordinates, small positions and normals), N = 2 or 3 components, two vertices per
// iteration: one conversion for four (N = 2) or six (N = 3) components. Same arithmetic as DecodeAttr (int16 -> float, one multiplication by the
// power-of-two scale): the results are bit-identical. About 12 instructions per vertex where the scalar loop takes 27 (N = 2).
template <int N, bool LE>
__attribute__((noinline)) uint32_t DecodeS16Idx16Neon(const uint8_t* s, uint8_t* o, uint32_t count, uint32_t ss, uint32_t os, const uint8_t* adata,
                                                      uint32_t astride, uint32_t maxOff, float scale) {
    uint32_t oob = 0;
    const float32x4_t vs = vdupq_n_f32(scale);
    for (; count >= 2; count -= 2, s += 2 * ss, o += 2 * os) {
        const uint32_t off0 = static_cast<uint32_t>(be16(s)) * astride, off1 = static_cast<uint32_t>(be16(s + ss)) * astride;
        const bool b0 = off0 > maxOff, b1 = off1 > maxOff;
        oob |= static_cast<uint32_t>(b0 | b1);
        const uint8_t* e0 = b0 ? kZero : adata + off0;
        const uint8_t* e1 = b1 ? kZero : adata + off1;
        uint32x2_t xy = vld1_lane_u32(reinterpret_cast<const U32u*>(e0), vdup_n_u32(0), 0);
        xy = vld1_lane_u32(reinterpret_cast<const U32u*>(e1), xy, 1);
        int16x4_t hxy = vreinterpret_s16_u32(xy);
        if (!LE) hxy = vreinterpret_s16_u8(vrev16_u8(vreinterpret_u8_s16(hxy)));
        const float32x4_t f = vmulq_f32(vcvtq_f32_s32(vmovl_s16(hxy)), vs); // x0 y0 x1 y1
        float* const p0 = reinterpret_cast<float*>(o);
        float* const p1 = reinterpret_cast<float*>(o + os);
        vst1_f32(p0, vget_low_f32(f));
        vst1_f32(p1, vget_high_f32(f));
        if (N == 3) {
            uint16x4_t z = vld1_lane_u16(reinterpret_cast<const U16u*>(e0 + 4), vdup_n_u16(0), 0);
            z = vld1_lane_u16(reinterpret_cast<const U16u*>(e1 + 4), z, 1);
            int16x4_t hz = vreinterpret_s16_u16(z);
            if (!LE) hz = vreinterpret_s16_u8(vrev16_u8(vreinterpret_u8_s16(hz)));
            const float32x4_t fz = vmulq_f32(vcvtq_f32_s32(vmovl_s16(hz)), vs); // z0 z1 0 0
            const uint32x4_t wz = vreinterpretq_u32_f32(fz);
            vst1q_lane_u32(reinterpret_cast<U32u*>(p0 + 2), wz, 0);
            vst1q_lane_u32(reinterpret_cast<U32u*>(p1 + 2), wz, 1);
        }
    }
    if (count) oob |= DecodeAttr<2, C_S16, N, false, LE>(s, o, count, ss, os, adata, astride, maxOff, scale);
    return oob;
}
// 16-bit index, 32-bit float components (N = 2 or 3): the words are moved as they are (byte-reversed when the array is big-endian), so NaN payloads
// and denormals survive exactly as in AttrComp<C_F32>. Two vertices per iteration.
template <int N, bool LE>
__attribute__((noinline)) uint32_t DecodeF32Idx16Neon(const uint8_t* s, uint8_t* o, uint32_t count, uint32_t ss, uint32_t os, const uint8_t* adata,
                                                      uint32_t astride, uint32_t maxOff, float) {
    uint32_t oob = 0;
    for (; count >= 2; count -= 2, s += 2 * ss, o += 2 * os) {
        const uint32_t off0 = static_cast<uint32_t>(be16(s)) * astride, off1 = static_cast<uint32_t>(be16(s + ss)) * astride;
        const bool b0 = off0 > maxOff, b1 = off1 > maxOff;
        oob |= static_cast<uint32_t>(b0 | b1);
        const uint8_t* e0 = b0 ? kZero : adata + off0;
        const uint8_t* e1 = b1 ? kZero : adata + off1;
        uint8x8_t xy0 = vld1_u8(e0), xy1 = vld1_u8(e1);
        if (!LE) { xy0 = vrev32_u8(xy0); xy1 = vrev32_u8(xy1); }
        vst1_f32(reinterpret_cast<float*>(o), vreinterpret_f32_u8(xy0));
        vst1_f32(reinterpret_cast<float*>(o + os), vreinterpret_f32_u8(xy1));
        if (N == 3) {
            uint32x2_t z = vld1_lane_u32(reinterpret_cast<const U32u*>(e0 + 8), vdup_n_u32(0), 0);
            z = vld1_lane_u32(reinterpret_cast<const U32u*>(e1 + 8), z, 1);
            if (!LE) z = vreinterpret_u32_u8(vrev32_u8(vreinterpret_u8_u32(z)));
            vst1_lane_u32(reinterpret_cast<U32u*>(o + 8), z, 0);
            vst1_lane_u32(reinterpret_cast<U32u*>(o + os + 8), z, 1);
        }
    }
    if (count) oob |= DecodeAttr<2, C_F32, N, false, LE>(s, o, count, ss, os, adata, astride, maxOff, 1.0f);
    return oob;
}
// The lowest and highest 16-bit big-endian index among `count` at s, s + ss, ... (four per iteration: one lane load per index, one byte swap, one
// minimum and one maximum; a scalar loop is about 18 instructions per index).
inline void IndexRange16(const uint8_t* s, uint32_t count, uint32_t ss, uint32_t& mn, uint32_t& mx) {
    uint16x4_t vmn = vdup_n_u16(0xFFFF), vmx = vdup_n_u16(0);
    uint32_t lo = 0xFFFFu, hi = 0;
    for (; count >= 4; count -= 4, s += 4 * ss) {
        uint16x4_t t = vld1_lane_u16(reinterpret_cast<const U16u*>(s), vdup_n_u16(0), 0);
        t = vld1_lane_u16(reinterpret_cast<const U16u*>(s + ss), t, 1);
        t = vld1_lane_u16(reinterpret_cast<const U16u*>(s + 2 * ss), t, 2);
        t = vld1_lane_u16(reinterpret_cast<const U16u*>(s + 3 * ss), t, 3);
        t = vreinterpret_u16_u8(vrev16_u8(vreinterpret_u8_u16(t)));
        vmn = vmin_u16(vmn, t);
        vmx = vmax_u16(vmx, t);
    }
    const uint16x4_t a = vpmin_u16(vmn, vmn), b = vpmax_u16(vmx, vmx);
    lo = vget_lane_u16(vpmin_u16(a, a), 0);
    hi = vget_lane_u16(vpmax_u16(b, b), 0);
    for (; count; --count, s += ss) {
        const uint32_t i = be16(s);
        if (i < lo) lo = i;
        if (i > hi) hi = i;
    }
    mn = lo;
    mx = hi;
}
// The position-matrix-index column: the byte of every vertex as a float, and the highest one (GX_VA_PNMTXIDX; 8 instructions per vertex in the
// scalar loop, about 4 here).
inline uint32_t MatrixIndexColumn(const uint8_t* s, uint8_t* o, uint32_t count, uint32_t ss, uint32_t os) {
    uint8x8_t vmx = vdup_n_u8(0);
    for (; count >= 4; count -= 4, s += 4 * ss, o += 4 * os) {
        uint8x8_t t = vld1_lane_u8(s, vdup_n_u8(0), 0);
        t = vld1_lane_u8(s + ss, t, 1);
        t = vld1_lane_u8(s + 2 * ss, t, 2);
        t = vld1_lane_u8(s + 3 * ss, t, 3);
        vmx = vmax_u8(vmx, t);
        const float32x4_t f = vcvtq_f32_u32(vmovl_u16(vget_low_u16(vmovl_u8(t))));
        const uint32x4_t w = vreinterpretq_u32_f32(f);
        vst1q_lane_u32(reinterpret_cast<U32u*>(o), w, 0);
        vst1q_lane_u32(reinterpret_cast<U32u*>(o + os), w, 1);
        vst1q_lane_u32(reinterpret_cast<U32u*>(o + 2 * os), w, 2);
        vst1q_lane_u32(reinterpret_cast<U32u*>(o + 3 * os), w, 3);
    }
    const uint8x8_t m1 = vpmax_u8(vmx, vmx), m2 = vpmax_u8(m1, m1), m3 = vpmax_u8(m2, m2);
    uint32_t mx = vget_lane_u8(m3, 0);
    for (; count; --count, s += ss, o += os) {
        if (s[0] > mx) mx = s[0];
        const float f = static_cast<float>(s[0]);
        std::memcpy(o, &f, 4);
    }
    return mx;
}
#endif
// MSC_VGX_GXD with GXD_VERIFY (1 << 30): every shortcut of the decoder also runs the plain code and reports a difference (first 8, then a count at exit).
uint64_t g_gxdVerifyFails = 0;
inline bool GxdVerify() { return Gxd(GXD_VERIFY); }
inline void GxdFail(const char* what, uint32_t a = 0, uint32_t b = 0) {
    if (++g_gxdVerifyFails <= 8) std::fprintf(stderr, "[vgx] GXD VERIFY FAILED: %s (%u %u)\n", what, a, b);
}
using AttrFn = uint32_t (*)(const uint8_t*, uint8_t*, uint32_t, uint32_t, uint32_t, const uint8_t*, uint32_t, uint32_t, float);
// [IDX][type][components - 1, or 3 for one component with the second zeroed]; kAttrFnLe: the same for little-endian
// array elements (IDX 1, 2 only: a direct attribute is stream data, which is always big-endian).
#define VGX_ATTR_ROW(I, T, L) {&DecodeAttr<I, T, 1, false, L>, &DecodeAttr<I, T, 2, false, L>, &DecodeAttr<I, T, 3, false, L>, &DecodeAttr<I, T, 1, true, L>}
#define VGX_ATTR_IDX(I, L) {VGX_ATTR_ROW(I, C_U8, L), VGX_ATTR_ROW(I, C_S8, L), VGX_ATTR_ROW(I, C_U16, L), VGX_ATTR_ROW(I, C_S16, L), VGX_ATTR_ROW(I, C_F32, L)}
const AttrFn kAttrFn[3][5][4] = {VGX_ATTR_IDX(0, false), VGX_ATTR_IDX(1, false), VGX_ATTR_IDX(2, false)};
const AttrFn kAttrFnLe[3][5][4] = {VGX_ATTR_IDX(0, false), VGX_ATTR_IDX(1, true), VGX_ATTR_IDX(2, true)};
const AttrFn kRgba8Fn[3] = {&DecodeRgba8<0>, &DecodeRgba8<1>, &DecodeRgba8<2>};
#undef VGX_ATTR_ROW
#undef VGX_ATTR_IDX
// MSC_VGX_DECODE=0: the general loops only (to compare the two: frame hashes must be equal).
inline bool SpecialisedDecode() {
    static const bool on = EnvInt("MSC_VGX_DECODE", 1) != 0;
    return on;
}
} // namespace

void DecodeVertices(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out, ArraySpans* spans) {
    if (TagCensusOn()) TagCensusDecode(count);
    const VtxLayout& L = p.layout;
    g_lastPosMtxMax = 0;
    const uint32_t ss = p.streamSize, os = L.stride;
    if (spans) *spans = ArraySpans{};
    // Defaults: z = 0 for two-component positions, matrix row 0, texture-matrix indices 0.
    // (Skipped for what an attribute of this plan writes for every vertex anyway: z by a
    // three-component position, the matrix row by PNMTXIDX.)
    {
        bool needZ = true, needW = true;
        for (int i = 0; i < p.n; ++i) {
            if (p.a[i].attr == A_POS && p.a[i].comps == 3) needZ = false;
            if (p.a[i].attr == 0) needW = false;
        }
        if (!SpecialisedDecode()) needZ = needW = true;
        const bool tmi = (L.mask & VL_TMI) != 0;
        if (needZ || needW || tmi) {
            uint8_t* o = out;
            for (uint32_t v = 0; v < count; ++v, o += os) {
                if (needZ) std::memset(o + 8, 0, 4);
                if (needW) std::memset(o + 12, 0, 4);
                if (tmi) std::memset(o + L.offTmi, 0, 8);
            }
        }
    }
    uint32_t srcOff = 0;
    for (int i = 0; i < p.n; ++i) {
        const AttrPlan& a = p.a[i];
        const uint8_t* s = src + srcOff;
        srcOff += a.srcSize;
        uint8_t* o = out + a.outOff;
        if (a.attr <= 8) {
            if (a.attr == 0) {
                uint32_t mx = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                if (Gxd(GXD_DEC_NEON)) {
                    mx = MatrixIndexColumn(s, o, count, ss, os);
                    if (GxdVerify()) {
                        uint32_t mx2 = 0;
                        for (uint32_t v = 0; v < count; ++v) {
                            if (s[static_cast<size_t>(v) * ss] > mx2) mx2 = s[static_cast<size_t>(v) * ss];
                            const float f = static_cast<float>(s[static_cast<size_t>(v) * ss]);
                            if (std::memcmp(o + static_cast<size_t>(v) * os, &f, 4) != 0) { GxdFail("matrix index column differs", v, count); break; }
                        }
                        if (mx2 != mx) GxdFail("matrix index maximum differs", mx, mx2);
                    }
                } else
#endif
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                    if (s[0] > mx) mx = s[0];
                    const float f = static_cast<float>(s[0]);
                    std::memcpy(o, &f, 4); // outOff = 12: the position's w
                }
                g_lastPosMtxMax = mx;
            } else {
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) o[0] = s[0];
            }
            continue;
        }
        // Source element for vertex v: the stream itself (direct) or an array element (indexed).
        const ArrayRef& ar = g.arrays[a.attr];
        const bool indexed = a.mode != T_DIRECT;
        const bool idx16 = a.mode == T_INDEX16;
        const uint32_t elem = (a.attr == A_NRM && a.cnt == 2) ? 3u * static_cast<uint32_t>(CompSize(a.type)) : a.elemSize; // NBT3: normal only
        const bool le = indexed && ar.le; // little-endian array elements (see AttrComp)
        uint32_t minIdx = 0xFFFFFFFFu, maxIdx = 0;
        const uint8_t* const adata = ar.data;
        const uint32_t astride = ar.stride;
        const uint64_t asize = adata ? ar.size : 0;
        bool oob = false;
        // An index is at most 65535, so with a stride below 65536 its offset fits 32 bits and
        // one unsigned comparison says whether the element lies inside the array (the 64-bit
        // form was 44 instructions per vertex and attribute with the range bookkeeping below).
        const bool narrow = astride <= 0xFFFFu && asize >= elem && asize - elem <= 0xFFFFFFFFull;
        const uint32_t maxOff = narrow ? static_cast<uint32_t>(asize - elem) : 0;
        // The lowest and highest index that addressed an element (for the display-list cache,
        // which wants the byte range of each array a list read): a pass of its own, and only
        // when somebody asks.
        if (indexed && spans) {
            const uint8_t* si = s;
            bool done = false;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            // GXD_DEC_NEON: when the highest index addresses an element inside the array so does every other one (the test is monotonic in the
            // index), and the range is the plain minimum and maximum; otherwise the exact loop below decides which indices count.
            if (idx16 && count && Gxd(GXD_DEC_NEON)) {
                uint32_t mn, mx;
                IndexRange16(si, count, ss, mn, mx);
                if (static_cast<uint64_t>(mx) * astride + elem <= asize) { minIdx = mn; maxIdx = mx; done = true; }
                if (GxdVerify()) {
                    uint32_t lo2 = 0xFFFFFFFFu, hi2 = 0;
                    for (uint32_t v = 0; v < count; ++v) {
                        const uint32_t idx = be16(si + static_cast<size_t>(v) * ss);
                        if (idx < lo2) lo2 = idx;
                        if (idx > hi2) hi2 = idx;
                    }
                    if (lo2 != mn || hi2 != mx) GxdFail("index range differs", mn, lo2);
                }
            }
#endif
            if (!done)
            for (uint32_t v = 0; v < count; ++v, si += ss) {
                const uint32_t idx = idx16 ? be16(si) : si[0];
                if (static_cast<uint64_t>(idx) * astride + elem > asize) continue;
                if (idx < minIdx) minIdx = idx;
                if (idx > maxIdx) maxIdx = idx;
            }
        }
        if (le && (!(narrow && SpecialisedDecode()) || ((a.attr == A_CLR0 || a.attr == A_CLR1) && a.type != 5))) {
            // Little-endian elements without a specialised kernel (16-bit colours, out-of-range or wide strides, MSC_VGX_DECODE=0).
            for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                bool leElem = false;
                const uint8_t* e = ArrayElem(a.attr, idx16 ? be16(s) : s[0], elem, leElem);
                if (a.attr == A_CLR0 || a.attr == A_CLR1) { DecodeColor(e, a.type, o, leElem); continue; }
                const int cs = CompSize(a.type);
                const float scale = a.type == C_F32 ? 1.0f : 1.0f / static_cast<float>(1u << a.frac);
                const int n = a.attr == A_NRM ? 3 : a.comps;
                float f[3] = {0.0f, 0.0f, 0.0f};
                for (int c = 0; c < n; ++c) f[c] = ReadComp(e + c * cs, a.type, leElem) * scale;
                if (a.attr == A_POS) std::memcpy(o, f, static_cast<size_t>(n) * 4);
                else if (a.attr == A_NRM) std::memcpy(o, f, 12);
                else std::memcpy(o, f, 8);
            }
            if (spans && minIdx <= maxIdx) {
                spans->mask |= 1u << a.attr;
                spans->lo[a.attr - A_POS] = minIdx * astride;
                spans->hi[a.attr - A_POS] = maxIdx * astride + elem;
            }
            continue;
        }
        if ((!indexed || narrow) && SpecialisedDecode()) {
            const int im = !indexed ? 0 : (idx16 ? 2 : 1);
            const bool color = a.attr == A_CLR0 || a.attr == A_CLR1;
            if (!color || a.type == 5) {
                uint32_t bad;
                if (color) {
                    bad = kRgba8Fn[im](s, o, count, ss, os, adata, astride, maxOff, 1.0f);
                } else {
                    // POS: 2 or 3 components; NRM: 3 (of 3 or 9); TEXn: 1 or 2 (the second defaults to 0).
                    const int n = a.attr == A_NRM ? 3 : a.comps;
                    const bool padT = a.attr >= A_TEX0 && n == 1;
                    const float scale = 1.0f / static_cast<float>(1u << a.frac);
                    const int type = a.type <= C_F32 ? a.type : C_U8;
                    AttrFn fn = (le ? kAttrFnLe : kAttrFn)[im][type][padT ? 3 : (n >= 1 && n <= 3 ? n - 1 : 2)];
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                    if (im == 2 && !padT && (n == 2 || n == 3) && Gxd(GXD_DEC_NEON)) {
                        if (type == C_S16)
                            fn = n == 2 ? (le ? &DecodeS16Idx16Neon<2, true> : &DecodeS16Idx16Neon<2, false>) : (le ? &DecodeS16Idx16Neon<3, true> : &DecodeS16Idx16Neon<3, false>);
                        else if (type == C_F32)
                            fn = n == 2 ? (le ? &DecodeF32Idx16Neon<2, true> : &DecodeF32Idx16Neon<2, false>) : (le ? &DecodeF32Idx16Neon<3, true> : &DecodeF32Idx16Neon<3, false>);
                    }
#endif
                    bad = fn(s, o, count, ss, os, adata, astride, maxOff, scale);
                    if (fn != (le ? kAttrFnLe : kAttrFn)[im][type][padT ? 3 : (n >= 1 && n <= 3 ? n - 1 : 2)] && GxdVerify()) {
                        // the same attribute with the scalar kernel into a scratch area: the bytes it writes must be equal
                        static std::vector<uint8_t> tmp;
                        tmp.assign(static_cast<size_t>(count) * os + 16, 0);
                        const uint32_t bad2 = (le ? kAttrFnLe : kAttrFn)[im][type][padT ? 3 : (n >= 1 && n <= 3 ? n - 1 : 2)](s, tmp.data(), count, ss, os, adata, astride, maxOff, scale);
                        if (bad2 != bad) GxdFail("decode: out-of-range flag differs", bad, bad2);
                        for (uint32_t v = 0; v < count; ++v)
                            if (std::memcmp(o + static_cast<size_t>(v) * os, tmp.data() + static_cast<size_t>(v) * os, static_cast<size_t>(n) * 4) != 0) { GxdFail("decode: vertex differs", v, count); break; }
                    }
                }
                if (bad) VGX_LOG_N(16, "vertex array read out of range: attr %d stride %u size %u\n", a.attr, ar.stride, ar.size);
                if (indexed && spans && minIdx <= maxIdx) {
                    spans->mask |= 1u << a.attr;
                    spans->lo[a.attr - A_POS] = minIdx * astride;
                    spans->hi[a.attr - A_POS] = maxIdx * astride + elem;
                }
                continue;
            }
        }
#define VGX_ELEM(e)                                                                                   \
        const uint8_t* e;                                                                             \
        if (indexed) {                                                                                \
            const uint32_t idx = idx16 ? be16(s) : s[0];                                              \
            if (narrow) {                                                                             \
                const uint32_t off = idx * astride;                                                   \
                if (off > maxOff) { e = kZero; oob = true; }                                          \
                else e = adata + off;                                                                 \
            } else {                                                                                  \
                const uint64_t off = static_cast<uint64_t>(idx) * astride;                            \
                if (off + elem > asize) { e = kZero; oob = true; }                                    \
                else e = adata + off;                                                                 \
            }                                                                                         \
        } else e = s;
// One store per component (a memcpy of n * 4 bytes with n not a constant is a call into
// the C library for every vertex: 12 500 calls per open-play frame).
#define VGX_STORE(src)                                    \
        std::memcpy(o, &src[0], 4);                       \
        if (n > 1) std::memcpy(o + 4, &src[1], 4);        \
        if (n > 2) std::memcpy(o + 8, &src[2], 4);
        if (a.attr == A_CLR0 || a.attr == A_CLR1) {
            if (a.type == 5) { // RGBA8
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) { VGX_ELEM(e) std::memcpy(o, e, 4); }
            } else {
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) { VGX_ELEM(e) DecodeColor(e, a.type, o); }
            }
        } else {
            // POS: 2 or 3 components; NRM: 3 (of 3 or 9); TEXn: 1 or 2 (the second defaults to 0).
            const int n = a.attr == A_NRM ? 3 : a.comps;
            const bool padT = a.attr >= A_TEX0 && n == 1;
            const float scale = 1.0f / static_cast<float>(1u << a.frac);
            switch (a.type) {
            case C_F32:
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                    VGX_ELEM(e)
                    uint32_t w[3];
                    w[0] = be32(e);
                    if (n > 1) w[1] = be32(e + 4);
                    if (n > 2) w[2] = be32(e + 8);
                    // Same value as the generic path's "float * 1.0f" for every non-NaN input.
                    VGX_STORE(w)
                    if (padT) std::memset(o + 4, 0, 4);
                }
                break;
            case C_S16:
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                    VGX_ELEM(e)
                    float f[3];
                    f[0] = static_cast<float>(static_cast<int16_t>(be16(e))) * scale;
                    if (n > 1) f[1] = static_cast<float>(static_cast<int16_t>(be16(e + 2))) * scale;
                    if (n > 2) f[2] = static_cast<float>(static_cast<int16_t>(be16(e + 4))) * scale;
                    VGX_STORE(f)
                    if (padT) std::memset(o + 4, 0, 4);
                }
                break;
            case C_U16:
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                    VGX_ELEM(e)
                    float f[3];
                    f[0] = static_cast<float>(be16(e)) * scale;
                    if (n > 1) f[1] = static_cast<float>(be16(e + 2)) * scale;
                    if (n > 2) f[2] = static_cast<float>(be16(e + 4)) * scale;
                    VGX_STORE(f)
                    if (padT) std::memset(o + 4, 0, 4);
                }
                break;
            case C_S8:
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                    VGX_ELEM(e)
                    float f[3];
                    f[0] = static_cast<float>(static_cast<int8_t>(e[0])) * scale;
                    if (n > 1) f[1] = static_cast<float>(static_cast<int8_t>(e[1])) * scale;
                    if (n > 2) f[2] = static_cast<float>(static_cast<int8_t>(e[2])) * scale;
                    VGX_STORE(f)
                    if (padT) std::memset(o + 4, 0, 4);
                }
                break;
            default: // C_U8
                for (uint32_t v = 0; v < count; ++v, s += ss, o += os) {
                    VGX_ELEM(e)
                    float f[3];
                    f[0] = static_cast<float>(e[0]) * scale;
                    if (n > 1) f[1] = static_cast<float>(e[1]) * scale;
                    if (n > 2) f[2] = static_cast<float>(e[2]) * scale;
                    VGX_STORE(f)
                    if (padT) std::memset(o + 4, 0, 4);
                }
                break;
            }
        }
#undef VGX_ELEM
#undef VGX_STORE
        if (oob) VGX_LOG_N(16, "vertex array read out of range: attr %d stride %u size %u\n", a.attr, ar.stride, ar.size);
        if (indexed && spans && minIdx <= maxIdx) {
            spans->mask |= 1u << a.attr;
            spans->lo[a.attr - A_POS] = minIdx * astride;
            spans->hi[a.attr - A_POS] = maxIdx * astride + elem;
        }
    }
}

// ------------------------------------------------------- GPU skinning (W5 SKIN, vgx_ext.h)
namespace {
uint64_t g_skinMissed = 0; // draws made while a skin state was on that could not carry it (position not indexed)
}
bool SkinPosIndexed(const Plan& p) {
    for (int i = 0; i < p.n; ++i)
        if (p.a[i].attr == A_POS) return p.a[i].mode == T_INDEX8 || p.a[i].mode == T_INDEX16;
    return false;
}
void SkinNoteMissed(uint32_t count) {
    if (g_skinMissed++ < 8) VGX_LOG("GPU skinning: a draw of %u vertices under a skin state has no indexed position: drawn without skinning\n", count);
}
// Decoded vertices of layout p.layout at `v` (count of them) become layout p.layoutMask | VL_SKIN in place (the buffer must hold count x the wider stride):
// each vertex moves to its new place, from the last one down, and gets its bone rows and weights, looked up by its position index in the stream bytes
// `src`. The game's CPU loop: for each bone below nBones, for each vertex, the slots up to the first zero weight whose bone is that bone; so a slot counts
// when it and every slot before it have a weight and its bone is below nBones. The counting slots are sorted by bone number (the CPU sums bone by bone,
// in that order) and the others get weight 0 and bone 0.
void SkinExpand(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* v) {
    uint32_t posOff = 0;
    int mode = T_NONE;
    for (int i = 0; i < p.n; ++i) {
        if (p.a[i].attr == A_POS) { mode = p.a[i].mode; break; }
        posOff += p.a[i].srcSize;
    }
    const VtxLayout L1 = MakeLayout(p.layoutMask | VL_SKIN);
    const uint32_t s0 = p.layout.stride, s1 = L1.stride, ss = p.streamSize;
    const SkinState& sk = g_skin;
    for (uint32_t k = count; k-- > 0;) {
        uint8_t* d = v + static_cast<size_t>(k) * s1;
        std::memmove(d, v + static_cast<size_t>(k) * s0, s0);
        const uint8_t* e = src + static_cast<size_t>(k) * ss + posOff;
        const uint32_t idx = mode == T_INDEX16 ? be16(e) : e[0];
        uint8_t bone[4] = {0, 0, 0, 0};
        float w[4] = {0, 0, 0, 0};
        int n = 0;
        if (idx < sk.nVerts) {
            const uint8_t* ws = sk.weights + static_cast<size_t>(idx) * 16u;
            const uint8_t* is = sk.indices + static_cast<size_t>(idx) * 4u;
            for (int j = 0; j < 4; ++j) {
                const float wj = bef32(ws + 4 * j);
                if (wj == 0.0f) break;
                if (is[j] >= sk.nBones) continue;
                int at = n++;
                while (at > 0 && bone[at - 1] > is[j]) { bone[at] = bone[at - 1]; w[at] = w[at - 1]; --at; }
                bone[at] = is[j];
                w[at] = wj;
            }
        }
        uint8_t* o = d + L1.offSkin;
        for (int j = 0; j < 4; ++j) o[j] = static_cast<uint8_t>(bone[j] * 3u);
        std::memcpy(o + 4, w, 16);
    }
}

void DrawStream(uint8_t prim, int fmt, const uint8_t* src, uint32_t count, uint32_t avail) {
    const Plan& p = GetPlan(fmt);
    if (count == 0 || p.streamSize == 0) return;
    if (static_cast<uint64_t>(count) * p.streamSize > avail) {
        VGX_LOG_N(16, "draw truncated: %u verts x %u bytes > %u available\n", count, p.streamSize, avail);
        count = avail / p.streamSize;
        if (count == 0) return;
    }
    const bool skin = g_skin.on && SkinPosIndexed(p);
    if (g_skin.on && !skin) SkinNoteMissed(count);
    const uint32_t mask = skin ? (p.layoutMask | VL_SKIN) : p.layoutMask;
    uint8_t* out = BeginVertices(mask, count);
    if (!out) return;
    DecodeVertices(p, src, count, out, nullptr);
    if (skin) SkinExpand(p, src, count, out);
    SetPendingPosMtxMax(g_lastPosMtxMax);
    EmitDraw(prim, mask, count);
}

void DrawDecoded(uint8_t prim, uint32_t layoutMask, const uint8_t* verts, uint32_t count) {
    if (count == 0) return;
    if (g_skin.on) SkinNoteMissed(count);
    uint8_t* out = BeginVertices(layoutMask, count);
    std::memcpy(out, verts, static_cast<size_t>(count) * MakeLayout(layoutMask).stride);
    EmitDraw(prim, layoutMask, count);
}

// ------------------------------------------------------- command streams
void ProcessStream(const uint8_t* p, uint32_t nbytes) {
    const uint8_t* const end = p + nbytes;
    ++stats::Cur().streams;
    stats::Cur().streamBytes += nbytes;
    // MSC_VGX_DLTRACE=<frame>: one line per command stream of that frame.
    static const long dlTrace = EnvInt("MSC_VGX_DLTRACE", -1);
    const bool traceThis = dlTrace >= 0 && static_cast<uint64_t>(dlTrace) == CurrentFrame();
    const uint8_t* const start = p;
    const uint32_t verts0 = stats::Cur().vertsDL, prims0 = stats::Cur().primsDL;
    uint32_t stateCmds = 0;
    struct TraceOut {
        bool on; const uint8_t* start; uint32_t nbytes, verts0, prims0; uint32_t* stateCmds;
        ~TraceOut() {
            if (!on) return;
            uint32_t idxMask = 0;
            for (int a = 9; a <= 20; ++a) if (g.vcd[a] >= 2) idxMask |= 1u << a;
            std::fprintf(stderr, "[dl] %p %u bytes: %u prims, %u verts, %u state cmds, indexed attrs 0x%X, pos array %p stride %u, pnmtxidx %u\n", (const void*)start, nbytes,
                         stats::Cur().primsDL - prims0, stats::Cur().vertsDL - verts0, *stateCmds, idxMask, (const void*)g.arrays[9].data, g.arrays[9].stride, g.vcd[0]);
        }
    } traceOut{traceThis, start, nbytes, verts0, prims0, &stateCmds};
    while (p < end) {
        const uint8_t cmd = *p++;
        if (cmd == 0x00) continue;
        const uint8_t op = cmd & 0xF8;
        if (cmd == 0x61) {
            if (end - p < 4) break;
            WriteBP(be32(p));
            p += 4;
            ++stateCmds;
        } else if (op == 0x08) {
            if (end - p < 5) break;
            WriteCP(p[0], be32(p + 1));
            p += 5;
            ++stateCmds;
        } else if (op == 0x10) {
            if (end - p < 4) break;
            const uint32_t n = static_cast<uint32_t>(be16(p)) + 1u;
            const uint32_t addr = be16(p + 2);
            p += 4;
            if (static_cast<uint32_t>(end - p) < n * 4u) break;
            WriteXFBE(addr, n, p);
            p += n * 4u;
            ++stateCmds;
        } else if (op >= 0x20 && op <= 0x38) {
            if (end - p < 4) break;
            const uint32_t index = be16(p);
            const uint32_t v = be16(p + 2);
            p += 4;
            const uint32_t n = (v >> 12) + 1u, addr = v & 0xFFF;
            const int attr = A_POSMTXARRAY + ((op - 0x20) >> 3);
            bool le = false;
            const uint8_t* src = ArrayElem(attr, index, n * 4u, le);
            if (src != kZero) WriteXFBE(addr, n, src);
            ++stateCmds;
        } else if (op == 0x40) {
            // Nested display lists carry guest addresses; the runtime flattens them before calling us.
            VGX_LOG_N(8, "nested CALL_DL inside a command stream ignored\n");
            if (end - p < 8) break;
            p += 8;
        } else if (op == 0x48) {
            continue; // invalidate vertex cache
        } else if (cmd & 0x80) {
            if (end - p < 2) break;
            const uint32_t count = be16(p);
            p += 2;
            const uint32_t vsize = StreamVertexSize(cmd & 7);
            const uint32_t avail = static_cast<uint32_t>(end - p);
            if (vsize == 0) break;
            DrawStream(op, cmd & 7, p, count, avail);
            ++stats::Cur().primsDL;
            stats::Cur().vertsDL += count;
            const uint64_t used = static_cast<uint64_t>(count) * vsize;
            if (used > avail) break;
            p += used;
        } else {
            VGX_LOG_N(16, "unknown GX opcode 0x%02X in command stream (%u bytes left)\n", cmd, static_cast<unsigned>(end - p));
            break;
        }
    }
}

} // namespace vgx
