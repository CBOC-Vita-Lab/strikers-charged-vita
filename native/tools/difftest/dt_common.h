// difftest common code, compiled into BOTH programs:
//   dt_a  (x86-64): runs the RETAIL code (the recompiler's translation of main.dol) over a synthetic game state in
//                   big-endian guest memory
//   dt_b  (host32): runs the NATIVE port code (src/Game/AI/*.cpp) over the same state in little-endian host memory
// Both print the same text; run.sh compares the numbers. The state is built by DtWorld<Policy>::build from one
// descriptor (Desc) at the RETAIL offsets, so every field the native code reads through a header member name is also
// checked for its offset and meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace dt {

struct V3 { float x, y, z; };

// tiny handle -> id table (no STL: the decomp's headers define placement new, which clashes with <new>)
struct IdMap {
    uint32_t key[128]; int val[128]; int n = 0;
    void clear() { n = 0; }
    void put(uint32_t k, int v) { key[n] = k; val[n] = v; n++; if (n >= 128) abort(); }
    int get(uint32_t k) const { for (int i = 0; i < n; i++) if (key[i] == k) return val[i]; return -9999; }
};

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) { for (int i = 0; i < 4; i++) next(); }
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
    float uf() { return (next() & 0xFFFFFF) / 16777216.0f; }
    float rng(float a, float b) { return a + (b - a) * uf(); }
    int ri(int n) { return (int)(next() % (uint32_t)n); }
    bool chance(float p) { return uf() < p; }
};

// ---- retail offsets -----------------------------------------------------------------------------------------
enum {
    P_CLASS = 240, P_POS = 48, P_VEL = 72, P_SPEED = 136, P_ID = 484, P_BALL = 784, P_SPACESEARCH = 796, P_TEAM = 788,
    P_AVOID = 800, P_TWEAKS = 804, P_FLAGS = 1108, P_SIZE = 0x600,
    T_SIDE = 0, T_BEST = 136, T_NET = 232, T_INTERCEPTPOS = 88, T_PLAYERS = 164, T_SIZE = 0x100,
    N_DIR = 4, N_LOC = 8, N_SIZE = 0x20,
    B_SHOT = 16, B_POS = 84, B_PASSINT = 120, B_STATE = 184, B_OWNER = 200, B_PASSTARGET = 212, B_SIZE = 0x100,
    G_TWEAKS = 4, G_SIZE = 0x1100,
    FT_FIRST = 0x44, FT_STRIDE = 0x10, FT_COUNT = 118, FT_SIZE = 0x7B8,
    PT_FSHOOTING = 0xA4, PT_SIZE = 0xC4,
    CLS_FIELDER = 2, CLS_GOALIE = 3,
    // Desire objects (shdStateMachine base: mScriptMachine at 24; Desire: m_pFielder 136, mvDesiredPosition 140)
    D_SM = 24, D_FIELDER = 136, D_DESIRED = 140, D_A4 = 164, D_SIZE = 0xD0,
    SM_AICTX = 100,
    // DesireRunToTarget fields
    R_TFIELDER = 164, R_TBALL = 168, R_TPOS = 172, R_DIR = 184, R_DISTOFF = 188, R_URGENCY = 192, R_SPEED = 196, R_AVOID = 200,
    // UnidentifiedVariant_80054AB8 / FuzzyVariant
    V_TYPE = 4, V_DATA = 8, V_EXTRA = 24, V_TEMP = 100, V_SIZE = 0x70, FV_INDEX = 20, FVD_SIZE = 24,
};
// FuzzyTweaks binding index by retail pointer offset (pointer = binding + 0xC)
inline int ftIndex(int ptrOffset) { return (ptrOffset - 0xC - FT_FIRST) / FT_STRIDE; }

// ---- scenario -----------------------------------------------------------------------------------------------
struct PlayerDesc {
    int team;           // 0 / 1
    int cls;            // CLS_FIELDER / CLS_GOALIE
    V3 pos, vel;
    float speed;        // actual speed
    int id;             // gameplay id
    float run, jog, shooting;
    unsigned flags;     // muInvincibleStatus
};
struct Desc {
    PlayerDesc p[10];   // team*5 + 0..3 fielders, +4 goalie
    float netDir[2];
    V3 netLoc[2];
    int best[2];        // best ball interceptor (player index) or -1
    V3 interceptPos[2][4];
    int ballState, ballOwner, ballPassTarget;   // players or -1
    unsigned ballShotPacked;
    V3 ballPos, ballPassInt;
    float ft[FT_COUNT];
    // Desire test inputs
    float radius[10], reach[10];
    bool stuck[10], grow[10];
    int salt;
    int updateInit;                 // initial DesireUpdate value (0..4)
    int fielder;                    // the fielder that owns the desire
    V3 desired;
    int targetKind;                 // 0 none, 1 fielder, 2 ball, 3 vector (RunToTarget)
    int targetFielder; V3 targetPos; int direction; float distOff, urgency, speedCoeff, avoidCoeff;
    bool timerRunning;
    int paramMask;                  // which of the RunToTarget parameters (17,13,18,11,2) are set
    int paramDirection; float paramSpeed, paramDist, paramUrgency, paramAvoid;
    int paramTarget;                // 0: none, 1: index 14 player, 2: index 14 ball, 3: index 14 vector, 4: index 0 player, 5: index 14 vector AND index 0 player (14 wins)
    bool debugFlag;
    int icMode;                     // InterceptBall::Update: 0 plain update, 1 CHANGE+shot (param 8 = 15), 2 CHANGE+pass (8 = 14), 3 CHANGE other (8 = 7)
    bool icChip, icPass;            // shot is a chip shot / mbInterceptPass
    int icTarget;                   // pass target player
    float ballIn[2][4];             // team->mfBallInTimes (0x48)
};

inline void genDesc(Desc& d, Rng& r)
{
    for (int i = 0; i < 10; i++) {
        PlayerDesc& p = d.p[i];
        p.team = i / 5;
        p.cls = (i % 5 == 4) ? CLS_GOALIE : CLS_FIELDER;
        p.pos = { r.rng(-20, 20), r.rng(-10, 10), 0.0f };
        if (p.cls == CLS_GOALIE) p.pos.x = (p.team == 0 ? -1 : 1) * r.rng(17, 20);
        float sp = r.chance(0.3f) ? r.rng(0, 1) : r.rng(0, 9);
        float ang = r.rng(0, 6.2831853f);
        p.vel = { cosf(ang) * sp, sinf(ang) * sp, 0.0f };
        p.speed = sp;
        p.id = i % 5;
        p.run = r.rng(7, 9);
        p.jog = r.rng(3, 5);
        p.shooting = r.rng(0, 1);
        p.flags = r.chance(0.3f) ? 1u : 0u;
    }
    for (int t = 0; t < 2; t++) {
        d.netDir[t] = (t == 0) ? 1.0f : -1.0f;   // net direction of a team's own net
        d.netLoc[t] = { (t == 0 ? -21.0f : 21.0f), r.rng(-0.5f, 0.5f), 0.0f };
        d.best[t] = r.chance(0.2f) ? -1 : t * 5 + r.ri(4);
        for (int k = 0; k < 4; k++) d.interceptPos[t][k] = { r.rng(-20, 20), r.rng(-10, 10), 0.0f };
    }
    const int st[] = { 3, 5, 5, 0, 1, 7, 8 };
    d.ballState = st[r.ri(7)];
    d.ballOwner = r.chance(0.4f) ? r.ri(10) : -1;
    d.ballPassTarget = r.chance(0.7f) ? r.ri(10) : -1;
    d.ballShotPacked = r.chance(0.5f) ? 0u : 1234u;
    d.ballPos = { r.rng(-20, 20), r.rng(-10, 10), r.rng(0, 2) };
    d.ballPassInt = { r.rng(-20, 20), r.rng(-10, 10), r.rng(0, 2) };
    for (int i = 0; i < FT_COUNT; i++) d.ft[i] = r.rng(0.5f, 30.0f);
    for (int i = 0; i < 10; i++) { d.radius[i] = 0.3f + 0.4f * r.uf(); d.reach[i] = 0.5f + r.uf(); d.stuck[i] = r.chance(0.2f); d.grow[i] = r.chance(0.3f); }
    d.salt = r.ri(1000);
    d.updateInit = r.ri(5);
    d.fielder = r.ri(2) * 5 + r.ri(4);
    d.desired = { r.rng(-20, 20), r.rng(-10, 10), r.rng(-1, 1) };
    d.targetKind = r.ri(4);
    d.targetFielder = r.ri(10);
    d.targetPos = { r.rng(-20, 20), r.rng(-10, 10), r.rng(-1, 1) };
    d.direction = r.chance(0.4f) ? 4 : 3;
    d.distOff = r.rng(0, 2); d.urgency = r.rng(1, 5); d.speedCoeff = r.rng(0.5f, 2); d.avoidCoeff = r.rng(0.5f, 2);
    d.timerRunning = r.chance(0.3f);
    d.paramMask = r.ri(32);
    d.paramDirection = r.chance(0.5f) ? 4 : 3; d.paramSpeed = r.rng(0.5f, 2); d.paramDist = r.rng(0, 2); d.paramUrgency = r.rng(1, 5); d.paramAvoid = r.rng(0.5f, 2);
    d.paramTarget = r.ri(6);
    d.debugFlag = r.chance(0.5f);
    // late additions (kept at the end so the earlier draws are unchanged): airborne players (z != 0), the test fielder owning the ball
    for (int i = 0; i < 10; i++)
        if (r.chance(0.3f)) { d.p[i].pos.z = r.rng(0, 1.5f); d.p[i].vel.z = r.rng(-2, 2); }
    if (r.chance(0.2f)) d.ballOwner = d.fielder;
    d.icMode = r.chance(0.5f) ? 0 : 1 + r.ri(3); d.icChip = r.chance(0.5f); d.icPass = r.chance(0.6f); d.icTarget = r.ri(10);
    for (int t = 0; t < 2; t++) for (int k = 0; k < 4; k++) d.ballIn[t][k] = r.rng(0, 6);
}

// ---- stand-ins for the helper functions both sides call (pure functions of plain values) ---------------------
inline float sm(float seed, float a = 0, float b = 0, float c = 0, float d = 0)
{
    return 0.5f + 0.5f * sinf(seed + 1.7f * a + 2.3f * b + 3.1f * c + 4.3f * d);
}
inline float so_PositionOpenness(const V3& pos, int team, int player, bool hasRadius, float r0, float r1, bool b, float t)
{
    return sm(1.0f + 0.37f * team + 0.91f * player + (hasRadius ? 2.0f : 0.0f) + (b ? 5.0f : 0.0f), pos.x * 0.21f, pos.y * 0.33f, t * 3.0f, r0 + r1);
}
inline float so_WidePositionOpenness(const V3& pos, int team, int player, bool b, float t)
{
    return sm(2.0f + 0.41f * team + 0.77f * player + (b ? 5.0f : 0.0f), pos.x * 0.19f, pos.y * 0.29f, t * 3.0f);
}
inline float so_NearToSideline(const V3& p) { return sm(3.0f, p.x * 0.13f, p.y * 0.4f); }
inline float so_fn_800DCB4C(const V3& p, const V3& q) { return sm(4.0f, p.x * 0.11f, p.y * 0.2f, q.x * 0.17f, q.y * 0.23f); }
inline float so_PositionIsInFrontOfNet(const V3& p, int net) { return sm(5.0f + net, p.x * 0.15f, p.y * 0.31f); }
inline float so_InOffensiveZone(const V3& p, int side) { float v = sm(6.0f + side, p.x * 0.3f, p.y * 0.1f); return v < 0.35f ? 0.0f : v; }
inline float so_PositionShotDistance(const V3& p, const V3& q, float shooting) { return sm(7.0f, p.x * 0.12f, p.y * 0.2f, q.x * 0.17f, shooting * 4.0f); }
inline float so_PositionDistanceConfidence(const V3& p, const V3& q, float mn, float mx) { return sm(8.0f, p.x * 0.1f, p.y * 0.3f, q.x * 0.17f + q.y * 0.13f, mn * 0.3f + mx * 0.1f); }
inline float so_CloseToSideline(const V3& p, bool hasConf, float c0, float c1, bool invert) { return sm(9.0f + (invert ? 3.0f : 0.0f), p.x * 0.2f, p.y * 0.35f, hasConf ? c0 + c1 : 0.0f); }
inline float so_NearToFormation(int player, const V3& p) { return sm(10.0f + player, p.x * 0.16f, p.y * 0.27f); }
inline V3 so_FormationPos(int player) { return { sm(11.0f + player) * 30.0f - 15.0f, sm(12.0f + player) * 16.0f - 8.0f, 0.0f }; }
inline bool so_FormationFlag(int player) { return sm(13.0f + player) > 0.7f; }
inline float so_OpenLane(const V3& a, const V3& b, int p1, int p2, float w0, float w1, float w2, float t)
{
    return sm(14.0f + 0.3f * p1 + 0.7f * p2, a.x * 0.12f + b.x * 0.07f, a.y * 0.2f + b.y * 0.11f, w0 + 2.0f * w1 + 3.0f * w2, t * 5.0f);
}
inline V3 so_OffNet(int player, bool hasRef, const V3& ref) { return { (player < 5 ? 21.0f : -21.0f), sm(15.0f + player, hasRef ? ref.y : 0.0f) - 0.5f, 0.0f }; }
inline float so_GoalLineX(float side) { return side > 0.0f ? 20.6f : -20.6f; }   // retail 0x8002D148 reads mv3FieldPosition.x = 20.6
inline V3 so_FixOutOfBounds(const V3& p, float m)
{
    V3 q = p;
    if (q.x > 22.0f - m) q.x = 22.0f - m; if (q.x < -22.0f + m) q.x = -22.0f + m;
    if (q.y > 11.5f - m) q.y = 11.5f - m; if (q.y < -11.5f + m) q.y = -11.5f + m;
    return q;
}

inline float so_radius(const Desc& d, int p) { return d.radius[p]; }
inline int so_StrategicBallOwner(int team, int salt) { int v = (salt * 7 + team * 3) % 9; return v >= 4 ? -1 : team * 5 + v; }
inline float so_DistToDesired(int p, int salt) { float v = sm(24.0f + p, (float)salt) * 7.3f; v -= (float)(int)v; return 1.2f * (v < 0 ? v + 1.0f : v); }   // spread over [0,1.2) so the 0.5 / 0.7 thresholds are probed
inline uint32_t so_hash(const unsigned char* s) { uint32_t h = 0xFFFFFFFFu; while ((signed char)*s) { h = h + (h << 5) + *s; s++; } return h; }
inline float so_skill(uint32_t hash, int player) { return sm(30.0f + player, (float)(hash & 0xFFFF) * 0.001f); }
inline uint32_t g_rs;
inline void so_seedRandom(int salt) { g_rs = 12345u + (uint32_t)salt * 77u; }
inline float so_random(float mx) { g_rs = g_rs * 1664525u + 1013904223u; return ((g_rs >> 8) / 16777216.0f) * mx; }
inline float so_PassInFlight(int state, int passTarget) { return ((state == 5 || state == 3) && passTarget >= 0) ? 1.0f : 0.0f; }

// ---- InterceptBall::Update helpers (script questions etc.): smooth / hashed stand-ins -----------------------------
inline float frac01(float v) { return v - floorf(v); }
inline float so_BallQ(int salt) { return frac01(salt * 0.173f) < 0.1f ? 1.0f : 0.0f; }                  // fn_800DAD3C(ball)
inline float so_DF0B8(int pid, int salt) { return frac01(salt * 0.61f + pid * 0.37f) < 0.5f ? 1.0f : 0.0f; }
inline float so_D7B00(int pid, int salt) { return sm(50.0f + pid, (float)salt); }
inline float so_OpenTo(int a, int b, int salt) { return sm(51.0f + a, (float)b, (float)salt); }
inline float so_AirHeight(int pid, int type) { return 0.2f + 1.5f * sm(52.0f + pid, (float)type); }
inline V3 so_Predict(const V3& ballPos, float t) { return { ballPos.x + 3.0f * t, ballPos.y - 2.0f * t, ballPos.z + sm(53.0f, t) }; }
inline V3 so_Approach(int pid, const V3& from, float t) { return { from.x * 0.5f + pid + t, from.y * 0.25f - pid * 0.5f, sm(54.0f, from.z, t) }; }

// ---- world builder ------------------------------------------------------------------------------------------
// Policy P: using H = uint32_t; static H alloc(n); w32(H,u32); wf(H,float); r32(H); rf(H); registry ids
template <class P>
struct DtWorld {
    typename P::H players[10], teams[2], nets[2], ball, game, ftw, ptw[10], cells;
    typename P::H ftCell[FT_COUNT], shootCell[10];
    void wv(typename P::H a, const V3& v) { P::wf(a, v.x); P::wf(a + 4, v.y); P::wf(a + 8, v.z); }
    void build(const Desc& d)
    {
        for (int t = 0; t < 2; t++) { nets[t] = P::alloc(N_SIZE); teams[t] = P::alloc(T_SIZE); }
        for (int i = 0; i < 10; i++) { players[i] = P::alloc(P_SIZE); ptw[i] = P::alloc(PT_SIZE); shootCell[i] = P::alloc(4); }
        ball = P::alloc(B_SIZE); game = P::alloc(G_SIZE); ftw = P::alloc(FT_SIZE);
        cells = P::alloc(4 * FT_COUNT);
        for (int t = 0; t < 2; t++) {
            P::w32(teams[t] + T_SIDE, t);
            P::w32(teams[t] + T_NET, P::ptr(nets[t]));
            P::w32(teams[t] + T_BEST, d.best[t] < 0 ? 0 : P::ptr(players[d.best[t]]));
            for (int k = 0; k < 4; k++) wv(teams[t] + T_INTERCEPTPOS + 12 * k, d.interceptPos[t][k]);
            for (int k = 0; k < 4; k++) P::wf(teams[t] + 0x48 + 4 * k, d.ballIn[t][k]);   // mfBallInTimes
            for (int k = 0; k < 5; k++) P::w32(teams[t] + T_PLAYERS + 4 * k, P::ptr(players[t * 5 + k]));
            P::wf(nets[t] + N_DIR, d.netDir[t]);
            wv(nets[t] + N_LOC, d.netLoc[t]);
            P::reg(teams[t], 0x100 + t); P::reg(nets[t], 0x200 + t);
        }
        for (int i = 0; i < 10; i++) {
            const PlayerDesc& p = d.p[i];
            typename P::H h = players[i];
            P::w32(h + P_CLASS, p.cls);
            wv(h + P_POS, p.pos); wv(h + P_VEL, p.vel);
            P::wf(h + P_SPEED, p.speed);
            P::w32(h + P_ID, p.id);
            P::w32(h + P_TEAM, P::ptr(teams[p.team]));
            P::w32(h + P_FLAGS, p.flags);
            P::w32(h + P_TWEAKS, P::ptr(ptw[i]));
            P::wf(shootCell[i], p.shooting);
            P::w32(ptw[i] + PT_FSHOOTING + 0xC, P::ptr(shootCell[i]));
            P::reg(h, i); P::reg(ptw[i], 0x300 + i);
        }
        // ball
        P::w32(ball + B_SHOT, d.ballShotPacked);
        wv(ball + B_POS, d.ballPos); wv(ball + B_PASSINT, d.ballPassInt);
        P::w32(ball + B_STATE, d.ballState);
        P::w32(ball + B_OWNER, d.ballOwner < 0 ? 0 : P::ptr(players[d.ballOwner]));
        P::w32(ball + B_PASSTARGET, d.ballPassTarget < 0 ? 0 : P::ptr(players[d.ballPassTarget]));
        // game + fuzzy tweaks
        P::w32(game + G_TWEAKS, P::ptr(ftw));
        for (int i = 0; i < FT_COUNT; i++) {
            P::wf(cells + 4 * i, d.ft[i]);
            P::w32(ftw + FT_FIRST + FT_STRIDE * i + 0xC, P::ptr(cells + 4 * i));
        }
        P::reg(ball, 0x400); P::reg(game, 0x500);
        P::setGlobals(ball, game, teams[0], teams[1]);
        this->d = d;
    }
    Desc d;
    V3 rv(typename P::H a) { return { P::rf(a), P::rf(a + 4), P::rf(a + 8) }; }
};

inline void printf3(const char* name, float v) { printf("%s %.7g\n", name, v); }

}  // namespace dt
