// Screen-size level of detail, game side (np/lod.h). 1.3 wave 5.
#ifdef NP_NATIVE
#include "np/lod.h"
#include "np/lod_simplify.h"

#include "NL/gl/glModel.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/glx/glxDisplayList.h"
#include "Game/Task/GameTaskState.h"
#include "NL/nlTask.h"

#include <map>
#include <vector>
#include <time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int LodInvOn()
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("MSC_LOD_INV"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}

static unsigned long ModelTriangles(const glModel* m, unsigned long* uniq, unsigned long* idx)
{
    unsigned long t = 0;
    *uniq = 0;
    *idx = 0;
    for (unsigned long i = 0; i < m->numPackets; ++i)
    {
        const glModelPacket& p = m->packets[i];
        t += glGetNumTriangles((eGLPrimitive)(unsigned char)p.primType, p.numVertices);
        *uniq += p.numUniqueVertices;
        *idx += p.numVertices;
    }
    return t;
}

// MSC_LOD_INV=1: one line per model loaded (hash id, packets, triangles of the index buffers, unique vertices, index count, primitive of the first packet,
// whether it carries skin streams 5/7). MSC_LOD_DUMP=<dir>: also the raw streams and indices of every packet, for the host tools (native/tools/lod).
extern "C" void NpLodModelLoaded(const glModel* m)
{
    if (!LodInvOn()) return;
    unsigned long uniq, idx;
    const unsigned long tris = ModelTriangles(m, &uniq, &idx);
    int skin = 0;
    for (unsigned long i = 0; i < m->numPackets; ++i)
        for (unsigned long s = 0; s < m->packets[i].numStreams; ++s)
            if (m->packets[i].streams[s].id == 7) skin = 1;
    fprintf(stderr, "[lod-inv] model %08lx packets %lu tris %lu uniq %lu idx %lu prim %d skin %d\n", (unsigned long)m->id, (unsigned long)m->numPackets, tris, uniq, idx,
            m->numPackets ? (int)m->packets[0].primType : -1, skin);
    const char* dir = getenv("MSC_LOD_DUMP");
    if (dir && *dir)
    {
        for (unsigned long i = 0; i < m->numPackets; ++i)
        {
            const glModelPacket& p = m->packets[i];
            if (p.indexBuffer == 0) continue;
            char name[512];
            snprintf(name, sizeof name, "%s/%08lx_%02lu.pkt", dir, (unsigned long)m->id, i);
            FILE* f = fopen(name, "wb");
            if (!f) continue;
            unsigned int hdr[8] = {0x4B50444Cu, (unsigned)p.numVertices, (unsigned)p.numUniqueVertices, (unsigned)(unsigned char)p.primType, (unsigned)p.numStreams, 0, 0, 0};
            fwrite(hdr, 4, 8, f);
            for (unsigned long s = 0; s < p.numStreams; ++s)
            {
                const glModelStream& st = p.streams[s];
                unsigned int sh[4] = {st.id, st.stride, st.unknown07, 0};
                fwrite(sh, 4, 4, f);
                fwrite(st.address, st.stride, p.numUniqueVertices, f);
            }
            fwrite(p.indexBuffer, 2, p.numVertices, f);
            fclose(f);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------- config
static float EnvF(const char* n, float d) { const char* e = getenv(n); return (e && *e) ? (float)atof(e) : d; }

extern "C" int NpLodMode(void)
{
    static int v = -1;
    if (v < 0) { v = (int)EnvF("MSC_LOD_SIZE", 0); if (v < 0) v = 0; if (v > 2) v = 2; }
    return v;
}
extern "C" float NpLodHyst(void) { static float h = EnvF("MSC_LOD_HYST", 0.15f); return h; }
extern "C" float NpLodCullPx(void) { static float c = EnvF("MSC_LOD_CULL_PX", 2.0f); return c; }

// sizes are the projected diameter of a 2-unit sphere (a figure is about 1.6-2 units tall) on the 544-line screen. thr[0]: under it level 1, thr[1]: under it level 2.
// PLAY keeps the game's lower mesh as the ceiling of detail (thr[0] huge). MSC_LOD_PX=a,b sets REPLAY and NIS (NIS = 1.08 x), MSC_LOD_PX_PLAY=b sets PLAY's level 2.
static void Thresholds(int scene, float* thr)
{
    static float rep[2] = {120.0f, 58.0f}, play2 = 58.0f;
    static int init = 0;
    if (!init)
    {
        init = 1;
        float t[4];
        const int n = NpLodParseList(getenv("MSC_LOD_PX"), t, 4);
        if (n >= 1) rep[0] = t[0];
        if (n >= 2) rep[1] = t[1];
        play2 = EnvF("MSC_LOD_PX_PLAY", play2);
    }
    if (scene == NP_LOD_SCENE_PLAY) { thr[0] = 1.0e8f; thr[1] = play2; }
    else if (scene == NP_LOD_SCENE_REPLAY) { thr[0] = rep[0]; thr[1] = rep[1]; }
    else { thr[0] = rep[0] * 1.08f; thr[1] = rep[1] * 1.08f; }
}

extern "C" int NpLodSceneOf(unsigned long st)
{
    if (st == TASK_GAMEPLAY) return NP_LOD_SCENE_PLAY;
    if (st & TASK_REPLAY) return NP_LOD_SCENE_REPLAY;
    if (st & TASK_NIS) return NP_LOD_SCENE_NIS;
    return NP_LOD_SCENE_OTHER;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------- selection state
struct LodState { const void* key; signed char level; signed char culled; unsigned frame; };
static LodState sState[96];
static unsigned sTick;

extern "C" int NpLodChoose(const void* key, int scene, float px, int* cull)
{
    *cull = 0;
    const int mode = NpLodMode();
    if (mode == 0 || scene == NP_LOD_SCENE_OTHER) return 0;
    ++sTick;
    LodState* st = 0;
    LodState* oldest = &sState[0];
    for (unsigned i = 0; i < sizeof sState / sizeof sState[0]; ++i)
    {
        if (sState[i].key == key) { st = &sState[i]; break; }
        if (sState[i].frame < oldest->frame) oldest = &sState[i];
    }
    if (!st) { st = oldest; st->key = key; st->level = 0; st->culled = 0; }
    st->frame = sTick;
    float thr[2];
    Thresholds(scene, thr);
    const float h = NpLodHyst();
    int level = NpLodSelect(px, st->level, thr, 2, h);
    if (mode == 1 && level > 1) level = 1;
    st->level = (signed char)level;
    st->culled = (signed char)NpLodCulled(px, st->culled, NpLodCullPx(), h);
    *cull = st->culled;
    return level;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------- built meshes
struct BuiltPacket { u16* idx; unsigned n; DisplayList* dl; };
struct BuiltModel
{
    unsigned long id, numPackets;
    const u16* srcIdx[24];
    unsigned srcN[24];
    BuiltPacket lv[2][24];   // [0] = L1, [1] = L2
    bool has[2];
    unsigned long trisSrc, tris[2];
};
static std::map<unsigned long, BuiltModel*> sBuilt;

static inline float Bf(const void* p) { unsigned int v; memcpy(&v, p, 4); v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); float f; memcpy(&f, &v, 4); return f; }

static bool BuildLevel(const glModel* m, BuiltModel* b, int lvl, float designPx, double* tAcc)
{
    const float bound = EnvF("MSC_LOD_PIXEL_ERR", 1.0f);
    unsigned long total = 0;
    for (unsigned long i = 0; i < m->numPackets && i < 24; ++i)
    {
        const glModelPacket& p = m->packets[i];
        BuiltPacket& out = b->lv[lvl][i];
        out.idx = 0; out.n = 0; out.dl = 0;
        const unsigned nu = p.numUniqueVertices;
        const glModelStream *sp = 0, *sn = 0, *su = 0, *sw = 0, *sb = 0;
        for (unsigned s = 0; s < p.numStreams; ++s)
        {
            const glModelStream* st = &p.streams[s];
            if (st->id == 1 && !sp) sp = st;
            else if (st->id == 2 && !sn) sn = st;
            else if (st->id == 4 && !su && st->stride == 4) su = st;
            else if (st->id == 5) sw = st;
            else if (st->id == 7) sb = st;
        }
        if (!sp || sp->stride != 12 || p.indexBuffer == 0 || p.primType > 1 || nu < 8 || p.numVertices < 24) { total += glGetNumTriangles((eGLPrimitive)(unsigned char)p.primType, p.numVertices); continue; }
        std::vector<float> pos(nu * 3), nrm, uv, w;
        std::vector<unsigned char> bones;
        const unsigned char* pa = (const unsigned char*)sp->address;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (unsigned v = 0; v < nu; ++v)
            for (int k = 0; k < 3; ++k)
            {
                const float f = (sp->unknown07 & 1) ? ((const float*)pa)[v * 3 + k] : Bf(pa + v * 12 + k * 4);
                pos[v * 3 + k] = f;
                if (f < lo[k]) lo[k] = f;
                if (f > hi[k]) hi[k] = f;
            }
        nplod::SimpIn in;
        in.nVerts = nu; in.pos = pos.data();
        if (sn && sn->stride == 12)
        {
            nrm.resize(nu * 3);
            const unsigned char* na = (const unsigned char*)sn->address;
            for (unsigned v = 0; v < nu * 3; ++v) nrm[v] = (sn->unknown07 & 1) ? ((const float*)na)[v] : Bf(na + v * 4);
            in.nrm = nrm.data();
        }
        if (su)
        {
            uv.resize(nu * 2);
            const unsigned char* ua = (const unsigned char*)su->address;
            for (unsigned v = 0; v < nu * 2; ++v)
            {
                short s16;
                if (su->unknown07 & 1) memcpy(&s16, ua + v * 2, 2); else { unsigned char t[2] = {ua[v * 2 + 1], ua[v * 2]}; memcpy(&s16, t, 2); }
                uv[v] = s16 / 256.0f;
            }
            in.uv = uv.data();
        }
        if (sw && sb)
        {
            w.resize(nu * 4);
            const unsigned char* wa = (const unsigned char*)sw->address;
            for (unsigned v = 0; v < nu * 4; ++v) w[v] = (sw->unknown07 & 1) ? ((const float*)wa)[v] : Bf(wa + v * 4);
            bones.assign((const unsigned char*)sb->address, (const unsigned char*)sb->address + nu * 4);
            in.weights = w.data(); in.bones = bones.data();
        }
        in.idx = p.indexBuffer; in.nIdx = p.numVertices; in.prim = p.primType;
        const float ext = std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
        nplod::SimpParams prm;
        // the model's height is about 2 world units per 2-unit sphere of the size rule: designPx is the size of a 2-unit sphere, so one screen line is 2/designPx units
        prm.maxError = 2.0f / designPx * bound;
        prm.skinSpan = ext * 0.5f;
        std::vector<uint16_t> list, strip;
        nplod::SimpStats stats;
        if (!nplod::Simplify(in, prm, list, &stats) || list.size() < 3) { total += glGetNumTriangles((eGLPrimitive)(unsigned char)p.primType, p.numVertices); continue; }
        nplod::Stripify(list, strip);
        if (strip.size() >= p.numVertices) { total += glGetNumTriangles((eGLPrimitive)(unsigned char)p.primType, p.numVertices); continue; }   // no gain
        void* pool = glGetCurrentResourcePool();
        out.idx = (u16*)glResourceAlloc((unsigned long)(strip.size() * 2), GLM_VertexData, pool);
        memcpy(out.idx, strip.data(), strip.size() * 2);
        out.n = (unsigned)strip.size();
        glModelPacket tmp = p;
        tmp.indexBuffer = out.idx; tmp.numVertices = out.n; tmp.primType = 1;
        out.dl = dlMakeDisplayList(&tmp, pool, true);
        if (!out.dl) { out.idx = 0; out.n = 0; total += glGetNumTriangles((eGLPrimitive)(unsigned char)p.primType, p.numVertices); continue; }
        total += glGetNumTriangles(GLP_TriStrip, out.n);
        (void)tAcc;
    }
    b->tris[lvl] = total;
    return true;
}

extern "C" void NpLodBuild(glModel* m, int kind, int hasLow)
{
    if (NpLodMode() < 2 || !m || m->numPackets == 0 || m->numPackets > 24) return;
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    BuiltModel* b = new BuiltModel();
    memset(b, 0, sizeof *b);
    b->id = m->id; b->numPackets = m->numPackets;
    unsigned long full = 0;
    for (unsigned long i = 0; i < m->numPackets; ++i)
    {
        b->srcIdx[i] = m->packets[i].indexBuffer; b->srcN[i] = m->packets[i].numVertices;
        full += glGetNumTriangles((eGLPrimitive)(unsigned char)m->packets[i].primType, m->packets[i].numVertices);
    }
    b->trisSrc = full;
    float d1 = EnvF("MSC_LOD_BUILD_PX1", 120.0f), d2 = EnvF("MSC_LOD_BUILD_PX2", 58.0f);
    double dummy = 0;
    if (kind == 0 && hasLow) { delete b; return; }   // the game's lower mesh stands in for L1 and gets its own L2
    if (kind == 0) { BuildLevel(m, b, 0, d1, &dummy); b->has[0] = true; }
    BuildLevel(m, b, 1, d2, &dummy); b->has[1] = true;
    std::map<unsigned long, BuiltModel*>::iterator it = sBuilt.find(m->id);
    if (it != sBuilt.end()) { delete it->second; sBuilt.erase(it); }   // a reload: the pool memory of the old one is gone with its pool
    sBuilt[m->id] = b;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (getenv("MSC_LOD_LOG"))
        fprintf(stderr, "[lod] built model %08lx kind %d: %lu tris, L1 %lu, L2 %lu, %.1f ms\n", static_cast<unsigned long>(m->id), kind, full, b->has[0] ? b->tris[0] : 0UL, b->tris[1],
                (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
}

extern "C" int NpLodApply(glModel* dup, const glModel* src, int level)
{
    if (level < 1) return 0;
    std::map<unsigned long, BuiltModel*>::iterator it = sBuilt.find(src->id);
    if (it == sBuilt.end()) return 0;
    BuiltModel* b = it->second;
    const int lv = level - 1;
    if (!b->has[lv] || b->numPackets != src->numPackets || dup->numPackets != src->numPackets) return 0;
    int swapped = 0;
    for (unsigned long i = 0; i < src->numPackets && i < 24; ++i)
    {
        if (b->srcIdx[i] != src->packets[i].indexBuffer || b->srcN[i] != src->packets[i].numVertices) return swapped;   // stale record
        const BuiltPacket& bp = b->lv[lv][i];
        if (!bp.idx) continue;
        glModelPacket& d = dup->packets[i];
        d.indexBuffer = bp.idx; d.numVertices = bp.n; d.primType = 1; d.displayList = bp.dl;
        ++swapped;
    }
    return swapped;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------- statistics
static unsigned long sDraws[NP_LOD_SCENES][3], sTris[NP_LOD_SCENES], sFull[NP_LOD_SCENES], sCalls;

static unsigned long TrisOf(const glModel* m)
{
    unsigned long t = 0;
    for (unsigned long i = 0; i < m->numPackets; ++i) t += glGetNumTriangles((eGLPrimitive)(unsigned char)m->packets[i].primType, m->packets[i].numVertices);
    return t;
}

extern "C" void NpLodNote(int scene, int level, const glModel* drawn, const glModel* full)
{
    static int on = -1;
    if (on < 0) on = getenv("MSC_LOD_LOG") ? 1 : 0;
    if (!on) return;
    ++sDraws[scene][level];
    sTris[scene] += TrisOf(drawn);
    sFull[scene] += TrisOf(full);
    if (++sCalls % 1500 == 0)
    {
        static const char* nm[NP_LOD_SCENES] = {"play", "replay", "nis", "other"};
        for (int s = 0; s < NP_LOD_SCENES; ++s)
            if (sDraws[s][0] + sDraws[s][1] + sDraws[s][2])
                fprintf(stderr, "[lod] %s: draws L0 %lu L1 %lu L2 %lu, character triangles %lu of %lu with every full mesh\n", nm[s], sDraws[s][0], sDraws[s][1], sDraws[s][2], sTris[s], sFull[s]);
    }
}
#endif
