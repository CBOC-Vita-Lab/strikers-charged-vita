// dt_b: runs the NATIVE port code (src/Game/AI/SpaceSearch.cpp) over the same synthetic state. host32 (ILP32, little endian).
#include "Game/AI/SpaceSearch.h"
#include "Game/AI/Fielder.h"
#include "Game/Goalie.h"
#include "Game/Team.h"
#include "Game/Net.h"
#include "Game/Ball.h"
#include "Game/Game.h"
#include "Game/Field.h"
#include "Game/ScriptTuning.h"
#include "Game/CharacterTweaks.h"
#include "Game/AI/Scripts/ScriptQuestions.h"
#include "Game/AI/AiUtil.h"
#include "dt_common.h"
#include "dt_cases.h"
#include "dt_desire.h"
#include "Game/AI/Desire.h"
#include "Game/AI/DesireSteering.h"
#include "Game/AI/AvoidableObject.h"
#include "Game/AI/AIContext.h"
#include "Game/AI/SkillTweaks.h"
#include "Game/WeatherData.h"
#include "NL/nlString.h"

using namespace dt;

struct PolicyB {
    typedef uint32_t H;
    static inline IdMap ids;
    static H alloc(size_t n) { return (H)(uintptr_t)calloc(1, n < 32 ? 32 : n); }
    static void w32(H a, uint32_t v) { memcpy((void*)(uintptr_t)a, &v, 4); }
    static void wf(H a, float f) { memcpy((void*)(uintptr_t)a, &f, 4); }
    static uint32_t r32(H a) { uint32_t v; memcpy(&v, (void*)(uintptr_t)a, 4); return v; }
    static float rf(H a) { float f; memcpy(&f, (void*)(uintptr_t)a, 4); return f; }
    static uint32_t ptr(H h) { return h; }
    static void reg(H h, int id) { ids.put(h, id); }
    static int id(const void* h) { if (!h) return -1; int v = ids.get((uint32_t)(uintptr_t)h); if (v == -9999) { fprintf(stderr, "DT: unknown object %p\n", h); abort(); } return v; }
    static void setGlobals(H ball, H game, H t0, H t1);
};
static DtWorld<PolicyB> W;
static Desc g_desc;

cTeam* g_pTeams[2];
cBall* g_pBall;
cTeam* g_pCurrentlyUpdatingTeam;
cGame* g_pGame;
void PolicyB::setGlobals(H ball, H game, H t0, H t1)
{
    g_pBall = (cBall*)(uintptr_t)ball; g_pGame = (cGame*)(uintptr_t)game;
    g_pTeams[0] = (cTeam*)(uintptr_t)t0; g_pTeams[1] = (cTeam*)(uintptr_t)t1;
}

static V3 V(const nlVector3& v) { return { v.x, v.y, v.z }; }
static V3 V(const nlVector3* v) { return { v->x, v->y, v->z }; }
static int pid(const void* p) { return PolicyB::id(p); }
static int tid(const void* p) { return PolicyB::id(p) - 0x100; }

// ---- stand-ins (decomp signatures) ---------------------------------------------------------------------------
cTeam* cTeam::GetOtherTeam() { return (cTeam*)(uintptr_t)W.teams[1 - tid(this)]; }
Goalie* cTeam::GetGoalie() { return (Goalie*)(uintptr_t)W.players[tid(this) * 5 + 4]; }
nlVector3 cPlayer::GetAIOffNetLocation(const nlVector3* ref)
{
    V3 v = so_OffNet(pid(this), ref != 0, ref ? V(ref) : V3{ 0, 0, 0 });
    nlVector3 r; r.x = v.x; r.y = v.y; r.z = v.z; return r;
}
extern "C" float fn_800DAFCC(const nlVector3& a, const nlVector3& b, cPlayer* p1, cPlayer* p2, float w0, float w1, float w2, float t)
{
    return so_OpenLane(V(a), V(b), pid(p1), pid(p2), w0, w1, w2, t);
}
float PositionOpenness(const nlVector3& pos, cTeam* team, cPlayer* p, const nlVector2* r, bool b, float t)
{
    return so_PositionOpenness(V(pos), tid(team), pid(p), r != 0, r ? r->x : 0, r ? r->y : 0, b, t);
}
float WidePositionOpenness(const nlVector3& pos, cTeam* team, cPlayer* p, bool b, float t) { return so_WidePositionOpenness(V(pos), tid(team), pid(p), b, t); }
float NearToSideline(const nlVector3& p) { return so_NearToSideline(V(p)); }
extern "C" float fn_800DCB4C(const nlVector3* a, const nlVector3* b) { return so_fn_800DCB4C(V(a), V(b)); }
float PositionIsInFrontOfNet(const nlVector3& p, const cNet* net) { return so_PositionIsInFrontOfNet(V(p), PolicyB::id(net) - 0x200); }
float InOffensiveZone(const nlVector3& p, eTeamSide side) { return so_InOffensiveZone(V(p), (int)side); }
float PositionShotDistance(const nlVector3& p, const nlVector3& q, float s) { return so_PositionShotDistance(V(p), V(q), s); }
float PositionDistanceConfidence(const nlVector3& p, const nlVector3& q, float a, float b) { return so_PositionDistanceConfidence(V(p), V(q), a, b); }
float CloseToSideline(const nlVector3& p, const nlVector2* c, bool inv, nlVector2*) { return so_CloseToSideline(V(p), c != 0, c ? c->x : 0, c ? c->y : 0, inv); }
float NearToFormationPosition(cFielder* f, nlVector3* p) { return so_NearToFormation(pid(f), V(p)); }
bool cFielder::CalculateFormationPosition(nlVector3& d)
{
    int p = pid(this);
    V3 v = so_FormationPos(p);
    d.x = v.x; d.y = v.y; d.z = v.z;
    return so_FormationFlag(p);
}
float cFielder::GetRunningSpeed() { return g_desc.p[pid(this)].run; }
PlayerTweaks* cFielder::GetTweaks() const { return *(PlayerTweaks**)((uint8_t*)this + 804); }
float GetJogSpeed(const PlayerTweaks* t) { return g_desc.p[PolicyB::id(t) - 0x300].jog; }
float cField::GetGoalLineX(float side) { return so_GoalLineX(side); }
bool cField::FixOutOfBoundsPosition(nlVector3& v, float m, bool)
{
    V3 q = so_FixOutOfBounds(V(v), m);
    v.x = q.x; v.y = q.y; v.z = q.z;
    return false;
}
float nlSqrt(float x, bool) { return sqrtf(x); }
float nlRecipSqrt(float x, bool) { return 1.0f / sqrtf(x); }
float nlATan2f(float y, float x) { return atan2f(y, x); }
void nlPolarToCartesian(nlVector3& v, const nlPolar& p)
{
    float a = p.a * (6.2831853f / 65536.0f);
    v.x = cosf(a) * p.r; v.y = sinf(a) * p.r;
}

// ---- Impl ---------------------------------------------------------------------------------------------------
static nlVector3 N(const V3& v) { nlVector3 r; r.x = v.x; r.y = v.y; r.z = v.z; return r; }

struct ImplB {
    typedef uint32_t H;
    void setup(const Desc& d) { PolicyB::ids.clear(); g_desc = d; W.build(d); }
    cPlayer* pl(int i) { return i < 0 ? 0 : (cPlayer*)(uintptr_t)W.players[i]; }
    H make(int kind, int owner, int target, float speed)
    {
        void* mem = calloc(1, 0x80);
        cPlayer* po = pl(owner);
        cPlayer* pt = pl(target);
        switch (kind) {
        case K_OL_SHOT: new (mem) SSearchOpenLane(po, 0); break;
        case K_OL_PASS: new (mem) SSearchOpenLane(po, pt); break;
        case K_OL_PASS_NOOWNER: new (mem) SSearchOpenLane(0, pt); break;
        case K_GO: new (mem) SSearchGetOpen(po); break;
        case K_IS: case K_RN: new (mem) SSearchRunToNet(po); break;
        case K_CB: new (mem) SSearchCutAndBreak(po); break;
        default: new (mem) SSearchBestPass(po, pt, kind == K_BP_LEAD, kind == K_BP_PERFECT, speed); break;
        }
        return (H)(uintptr_t)mem + (kind == K_IS ? 0x10 : 0);
    }
    float eval(int kind, H obj, const V3& pos, const V3& center, int dir, int ang)
    {
        SpaceSearch* s = (SpaceSearch*)(uintptr_t)obj;
        nlVector3 p = N(pos), c = N(center);
        return s->EvaluatePosition(p, c, (eFieldDirection)dir, (unsigned short)ang);
    }
    float findBest(int kind, H obj, V3& dest, const V3& center, int dir, const V3& tgt, float radius, int cone)
    {
        SpaceSearch* s = (SpaceSearch*)(uintptr_t)obj;
        nlVector3 d = N(dest), c = N(center), t = N(tgt);
        float v = s->FindBestPosition(d, c, (eFieldDirection)dir, (dir == 3 || dir == 4 || dir == 5) ? &t : 0, radius, (unsigned short)cone);
        dest = { d.x, d.y, d.z };
        return v;
    }
};


extern bool lbl_806E0E28;
float g_fSimulationTick = 0.016666668f;
float lbl_806DC0A8 = 3.0f;       // retail .sdata values (np_desire_data.c)
int lbl_806DC0B0 = 16;
extern const float gStateMachineZeroDuration = 0.0f;
extern const float gStateMachineUnsetDuration = -1.0f;
extern const float gStateMachineNeverActiveTime = -99999.0f;
extern const float gStateMachineAgeThreshold = 10.0f;
// the collection accessors (tu_8030EDB0.cpp has global slot pools whose constructors need the real allocator)
static UnidentifiedFuzzyVariantData* gDefaultData() { static UnidentifiedFuzzyVariantData d; return &d; }
UnidentifiedVariantCollection::UnidentifiedVariantCollection() { for (int i = 0; i < 19; i++) mData[i] = 0; }
UnidentifiedVariantCollection::~UnidentifiedVariantCollection() { }
bool UnidentifiedVariantCollection::IsSet(int index) const { return index > -1 && index < 19 && mData[index] != 0; }
FuzzyVariant* UnidentifiedVariantCollection::Get(int index) { return IsSet(index) ? (FuzzyVariant*)mData[index] : (FuzzyVariant*)gDefaultData(); }
// ---- Desire tests: stand-ins -----------------------------------------------------------------------------------
static uint32_t gAIctx, gSkillTweaks, gFakeDesire;
void cFielder::AddDesiredPosition(const nlVector3& p, float urgency, float weight) { lg(L_ADD, (float)pid(this), p.x, p.y, p.z, urgency, weight); }
float cFielder::GetDistanceToDesiredPos() { int p = pid(this); lg(L_DIST, (float)p); return so_DistToDesired(p, g_desc.salt); }
void cFielder::SetAvoidanceMultiplier(float f) { lg(L_AVOID, (float)pid(this), f); }
Desire* GetFielderDesire(cFielder* f, int i) { lg(L_GETDESIRE, (float)pid(f), (float)i); return (Desire*)(uintptr_t)gFakeDesire; }
void ResetSteeringHistory(DesireSteering*) { lg(L_RESETSTEER); }
bool cFielder::IsStuck() const { return g_desc.stuck[pid(this)]; }
bool cFielder::IsSuperGrowActive() const { return g_desc.grow[pid(this)]; }
float GetFielderHitReach(cFielder* f) { return g_desc.reach[pid(f)]; }
void cPlayer::SetSpaceSearch(SpaceSearch* s) { m_pSpaceSearch = s; lg(L_SETSEARCH, (float)pid(this)); }
void* nlMalloc(unsigned long size, unsigned int, bool) { lg(L_MALLOC, (float)size); return (void*)calloc(1, size < 32 ? 32 : size); }
extern "C" cFielder* fn_800DF790(cTeam* t) { int p = so_StrategicBallOwner(tid(t), g_desc.salt); return p < 0 ? 0 : (cFielder*)(uintptr_t)W.players[p]; }
extern "C" float fn_800DFF1C() { return so_PassInFlight(g_desc.ballState, g_desc.ballPassTarget); }
u32 nlStringHash(const char* s) { return so_hash((const unsigned char*)s); }
SkillTweaks* fn_800A636C(cTeam*) { return (SkillTweaks*)(uintptr_t)gSkillTweaks; }
bool SkillTweaks::GetSkillValue(unsigned long key, float* value, cPlayer* p)
{
    float v = so_skill(key, pid(p));
    lg(L_SKILL, (float)(key & 0xFFFF), (float)pid(p), v);
    *value = v; return true;
}
float nlRandomf(float mx) { return so_random(mx); }
bool AIContext::IsTimerRunning(unsigned long k) { lg(L_ISTIMER, (float)k, g_timer[k] ? 1.0f : 0.0f); return g_timer[k]; }
Timer* AIContext::SetTimer(unsigned long k, float s) { lg(L_SETTIMER, (float)k, s); g_timer[k] = s > 0.0f; return 0; }
AvoidableObject::AvoidableObject(int type) { mType = type; }
AvoidableObject::~AvoidableObject() { }
struct FakeAvoid : AvoidableObject {
    int player;
    FakeAvoid(int p) : AvoidableObject(0), player(p) { }
    virtual const nlVector3& GetPosition() { static nlVector3 z; return z; }
    virtual const nlVector3& GetVelocity() { static nlVector3 z; return z; }
    virtual float GetRadius() { return g_desc.radius[player]; }
};
// weather tables normally defined by platform/compat/np_desire_data.c
nlVector3* lbl_8056DA00[6]; int lbl_8056DA18[6]; nlVector3* lbl_8056DA30[6]; int lbl_8056DA48[6];


// ---- InterceptBall::Update test: stand-ins -----------------------------------------------------------------------
static AvoidController* gAvoidCtl[10];
AvoidController* cFielder::GetAvoidController() { return gAvoidCtl[pid(this)]; }
float Offensive(cTeam* t) { return (t && *(int*)((uint8_t*)t + 24) == 0) ? 1.0f : 0.0f; }          // retail 0x800DFD48
float BallOwner(cPlayer* p) { return (p && *(int*)((uint8_t*)p + P_BALL) != 0) ? 1.0f : 0.0f; }   // retail 0x800D6758
extern "C" float fn_800DAD3C(cBall*) { return so_BallQ(g_desc.salt); }
extern "C" float fn_800DF0B8(cFielder* f) { return so_DF0B8(pid(f), g_desc.salt); }
extern "C" float fn_800D7B00(cFielder* f) { return so_D7B00(pid(f), g_desc.salt); }
float OpenTo(cPlayer* a, cPlayer* b) { return so_OpenTo(pid(a), pid(b), g_desc.salt); }
float cFielder::GetAirInterceptHeight(int t) { int p = pid(this); lg(L_AIRH, (float)p, (float)t); return so_AirHeight(p, t); }
extern "C" void fn_800180F4(cBall* b, nlVector3* o, float t)
{
    V3 bp = *(V3*)((uint8_t*)b + B_POS); lg(L_PREDICT, t, bp.x, bp.y, bp.z);
    V3 r = so_Predict(bp, t); o->x = r.x; o->y = r.y; o->z = r.z;
}
void cFielder::GetApproachPosition(nlVector3* o, const nlVector3* from, float t)
{
    int p = pid(this); lg(L_APPROACH, (float)p, from->x, from->y, from->z, t);
    V3 r = so_Approach(p, { from->x, from->y, from->z }, t); o->x = r.x; o->y = r.y; o->z = r.z;
}
void cFielder::InitActionLooseBallShot(bool b) { lg(L_SHOT, (float)pid(this), b ? 1.0f : 0.0f); }
void cFielder::InitActionLooseBallPass(cFielder* t, bool b) { lg(L_PASSACT, (float)pid(this), (float)pid(t), b ? 1.0f : 0.0f); }
cFielder* cBall::GetOwnerFielder() { return (m_pOwner && m_pOwner->m_eClassType == FIELDER) ? (cFielder*)m_pOwner : 0; }
extern "C" cTeam* fn_800D6670(cFielder* f) { return f ? *(cTeam**)((uint8_t*)f + P_TEAM) : 0; }
void UnidentifiedVariantCollection::Set(int index, FuzzyVariant value)
{
    UnidentifiedFuzzyVariantData* s = (UnidentifiedFuzzyVariantData*)mData[index];
    if (!s) { s = (UnidentifiedFuzzyVariantData*)calloc(1, FVD_SIZE); mData[index] = s; }
    memcpy((uint8_t*)s + 4, (uint8_t*)&value + 4, 16);
    *(int*)((uint8_t*)s + FV_INDEX) = index;
}

template <class T> static T* mkObj() { return new (calloc(1, D_SIZE)) T(1, (void*)0); }
template <> DesireInterceptBall* mkObj<DesireInterceptBall>() { return new (calloc(1, D_SIZE)) DesireInterceptBall(1); }
struct ImplB2 : ImplB {
    void setup(const Desc& d)
    {
        ImplB::setup(d);
        gAIctx = PolicyB::alloc(0x40); PolicyB::reg(gAIctx, 0x700);
        gSkillTweaks = PolicyB::alloc(0x40); PolicyB::reg(gSkillTweaks, 0x720);
        gFakeDesire = PolicyB::alloc(0x40); PolicyB::reg(gFakeDesire, 0x710);
        for (int i = 0; i < 10; i++) {
            FakeAvoid* av = new (calloc(1, sizeof(FakeAvoid))) FakeAvoid(i);
            *(AvoidableObject**)((uint8_t*)(uintptr_t)W.players[i] + P_AVOID) = av;
        }
        so_seedRandom(d.salt);
        for (int i = 0; i < 10; i++) gAvoidCtl[i] = (AvoidController*)calloc(1, 0x40);
        for (int i = 0; i < 8; i++) g_timer[i] = false;
    }
    uint32_t mkUpdate(int init)
    {
        UnidentifiedVariant_80054AB8* u = new (calloc(1, V_SIZE)) UnidentifiedVariant_80054AB8();
        uint8_t* p = (uint8_t*)u;
        *(int*)(p + V_TYPE) = 3; *(int*)(p + V_DATA) = init; p[V_TEMP] = 1;
        return (uint32_t)(uintptr_t)u;
    }
    void outUpdate(uint32_t h)
    {
        uint8_t* p = (uint8_t*)(uintptr_t)h;
        g_out.add((float)*(int*)(p + V_TYPE)); g_out.add((float)*(int*)(p + V_DATA)); g_out.add((float)p[V_TEMP]);
    }
    template <class T> T* mk(int fielder)
    {
        T* d = mkObj<T>();
        *(cFielder**)((uint8_t*)d + D_FIELDER) = (cFielder*)(uintptr_t)W.players[fielder];
        uint8_t* sm = (uint8_t*)calloc(1, 0x80); *(uint32_t*)(sm + SM_AICTX) = gAIctx;
        *(uint32_t*)((uint8_t*)d + D_SM) = (uint32_t)(uintptr_t)sm;
        return d;
    }
    void outV3(const void* a) { const float* f = (const float*)a; g_out.add(f[0]); g_out.add(f[1]); g_out.add(f[2]); }
    void desireInterceptUpdate(const Desc& d)
    {
        DesireInterceptBall* de = mk<DesireInterceptBall>(d.fielder);
        *(int*)((uint8_t*)de + 164) = 0; *((uint8_t*)de + 168) = d.icPass ? 1 : 0;
        *(int*)((uint8_t*)(uintptr_t)W.teams[d.p[d.fielder].team] + 24) = ((d.salt >> 4) % 4 == 0) ? 0 : 1;
        *(uint32_t*)((uint8_t*)(uintptr_t)W.players[d.fielder] + P_BALL) = ((d.salt >> 3) % 5 == 0) ? W.ball : 0;
        int init = d.icMode ? 3 : d.updateInit;
        uint32_t upd = mkUpdate(init);
        uint8_t* u = (uint8_t*)(uintptr_t)upd;
        auto setx = [&](int idx, FuzzyVariant* v) { ((FuzzyVariant**)(u + V_EXTRA))[idx] = v; };
        if (init == 3) switch (d.icMode) {
        case 1: setx(8, mkVar(3, 15, 0, 0, 8)); setx(16, mkVar(2, d.icChip ? 1u : 0u, 0, 0, 16)); break;
        case 2: setx(8, mkVar(3, 14, 0, 0, 8)); setx(14, mkVar(9, W.players[d.icTarget], 0, 0, 14)); break;
        case 3: setx(8, mkVar(3, 7, 0, 0, 8)); break;
        }
        de->DesireInterceptBall::Update((UnidentifiedVariant_80054AB8*)u, 0.02f);
        outUpdate(upd);
        uint8_t* e8 = (uint8_t*)((FuzzyVariant**)(u + V_EXTRA))[8];
        g_out.add(e8 ? 1.0f : 0.0f);
        if (e8) { g_out.add((float)*(int*)(e8 + 4)); g_out.add((float)*(int*)(e8 + 8)); }
        g_out.add(*(float*)((uint8_t*)gAvoidCtl[d.fielder] + 0x10));
    }
    void desireCut(const Desc& d)
    {
        DesireCutAndBreak* de = mk<DesireCutAndBreak>(d.fielder);
        float* des = (float*)((uint8_t*)de + D_DESIRED);
        des[0] = d.desired.x; des[1] = d.desired.y; des[2] = d.desired.z;
        uint32_t upd = mkUpdate(d.updateInit);
        de->DesireCutAndBreak::Update((UnidentifiedVariant_80054AB8*)(uintptr_t)upd, 0.02f);
        outUpdate(upd);
    }
    void desireGetOpen(const Desc& d)
    {
        DesireGetOpen* de = mk<DesireGetOpen>(d.fielder);
        lbl_806E0E28 = d.debugFlag;
        bool r = de->DesireGetOpen::Initialize(0);
        g_out.add(r ? 1.0f : 0.0f);
        outV3((uint8_t*)de + D_DESIRED);
        g_out.add(*(void**)((uint8_t*)de + D_A4) != 0 ? 1.0f : 0.0f);
        cFielder* f = (cFielder*)(uintptr_t)W.players[d.fielder];
        SpaceSearch* ss = f->m_pSpaceSearch;
        g_out.add((float)ss->m_bDebugOn);
        // kind: SSearchGetOpen has a different size than OpenLane; recognise it by the allocation logged above
        g_out.add(g_out.v[0] == L_MALLOC && g_out.v[1] == 20.0f ? 1.0f : (g_out.v[0] == L_MALLOC && g_out.v[1] == 40.0f ? 2.0f : 0.0f));
    }
    void setRunFields(DesireRunToTarget* de, const Desc& d)
    {
        uint8_t* b = (uint8_t*)de;
        *(uint32_t*)(b + R_TFIELDER) = d.targetKind == 1 ? W.players[d.targetFielder] : 0;
        *(uint32_t*)(b + R_TBALL) = d.targetKind == 2 ? W.ball : 0;
        float* tp = (float*)(b + R_TPOS); tp[0] = d.targetPos.x; tp[1] = d.targetPos.y; tp[2] = d.targetPos.z;
        *(int*)(b + R_DIR) = d.direction;
        *(float*)(b + R_DISTOFF) = d.distOff; *(float*)(b + R_URGENCY) = d.urgency;
        *(float*)(b + R_SPEED) = d.speedCoeff; *(float*)(b + R_AVOID) = d.avoidCoeff;
    }
    void outRunFields(DesireRunToTarget* de)
    {
        uint8_t* b = (uint8_t*)de;
        uint32_t tf = *(uint32_t*)(b + R_TFIELDER);
        g_out.add((float)(tf ? pid((void*)(uintptr_t)tf) : -1)); g_out.add(*(uint32_t*)(b + R_TBALL) != 0 ? 1.0f : 0.0f);
        outV3(b + R_TPOS);
        g_out.add((float)*(int*)(b + R_DIR));
        g_out.add(*(float*)(b + R_DISTOFF)); g_out.add(*(float*)(b + R_URGENCY));
        g_out.add(*(float*)(b + R_SPEED)); g_out.add(*(float*)(b + R_AVOID));
        outV3(b + D_DESIRED);
    }
    void desireRunUpdate(const Desc& d)
    {
        DesireRunToTarget* de = mk<DesireRunToTarget>(d.fielder);
        setRunFields(de, d);
        g_timer[1] = d.timerRunning;
        uint32_t upd = mkUpdate(d.updateInit);
        de->DesireRunToTarget::Update((UnidentifiedVariant_80054AB8*)(uintptr_t)upd, 0.02f);
        outUpdate(upd);
        outRunFields(de);
    }
    FuzzyVariant* mkVar(int type, uint32_t a, uint32_t b, uint32_t c, int index)
    {
        uint8_t* h = (uint8_t*)calloc(1, FVD_SIZE);
        FuzzyVariant tmp;                                  // for its vptr
        asm volatile("" : : "r"(&tmp) : "memory");
        memcpy(h, &tmp, 4);
        *(int*)(h + 4) = type; *(uint32_t*)(h + 8) = a; *(uint32_t*)(h + 12) = b; *(uint32_t*)(h + 16) = c; *(int*)(h + FV_INDEX) = index;
        return (FuzzyVariant*)h;
    }
    UnidentifiedVariantCollection* mkParams(const Desc& d)
    {
        UnidentifiedVariantCollection* coll = (UnidentifiedVariantCollection*)calloc(1, 19 * 4);
        auto fb = [](float f) { uint32_t b; memcpy(&b, &f, 4); return b; };
        auto setv = [&](int idx, FuzzyVariant* v) { ((FuzzyVariant**)coll)[idx] = v; };
        if (d.paramMask & 1) setv(17, mkVar(3, d.paramDirection, 0, 0, 17));
        if (d.paramMask & 2) setv(13, mkVar(5, fb(d.paramSpeed), 0, 0, 13));
        if (d.paramMask & 4) setv(18, mkVar(5, fb(d.paramDist), 0, 0, 18));
        if (d.paramMask & 8) setv(11, mkVar(5, fb(d.paramUrgency), 0, 0, 11));
        if (d.paramMask & 16) setv(2, mkVar(5, fb(d.paramAvoid), 0, 0, 2));
        switch (d.paramTarget) {
        case 1: setv(14, mkVar(9, W.players[d.targetFielder], 0, 0, 14)); break;
        case 2: setv(14, mkVar(12, W.ball, 0, 0, 14)); break;
        case 3: setv(14, mkVar(6, fb(d.targetPos.x), fb(d.targetPos.y), fb(d.targetPos.z), 14)); break;
        case 4: setv(0, mkVar(9, W.players[d.targetFielder], 0, 0, 0)); break;
        case 5: setv(14, mkVar(6, fb(d.targetPos.x), fb(d.targetPos.y), fb(d.targetPos.z), 14));
                setv(0, mkVar(9, W.players[d.targetFielder], 0, 0, 0)); break;
        }
        return coll;
    }
    void desireRunInit(const Desc& d)
    {
        DesireRunToTarget* de = mk<DesireRunToTarget>(d.fielder);
        g_timer[1] = d.timerRunning;
        UnidentifiedVariantCollection* params = mkParams(d);
        bool r = de->DesireRunToTarget::Initialize(params);
        g_out.add(r ? 1.0f : 0.0f);
        outRunFields(de);
    }
    void desireIntercept(const Desc& d)
    {
        DesireInterceptBall* de = mk<DesireInterceptBall>(d.fielder);
        g_pCurrentlyUpdatingTeam = (cTeam*)(uintptr_t)W.teams[d.p[d.fielder].team];
        bool r = de->DesireInterceptBall::Initialize(0);
        g_out.add(r ? 1.0f : 0.0f);
        g_out.add((float)*(int*)((uint8_t*)de + 164)); g_out.add((float)*((uint8_t*)de + 168));
    }
    void trans1(const Desc& d)
    {
        DesireRunToTarget* de = mk<DesireRunToTarget>(d.fielder);
        float* des = (float*)((uint8_t*)de + D_DESIRED);
        des[0] = d.desired.x; des[1] = d.desired.y; des[2] = d.desired.z;
        if (d.salt & 1) { float k = 0.63f + 0.2f * (float)((d.salt >> 2) & 7); float* pos = (float*)((uint8_t*)(uintptr_t)W.players[d.fielder] + P_POS); pos[0] = d.desired.x + 0.3f * k; pos[1] = d.desired.y - 0.4f * k; }
        uint8_t* ctx = (uint8_t*)calloc(1, 0x40); *(uint32_t*)(ctx + 8) = W.players[d.fielder];
        DesireUpdate r = fn_800B38AC((AIContext*)ctx, (UnidentifiedFuzzyRuntimeValue*)de);
        uint8_t* p = (uint8_t*)&r;
        g_out.add((float)*(int*)(p + V_TYPE)); g_out.add((float)*(int*)(p + V_DATA)); g_out.add((float)p[V_TEMP]);
    }
    void trans2(const Desc& d)
    {
        uint8_t* ctx = (uint8_t*)calloc(1, 0x40); *(uint32_t*)(ctx + 8) = W.players[d.fielder];
        *(uint32_t*)((uint8_t*)(uintptr_t)W.players[d.fielder] + P_BALL) = (d.salt & 2) ? W.ball : 0;
        DesireUpdate r = fn_800B4DC0((AIContext*)ctx, 0);
        uint8_t* p = (uint8_t*)&r;
        g_out.add((float)*(int*)(p + V_TYPE)); g_out.add((float)*(int*)(p + V_DATA)); g_out.add((float)p[V_TEMP]);
    }
    void weather()
    {
        fn_800B0358();
        for (int tab = 0; tab < 2; tab++) {
            nlVector3** pb = tab == 0 ? lbl_8056DA00 : lbl_8056DA30;
            int* cb = tab == 0 ? lbl_8056DA18 : lbl_8056DA48;
            for (int i = 0; i < 6; i++) {
                int n = cb[i];
                g_out.add((float)n);
                for (int j = 0; j < n; j++) { g_out.add(pb[i][j].x); g_out.add(pb[i][j].y); g_out.add(pb[i][j].z); }
            }
        }
        for (int i = 0; i < 4; i++)
            for (int side = 0; side < 2; side++) {
                nlVector4 v = fn_800B04BC(0, i, side);
                g_out.add(v.x); g_out.add(v.y); g_out.add(v.z); g_out.add(v.w);
            }
    }
};

int main(int argc, char** argv)
{
    int s0 = argc > 1 ? atoi(argv[1]) : 0, n = argc > 2 ? atoi(argv[2]) : 20;
    if (argc > 3 && !strcmp(argv[3], "desire")) { ImplB2 I; runDesire(I, s0, n); return 0; }
    ImplB I;
    runCases(I, s0, n);
    return 0;
}
