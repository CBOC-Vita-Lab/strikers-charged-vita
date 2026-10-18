// dt_a: runs the RETAIL code (translation of main.dol) over a synthetic state. x86-64, big-endian guest memory.
#include <string>
#include <string_view>
#include <map>
#include <vector>
#include "dt_common.h"
#include "abi_bridge.h"
#include "dt_cases.h"
#include "dt_desire.h"

uint8_t* g_dt_guest;
void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept
{
    fprintf(stderr, "DT fatal: %.*s: %.*s\n", (int)category.size(), category.data(), (int)details.size(), details.data());
    abort();
}

using namespace dt;

// ---- policy: guest memory ---------------------------------------------------------------------------------
struct PolicyA {
    typedef uint32_t H;
    static inline uint32_t bump = 0x80800000u;
    static inline IdMap ids;
    static inline uint32_t gBall, gGame;
    static H alloc(size_t n) { H h = bump; bump += (uint32_t)((n + 31) & ~31u); if (bump > 0x81000000u) abort(); memset(dtmem::P(h, (uint32_t)n), 0, n); return h; }
    static void w32(H a, uint32_t v) { MemoryInline::FlatWrite32(a, v); }
    static void wf(H a, float f) { MemoryInline::FlatWriteFloat32(a, f); }
    static uint32_t r32(H a) { return MemoryInline::FlatRead32(a); }
    static float rf(H a) { return MemoryInline::FlatReadFloat32(a); }
    static uint32_t ptr(H h) { return h; }
    static void reg(H h, int id) { ids.put(h, id); }
    static int id(uint32_t h) { if (h == 0) return -1; int v = ids.get(h); if (v == -9999) { fprintf(stderr, "DT: unknown object %08x\n", h); abort(); } return v; }
    static void setGlobals(H ball, H game, H t0, H t1) { gBall = ball; gGame = game; MemoryInline::FlatWrite32(0x806E0BC0u, ball); MemoryInline::FlatWrite32(0x806E0C94u, game); MemoryInline::FlatWrite32(0x806E0DF8u, t0); MemoryInline::FlatWrite32(0x806E0DFCu, t1); }
};
static DtWorld<PolicyA> W;
static Desc g_desc;

#include "gen_fns2.inc"

// ---- thunks for the helper functions (arguments from the PPC registers) ------------------------------------
typedef CpuContext C;
static inline uint32_t R(C* c, int i) { return c->gpr[i]; }
static inline float F(C* c, int i) { return (float)c->fpr[i].d; }
static inline void RetF(C* c, float f) { c->fpr[1].d = (double)f; }
static inline void RetR(C* c, uint32_t v) { c->gpr[3] = v; }
static inline V3 V(uint32_t a) { return { PolicyA::rf(a), PolicyA::rf(a + 4), PolicyA::rf(a + 8) }; }
static inline void WV(uint32_t a, const V3& v) { PolicyA::wf(a, v.x); PolicyA::wf(a + 4, v.y); PolicyA::wf(a + 8, v.z); }

static void th_GetOtherTeam(C* c) { int t = PolicyA::id(R(c, 3)); RetR(c, W.teams[1 - (t - 0x100)]); }
static void th_GetGoalie(C* c) { int t = PolicyA::id(R(c, 3)) - 0x100; RetR(c, W.players[t * 5 + 4]); }
static void th_GetAIOffNetLocation(C* c)
{
    int p = PolicyA::id(R(c, 4));
    WV(R(c, 3), so_OffNet(p, R(c, 5) != 0, R(c, 5) ? V(R(c, 5)) : V3{ 0, 0, 0 }));
}
static void th_fn_800DAFCC(C* c)
{
    RetF(c, so_OpenLane(V(R(c, 3)), V(R(c, 4)), PolicyA::id(R(c, 5)), PolicyA::id(R(c, 6)), F(c, 1), F(c, 2), F(c, 3), F(c, 4)));
}
static void th_PositionOpenness(C* c)
{
    int team = PolicyA::id(R(c, 4)) - 0x100;
    bool hasR = R(c, 6) != 0;
    float r0 = 0, r1 = 0;
    if (hasR) { r0 = PolicyA::rf(R(c, 6)); r1 = PolicyA::rf(R(c, 6) + 4); }
    RetF(c, so_PositionOpenness(V(R(c, 3)), team, PolicyA::id(R(c, 5)), hasR, r0, r1, (R(c, 7) & 0xFF) != 0, F(c, 1)));
}
static void th_WidePositionOpenness(C* c)
{
    RetF(c, so_WidePositionOpenness(V(R(c, 3)), PolicyA::id(R(c, 4)) - 0x100, PolicyA::id(R(c, 5)), (R(c, 6) & 0xFF) != 0, F(c, 1)));
}
static void th_NearToSideline(C* c) { RetF(c, so_NearToSideline(V(R(c, 3)))); }
static void th_fn_800DCB4C(C* c) { RetF(c, so_fn_800DCB4C(V(R(c, 3)), V(R(c, 4)))); }
static void th_PositionIsInFrontOfNet(C* c) { RetF(c, so_PositionIsInFrontOfNet(V(R(c, 3)), PolicyA::id(R(c, 4)) - 0x200)); }
static void th_InOffensiveZone(C* c) { RetF(c, so_InOffensiveZone(V(R(c, 3)), (int)R(c, 4))); }
static void th_PositionShotDistance(C* c) { RetF(c, so_PositionShotDistance(V(R(c, 3)), V(R(c, 4)), F(c, 1))); }
static void th_PositionDistanceConfidence(C* c) { RetF(c, so_PositionDistanceConfidence(V(R(c, 3)), V(R(c, 4)), F(c, 1), F(c, 2))); }
static void th_CloseToSideline(C* c)
{
    bool hasC = R(c, 4) != 0;
    float c0 = 0, c1 = 0;
    if (hasC) { c0 = PolicyA::rf(R(c, 4)); c1 = PolicyA::rf(R(c, 4) + 4); }
    RetF(c, so_CloseToSideline(V(R(c, 3)), hasC, c0, c1, (R(c, 5) & 0xFF) != 0));
}
static void th_NearToFormationPosition(C* c) { RetF(c, so_NearToFormation(PolicyA::id(R(c, 3)), V(R(c, 4)))); }
static void th_CalculateFormationPosition(C* c)
{
    int p = PolicyA::id(R(c, 3));
    WV(R(c, 4), so_FormationPos(p));
    RetR(c, so_FormationFlag(p) ? 1 : 0);
}
static void th_GetRunningSpeed(C* c) { RetF(c, g_desc.p[PolicyA::id(R(c, 3))].run); }
static void th_GetTweaks(C* c) { RetR(c, W.ptw[PolicyA::id(R(c, 3))]); }
static void th_GetJogSpeed(C* c) { RetF(c, g_desc.p[PolicyA::id(R(c, 3)) - 0x300].jog); }
static void th_GetGoalLineX(C* c) { RetF(c, so_GoalLineX(F(c, 1))); }
static void th_FixOutOfBounds(C* c) { V3 q = so_FixOutOfBounds(V(R(c, 3)), F(c, 1)); WV(R(c, 3), q); RetR(c, 0); }
static void th_nlSqrt(C* c) { RetF(c, sqrtf(F(c, 1))); }
static void th_nlRecipSqrt(C* c) { RetF(c, 1.0f / sqrtf(F(c, 1))); }
static void th_nlATan2f(C* c) { RetF(c, atan2f(F(c, 1), F(c, 2))); }
static void th_nlPolarToCartesian(C* c)
{
    uint32_t o = R(c, 3), p = R(c, 4);
    uint16_t ang = MemoryInline::FlatRead16(p);
    float r = PolicyA::rf(p + 4);
    float a = ang * (6.2831853f / 65536.0f);
    PolicyA::wf(o, cosf(a) * r); PolicyA::wf(o + 4, sinf(a) * r);
}

// ---- thunks for the Desire tests ----------------------------------------------------------------------------
static uint32_t gAIctx, gSkillTweaks, gFakeDesire;
static void th_AddDesiredPosition(C* c) { V3 p = V(R(c, 4)); lg(L_ADD, (float)PolicyA::id(R(c, 3)), p.x, p.y, p.z, F(c, 1), F(c, 2)); }
static void th_GetDistanceToDesiredPos(C* c) { int p = PolicyA::id(R(c, 3)); lg(L_DIST, (float)p); RetF(c, so_DistToDesired(p, g_desc.salt)); }
static void th_SetAvoidanceMultiplier(C* c) { lg(L_AVOID, (float)PolicyA::id(R(c, 3)), F(c, 1)); }
static void th_GetFielderDesire(C* c) { lg(L_GETDESIRE, (float)PolicyA::id(R(c, 3)), (float)(int)R(c, 4)); RetR(c, gFakeDesire); }
static void th_ResetSteeringHistory(C* c) { lg(L_RESETSTEER); }
static void th_IsStuck(C* c) { RetR(c, g_desc.stuck[PolicyA::id(R(c, 3))] ? 1 : 0); }
static void th_IsSuperGrowActive(C* c) { RetR(c, g_desc.grow[PolicyA::id(R(c, 3))] ? 1 : 0); }
static void th_GetFielderHitReach(C* c) { RetF(c, g_desc.reach[PolicyA::id(R(c, 3))]); }
static void th_SetSpaceSearch(C* c) { MemoryInline::FlatWrite32(R(c, 3) + P_SPACESEARCH, R(c, 4)); lg(L_SETSEARCH, (float)PolicyA::id(R(c, 3))); }
static void th_nlMalloc(C* c) { lg(L_MALLOC, (float)R(c, 3)); RetR(c, PolicyA::alloc(R(c, 3))); }
static void th_fn_800DF790(C* c) { int p = so_StrategicBallOwner(PolicyA::id(R(c, 3)) - 0x100, g_desc.salt); RetR(c, p < 0 ? 0 : W.players[p]); }
static void th_fn_800DFF1C(C* c) { RetF(c, so_PassInFlight(g_desc.ballState, g_desc.ballPassTarget)); }
static void th_nlStringHash(C* c) { RetR(c, so_hash(dtmem::P(R(c, 3), 1))); }
static void th_GetSkillValue(C* c)
{
    float v = so_skill(R(c, 4), PolicyA::id(R(c, 6)));
    lg(L_SKILL, (float)(R(c, 4) & 0xFFFF), (float)PolicyA::id(R(c, 6)), v);
    PolicyA::wf(R(c, 5), v); RetR(c, 1);
}
static void th_nlRandomf(C* c) { RetF(c, so_random(F(c, 1))); }
static void th_IsTimerRunning(C* c) { int k = (int)R(c, 4); lg(L_ISTIMER, (float)k, g_timer[k] ? 1.0f : 0.0f); RetR(c, g_timer[k] ? 1 : 0); }
static void th_SetTimer(C* c) { int k = (int)R(c, 4); lg(L_SETTIMER, (float)k, F(c, 1)); g_timer[k] = F(c, 1) > 0.0f; RetR(c, 0); }
static void th_noop(C*) {}
static void th_GetRadius(C* c) { RetF(c, g_desc.radius[PolicyA::id(R(c, 3)) - 0x600]); }


// ---- thunks for the InterceptBall::Update test ----------------------------------------------------------------
static uint32_t gAvoidCtl[10];
static void th_DAD3C(C* c) { RetF(c, so_BallQ(g_desc.salt)); }
static void th_DF0B8(C* c) { RetF(c, so_DF0B8(PolicyA::id(R(c, 3)), g_desc.salt)); }
static void th_D7B00(C* c) { RetF(c, so_D7B00(PolicyA::id(R(c, 3)), g_desc.salt)); }
static void th_OpenTo(C* c) { RetF(c, so_OpenTo(PolicyA::id(R(c, 3)), PolicyA::id(R(c, 4)), g_desc.salt)); }
static void th_AirHeight(C* c) { int p = PolicyA::id(R(c, 3)); int t = (int)R(c, 4); lg(L_AIRH, (float)p, (float)t); RetF(c, so_AirHeight(p, t)); }
static void th_Predict(C* c) { V3 b = V(R(c, 3) + B_POS); lg(L_PREDICT, F(c, 1), b.x, b.y, b.z); WV(R(c, 4), so_Predict(b, F(c, 1))); }
static void th_Approach(C* c)
{
    int p = PolicyA::id(R(c, 3)); V3 from = V(R(c, 5)); lg(L_APPROACH, (float)p, from.x, from.y, from.z, F(c, 1));
    WV(R(c, 4), so_Approach(p, from, F(c, 1)));
}
static void th_Shot(C* c) { lg(L_SHOT, (float)PolicyA::id(R(c, 3)), (float)(R(c, 4) & 0xFF)); }
static void th_PassAct(C* c) { lg(L_PASSACT, (float)PolicyA::id(R(c, 3)), (float)PolicyA::id(R(c, 4)), (float)(R(c, 5) & 0xFF)); }
static void th_GetAvoidController(C* c) { RetR(c, gAvoidCtl[PolicyA::id(R(c, 3))]); }
static void th_Set(C* c)   // UnidentifiedVariantCollection::Set(index, FuzzyVariant): a copy of type + data in a fresh data object
{
    uint32_t coll = R(c, 3); int idx = (int)R(c, 4); uint32_t v = R(c, 5);
    uint32_t slot = PolicyA::r32(coll + 4 * idx);
    if (!slot) { slot = PolicyA::alloc(FVD_SIZE); PolicyA::w32(coll + 4 * idx, slot); }
    PolicyA::w32(slot, 0x80503F7Cu);
    for (int k = 4; k < 20; k += 4) PolicyA::w32(slot + k, PolicyA::r32(v + k));
    PolicyA::w32(slot + FV_INDEX, idx);
}

static void th_abort(C*) { fprintf(stderr, "DT: unexpected call to an unmodelled helper\n"); abort(); }

static std::map<uint32_t, void (*)(C*)> g_table;
void dt_dispatch(uint32_t target, CpuContext* ctx)
{
    auto it = g_table.find(target);
    if (it == g_table.end()) { fprintf(stderr, "DT: call to unmodelled function %08x\n", target); abort(); }
    it->second(ctx);
}

static void initTable()
{
    for (auto& e : g_translated) g_table[e.a] = e.f;
    g_table[0x800A6A94] = th_GetOtherTeam;          g_table[0x800A6954] = th_GetGoalie;
    g_table[0x80098060] = th_GetAIOffNetLocation;   g_table[0x800DAFCC] = th_fn_800DAFCC;
    g_table[0x800DB678] = th_PositionOpenness;      g_table[0x800DBA68] = th_WidePositionOpenness;
    g_table[0x800DD1C0] = th_NearToSideline;        g_table[0x800DCB4C] = th_fn_800DCB4C;
    g_table[0x800DA350] = th_PositionIsInFrontOfNet; g_table[0x800E035C] = th_InOffensiveZone;
    g_table[0x800DDAC0] = th_PositionShotDistance;  g_table[0x800DD3DC] = th_PositionDistanceConfidence;
    g_table[0x800DCFC0] = th_CloseToSideline;       g_table[0x800DA7A8] = th_NearToFormationPosition;
    g_table[0x800381B4] = th_CalculateFormationPosition;
    g_table[0x8002E1B0] = th_GetRunningSpeed;       g_table[0x8003E6E4] = th_GetTweaks;
    g_table[0x8002CE14] = th_GetJogSpeed;           g_table[0x8002D148] = th_GetGoalLineX;
    g_table[0x8002D2C4] = th_FixOutOfBounds;
    g_table[0x802A9B98] = th_nlSqrt;                g_table[0x802A9C58] = th_nlRecipSqrt;
    g_table[0x802AA254] = th_nlATan2f;              g_table[0x802B5EE8] = th_nlPolarToCartesian;
    g_table[0x802BCC88] = th_abort;
    g_table[0x800401C0] = th_AddDesiredPosition;    g_table[0x8004028C] = th_GetDistanceToDesiredPos;
    g_table[0x800305DC] = th_SetAvoidanceMultiplier; g_table[0x8002E08C] = th_GetFielderDesire;
    g_table[0x800C574C] = th_ResetSteeringHistory;  g_table[0x8003886C] = th_IsStuck;
    g_table[0x8003E74C] = th_IsSuperGrowActive;     g_table[0x80030750] = th_GetFielderHitReach;
    g_table[0x80095784] = th_SetSpaceSearch;        g_table[0x802AA79C] = th_nlMalloc;
    g_table[0x800DF790] = th_fn_800DF790;           g_table[0x800DFF1C] = th_fn_800DFF1C;
    g_table[0x802AAACC] = th_nlStringHash;          g_table[0x800A3350] = th_GetSkillValue;
    g_table[0x80332ED4] = th_nlRandomf;             g_table[0x8030FB7C] = th_IsTimerRunning;
    g_table[0x8030FA10] = th_SetTimer;              g_table[0x8030EE04] = th_noop;
    g_table[0x80042118] = th_abort; g_table[0x802B4710] = th_abort; g_table[0x8030EEB8] = th_abort; g_table[0x802AA654] = th_abort;
    g_table[0x80FFF100] = th_GetRadius;
    g_table[0x800DAD3C] = th_DAD3C;                 g_table[0x800DF0B8] = th_DF0B8;
    g_table[0x800D7B00] = th_D7B00;                 g_table[0x800DC6DC] = th_OpenTo;
    g_table[0x800394A8] = th_AirHeight;             g_table[0x800180F4] = th_Predict;
    g_table[0x80039350] = th_Approach;              g_table[0x80048618] = th_Shot;
    g_table[0x80048338] = th_PassAct;               g_table[0x8002E144] = th_GetAvoidController;
    g_table[0x8030F0A8] = th_Set;
}

// ---- DOL loader -------------------------------------------------------------------------------------------
static void loadDol(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    std::vector<uint8_t> d;
    uint8_t buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    auto be = [&](size_t o) { return (uint32_t)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]); };
    for (int i = 0; i < 18; i++) {
        uint32_t off = be(i * 4), addr = be(0x48 + i * 4), size = be(0x90 + i * 4);
        if (size) memcpy(dtmem::P(addr, size), &d[off], size);
    }
}

// ---- the Impl for the test driver ---------------------------------------------------------------------------
static CpuContext g_ctx;
static CpuContext* newctx()
{
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.gpr[1] = 0x817FF000u; g_ctx.gpr[2] = 0x806EB060u; g_ctx.gpr[13] = 0x806E33C0u;
    return &g_ctx;
}
static uint32_t scratch(int i) { return 0x80F00000u + 0x40 * i; }

struct ImplA {
    typedef uint32_t H;
    void setup(const Desc& d) { PolicyA::bump = 0x80800000u; PolicyA::ids.clear(); g_desc = d; W.build(d); }
    H pl(int i) { return i < 0 ? 0 : W.players[i]; }
    H make(int kind, int owner, int target, float speed)
    {
        H obj = PolicyA::alloc(0x80);
        CpuContext* c = newctx();
        c->gpr[3] = obj;
        void (*fn)(CpuContext*) = nullptr;
        switch (kind) {
        case K_OL_SHOT: fn = func_800A4610; c->gpr[4] = pl(owner); c->gpr[5] = 0; break;
        case K_OL_PASS: fn = func_800A4610; c->gpr[4] = pl(owner); c->gpr[5] = pl(target); break;
        case K_OL_PASS_NOOWNER: fn = func_800A4610; c->gpr[4] = 0; c->gpr[5] = pl(target); break;
        case K_GO: fn = func_800A48DC; c->gpr[4] = pl(owner); break;
        case K_IS: case K_RN: fn = func_800A56A0; c->gpr[4] = pl(owner); break;
        case K_CB: fn = func_800A5A1C; c->gpr[4] = pl(owner); break;
        case K_BP_LEAD: case K_BP_PERFECT: case K_BP_PLAIN:
            fn = func_800A4F54; c->gpr[4] = pl(owner); c->gpr[5] = pl(target);
            c->gpr[6] = kind == K_BP_LEAD; c->gpr[7] = kind == K_BP_PERFECT; c->fpr[1].d = speed; break;
        }
        fn(c);
        return kind == K_IS ? obj + 0x10 : obj;
    }
    float eval(int kind, H obj, const V3& pos, const V3& center, int dir, int ang)
    {
        CpuContext* c = newctx();
        WV(scratch(0), pos); WV(scratch(1), center);
        c->gpr[3] = obj; c->gpr[4] = scratch(0); c->gpr[5] = scratch(1); c->gpr[6] = dir; c->gpr[7] = ang;
        void (*fn)(CpuContext*) = nullptr;
        switch (kind) {
        case K_OL_SHOT: case K_OL_PASS: case K_OL_PASS_NOOWNER: fn = func_800A4710; break;
        case K_GO: fn = func_800A4914; break;
        case K_IS: fn = func_800A4DB4; break;
        case K_RN: fn = func_800A580C; break;
        case K_CB: fn = func_800A5A54; break;
        default: fn = func_800A5048; break;
        }
        fn(c);
        return (float)c->fpr[1].d;
    }
    float findBest(int kind, H obj, V3& dest, const V3& center, int dir, const V3& tgt, float radius, int cone)
    {
        CpuContext* c = newctx();
        WV(scratch(0), dest); WV(scratch(1), center); WV(scratch(2), tgt);
        c->gpr[3] = obj; c->gpr[4] = scratch(0); c->gpr[5] = scratch(1); c->gpr[6] = dir;
        c->gpr[7] = (dir == 3 || dir == 4 || dir == 5) ? scratch(2) : 0;
        c->fpr[1].d = radius; c->gpr[8] = cone;
        func_800A401C(c);
        dest = V(scratch(0));
        return (float)c->fpr[1].d;
    }
};


// ---- Desire tests: Impl methods (declared as an extension of ImplA via a derived struct) ----------------------
static uint32_t mkVariant(uint32_t vptr, int type, uint32_t a, uint32_t b = 0, uint32_t c = 0, int index = -1)
{
    uint32_t h = PolicyA::alloc(FVD_SIZE);
    PolicyA::w32(h, vptr); PolicyA::w32(h + 4, type); PolicyA::w32(h + 8, a); PolicyA::w32(h + 12, b); PolicyA::w32(h + 16, c);
    PolicyA::w32(h + FV_INDEX, index);
    return h;
}
static uint32_t mkUpdate(int init)
{
    uint32_t h = PolicyA::alloc(V_SIZE);
    PolicyA::w32(h, 0x804FA310u); PolicyA::w32(h + V_TYPE, 3); PolicyA::w32(h + V_DATA, init); PolicyA::w32(h + V_TEMP, 0x01000000u);  // mTemporary = 1
    return h;
}
static void outUpdate(uint32_t h)
{
    g_out.add((float)(int)PolicyA::r32(h + V_TYPE)); g_out.add((float)(int)PolicyA::r32(h + V_DATA));
    g_out.add((float)(MemoryInline::FlatRead8(h + V_TEMP)));
}
static uint32_t mkDesire(uint32_t vptr, int fielder)
{
    uint32_t h = PolicyA::alloc(D_SIZE);
    uint32_t sm = PolicyA::alloc(0x80);
    PolicyA::w32(sm + SM_AICTX, gAIctx);
    PolicyA::w32(h, vptr); PolicyA::w32(h + D_SM, sm); PolicyA::w32(h + D_FIELDER, W.players[fielder]);
    return h;
}
static void outV3(uint32_t a) { g_out.add(PolicyA::rf(a)); g_out.add(PolicyA::rf(a + 4)); g_out.add(PolicyA::rf(a + 8)); }

struct ImplA2 : ImplA {
    void setup(const Desc& d)
    {
        ImplA::setup(d);
        gAIctx = PolicyA::alloc(0x40); PolicyA::reg(gAIctx, 0x700);
        gSkillTweaks = PolicyA::alloc(0x40); PolicyA::reg(gSkillTweaks, 0x720);
        gFakeDesire = PolicyA::alloc(0x40); PolicyA::reg(gFakeDesire, 0x710);
        for (int s = 0; s < 2; s++) PolicyA::w32(0x8056CF08u + 24 + 4 * s, gSkillTweaks);
        // avoidable objects: vptr -> fake vtable at 0x80FFF000 whose slot 3 (offset 8 + 4*3) is the thunk 0x80FFF100
        PolicyA::w32(0x80FFF000u + 8 + 12, 0x80FFF100u);
        for (int i = 0; i < 10; i++) {
            uint32_t av = PolicyA::alloc(0x20); PolicyA::w32(av, 0x80FFF000u); PolicyA::reg(av, 0x600 + i);
            PolicyA::w32(W.players[i] + P_AVOID, av);
        }
        so_seedRandom(d.salt);
        PolicyA::wf(0x806E0FACu, 0.016666668f);   // g_fSimulationTick
        for (int i = 0; i < 10; i++) { gAvoidCtl[i] = PolicyA::alloc(0x40); PolicyA::reg(gAvoidCtl[i], 0x800 + i); }
        for (int i = 0; i < 8; i++) g_timer[i] = false;
    }
    void desireInterceptUpdate(const Desc& d)
    {
        uint32_t desire = mkDesire(0x80501AA8u, d.fielder);
        PolicyA::w32(desire + 164, 0); MemoryInline::FlatWrite8(desire + 168, d.icPass ? 1 : 0);
        // Offensive(team) is true when team+24 == 0 (the retail leaf is inlined by the translation); BallOwner(player) when player+784 != 0
        PolicyA::w32(W.teams[d.p[d.fielder].team] + 24, ((d.salt >> 4) % 4 == 0) ? 0 : 1);
        PolicyA::w32(W.players[d.fielder] + P_BALL, ((d.salt >> 3) % 5 == 0) ? W.ball : 0);
        int init = d.icMode ? 3 : d.updateInit;
        uint32_t upd = mkUpdate(init);
        auto setx = [&](int idx, uint32_t h) { PolicyA::w32(upd + V_EXTRA + 4 * idx, h); };
        const uint32_t FV = 0x80503F7Cu;
        if (init == 3) switch (d.icMode) {
        case 1: setx(8, mkVariant(FV, 3, 15, 0, 0, 8)); setx(16, mkVariant(FV, 2, d.icChip ? 0x01000000u : 0u, 0, 0, 16)); break;   // bool = first byte (big endian)
        case 2: setx(8, mkVariant(FV, 3, 14, 0, 0, 8)); setx(14, mkVariant(FV, 9, W.players[d.icTarget], 0, 0, 14)); break;
        case 3: setx(8, mkVariant(FV, 3, 7, 0, 0, 8)); break;
        }
        CpuContext* c = newctx(); c->gpr[3] = desire; c->gpr[4] = upd; c->fpr[1].d = 0.02;
        func_800B5C44(c);
        outUpdate(upd);
        uint32_t e8 = PolicyA::r32(upd + V_EXTRA + 4 * 8);
        g_out.add(e8 ? 1.0f : 0.0f);
        if (e8) { g_out.add((float)(int)PolicyA::r32(e8 + 4)); g_out.add((float)(int)PolicyA::r32(e8 + 8)); }
        g_out.add(PolicyA::rf(gAvoidCtl[d.fielder] + 0x10));
    }
    void desireCut(const Desc& d)
    {
        uint32_t desire = mkDesire(0x80501738u, d.fielder);
        for (int k = 0; k < 3; k++) PolicyA::wf(desire + D_DESIRED + 4 * k, (&d.desired.x)[k]);
        uint32_t upd = mkUpdate(d.updateInit);
        CpuContext* c = newctx(); c->gpr[3] = desire; c->gpr[4] = upd; c->fpr[1].d = 0.02;
        func_800B09DC(c);
        outUpdate(upd);
    }
    void desireGetOpen(const Desc& d)
    {
        uint32_t desire = mkDesire(0x80501A18u, d.fielder);
        MemoryInline::FlatWrite8(0x806E0E28u, d.debugFlag ? 1 : 0);
        CpuContext* c = newctx(); c->gpr[3] = desire; c->gpr[4] = 0;
        func_800B4B58(c);
        g_out.add((float)(c->gpr[3] & 0xFF));
        outV3(desire + D_DESIRED);
        g_out.add(PolicyA::r32(desire + D_A4) != 0 ? 1.0f : 0.0f);
        uint32_t ss = PolicyA::r32(W.players[d.fielder] + P_SPACESEARCH);
        g_out.add((float)MemoryInline::FlatRead8(ss + 8));
        g_out.add(PolicyA::r32(ss) == 0x80500910u ? 1.0f : (PolicyA::r32(ss) == 0x80500920u ? 2.0f : 0.0f));
    }
    void setRunFields(uint32_t desire, const Desc& d)
    {
        PolicyA::w32(desire + R_TFIELDER, d.targetKind == 1 ? W.players[d.targetFielder] : 0);
        PolicyA::w32(desire + R_TBALL, d.targetKind == 2 ? W.ball : 0);
        for (int k = 0; k < 3; k++) PolicyA::wf(desire + R_TPOS + 4 * k, (&d.targetPos.x)[k]);
        PolicyA::w32(desire + R_DIR, d.direction);
        PolicyA::wf(desire + R_DISTOFF, d.distOff); PolicyA::wf(desire + R_URGENCY, d.urgency);
        PolicyA::wf(desire + R_SPEED, d.speedCoeff); PolicyA::wf(desire + R_AVOID, d.avoidCoeff);
    }
    void outRunFields(uint32_t desire)
    {
        g_out.add((float)PolicyA::id(PolicyA::r32(desire + R_TFIELDER))); g_out.add(PolicyA::r32(desire + R_TBALL) != 0 ? 1.0f : 0.0f);
        outV3(desire + R_TPOS);
        g_out.add((float)(int)PolicyA::r32(desire + R_DIR));
        g_out.add(PolicyA::rf(desire + R_DISTOFF)); g_out.add(PolicyA::rf(desire + R_URGENCY));
        g_out.add(PolicyA::rf(desire + R_SPEED)); g_out.add(PolicyA::rf(desire + R_AVOID));
        outV3(desire + D_DESIRED);
    }
    void desireRunUpdate(const Desc& d)
    {
        uint32_t desire = mkDesire(0x80501800u, d.fielder);
        setRunFields(desire, d);
        g_timer[1] = d.timerRunning;
        uint32_t upd = mkUpdate(d.updateInit);
        CpuContext* c = newctx(); c->gpr[3] = desire; c->gpr[4] = upd; c->fpr[1].d = 0.02;
        func_800B3364(c);
        outUpdate(upd);
        outRunFields(desire);
    }
    uint32_t mkParams(const Desc& d)
    {
        uint32_t coll = PolicyA::alloc(19 * 4);
        auto setv = [&](int idx, uint32_t h) { PolicyA::w32(coll + 4 * idx, h); };
        if (d.paramMask & 1) setv(17, mkVariant(0x80503F7Cu, 3, d.paramDirection, 0, 0, 17));
        if (d.paramMask & 2) { float f = d.paramSpeed; uint32_t b; memcpy(&b, &f, 4); setv(13, mkVariant(0x80503F7Cu, 5, b, 0, 0, 13)); }
        if (d.paramMask & 4) { float f = d.paramDist; uint32_t b; memcpy(&b, &f, 4); setv(18, mkVariant(0x80503F7Cu, 5, b, 0, 0, 18)); }
        if (d.paramMask & 8) { float f = d.paramUrgency; uint32_t b; memcpy(&b, &f, 4); setv(11, mkVariant(0x80503F7Cu, 5, b, 0, 0, 11)); }
        if (d.paramMask & 16) { float f = d.paramAvoid; uint32_t b; memcpy(&b, &f, 4); setv(2, mkVariant(0x80503F7Cu, 5, b, 0, 0, 2)); }
        auto fb = [](float f) { uint32_t b; memcpy(&b, &f, 4); return b; };
        switch (d.paramTarget) {
        case 1: setv(14, mkVariant(0x80503F7Cu, 9, W.players[d.targetFielder], 0, 0, 14)); break;
        case 2: setv(14, mkVariant(0x80503F7Cu, 12, W.ball, 0, 0, 14)); break;
        case 3: setv(14, mkVariant(0x80503F7Cu, 6, fb(d.targetPos.x), fb(d.targetPos.y), fb(d.targetPos.z), 14)); break;
        case 4: setv(0, mkVariant(0x80503F7Cu, 9, W.players[d.targetFielder], 0, 0, 0)); break;
        case 5: setv(14, mkVariant(0x80503F7Cu, 6, fb(d.targetPos.x), fb(d.targetPos.y), fb(d.targetPos.z), 14));
                setv(0, mkVariant(0x80503F7Cu, 9, W.players[d.targetFielder], 0, 0, 0)); break;
        }
        return coll;
    }
    void desireRunInit(const Desc& d)
    {
        uint32_t desire = mkDesire(0x80501800u, d.fielder);
        g_timer[1] = d.timerRunning;
        uint32_t params = mkParams(d);
        CpuContext* c = newctx(); c->gpr[3] = desire; c->gpr[4] = params;
        func_800B3060(c);
        g_out.add((float)(c->gpr[3] & 0xFF));
        outRunFields(desire);
    }
    void desireIntercept(const Desc& d)
    {
        uint32_t desire = mkDesire(0x80501AA8u, d.fielder);
        g_desc.salt = d.salt;
        MemoryInline::FlatWrite32(0x806E0E00u, W.teams[d.p[d.fielder].team]);   // g_pCurrentlyUpdatingTeam
        CpuContext* c = newctx(); c->gpr[3] = desire; c->gpr[4] = 0;
        func_800B5B9C(c);
        g_out.add((float)(c->gpr[3] & 0xFF));
        g_out.add((float)(int)PolicyA::r32(desire + 164)); g_out.add((float)MemoryInline::FlatRead8(desire + 168));
    }
    void trans1(const Desc& d)
    {
        uint32_t desire = mkDesire(0x80501800u, d.fielder);
        for (int k = 0; k < 3; k++) PolicyA::wf(desire + D_DESIRED + 4 * k, (&d.desired.x)[k]);
        // make the fielder sometimes close to the desired position
        if (d.salt & 1) { float k = 0.63f + 0.2f * (float)((d.salt >> 2) & 7); PolicyA::wf(W.players[d.fielder] + P_POS, d.desired.x + 0.3f * k); PolicyA::wf(W.players[d.fielder] + P_POS + 4, d.desired.y - 0.4f * k); }
        uint32_t aictx = PolicyA::alloc(0x40); PolicyA::w32(aictx + 8, W.players[d.fielder]);
        uint32_t res = PolicyA::alloc(V_SIZE);
        CpuContext* c = newctx(); c->gpr[3] = res; c->gpr[4] = aictx; c->gpr[5] = desire;
        func_800B38AC(c);
        outUpdate(res);
    }
    void trans2(const Desc& d)
    {
        uint32_t aictx = PolicyA::alloc(0x40); PolicyA::w32(aictx + 8, W.players[d.fielder]);
        PolicyA::w32(W.players[d.fielder] + P_BALL, (d.salt & 2) ? W.ball : 0);   // explicit both ways: earlier tests leave state behind
        uint32_t res = PolicyA::alloc(V_SIZE);
        CpuContext* c = newctx(); c->gpr[3] = res; c->gpr[4] = aictx; c->gpr[5] = 0;
        func_800B4DC0(c);
        outUpdate(res);
    }
    void weather()
    {
        CpuContext* c = newctx(); func_800B0358(c);
        for (int tab = 0; tab < 2; tab++) {
            uint32_t pb = tab == 0 ? 0x8056DA00u : 0x8056DA30u, cb = tab == 0 ? 0x8056DA18u : 0x8056DA48u;
            for (int i = 0; i < 6; i++) {
                uint32_t p = PolicyA::r32(pb + 4 * i); int n = (int)PolicyA::r32(cb + 4 * i);
                g_out.add((float)n);
                for (int j = 0; j < n; j++) outV3(p + 12 * j);
            }
        }
        weather2(0);
    }
    void weather2(int tabs)
    {
        for (int i = 0; i < 4; i++)
            for (int side = 0; side < 2; side++) {
                uint32_t ret = PolicyA::alloc(0x10);
                CpuContext* c = newctx(); c->gpr[3] = ret; c->gpr[4] = 0; c->gpr[5] = i; c->gpr[6] = side;
                func_800B04BC(c);
                for (int k = 0; k < 4; k++) g_out.add(PolicyA::rf(ret + 4 * k));
            }
    }
};

int main(int argc, char** argv)
{
    g_dt_guest = (uint8_t*)calloc(1, 0x01800000);
    loadDol(argc > 1 ? argv[1] : "/home/claude/msc/game/sys/main.dol");
    initTable();
    int s0 = argc > 2 ? atoi(argv[2]) : 0, n = argc > 3 ? atoi(argv[3]) : 20;
    if (argc > 4 && !strcmp(argv[4], "desire")) { ImplA2 I; runDesire(I, s0, n); return 0; }
    ImplA I;
    runCases(I, s0, n);
    return 0;
}
