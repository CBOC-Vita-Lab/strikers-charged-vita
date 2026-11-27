// MSC_STADIUM_CUT (np/stadiumcut.h): the per-stadium background cut mask and its action counters. Reads the environment once; nothing here touches the simulation.
#include "np/stadiumcut.h"

#ifdef NP_NATIVE
#include <stdio.h>

static int sMask;
static int sStadium = -1;
static unsigned long long sCount[8];
static unsigned long long sFrames;
static float sFar = -1.0f;

extern "C" int NpStadiumCutMask(void)
{
    return sMask;
}

extern "C" float NpStadiumCutFarPx(void)
{
    if (sFar < 0.0f) sFar = NpStadiumCutFarParse(getenv("MSC_STADIUM_CUT_FAR"));
    return sFar;
}

extern "C" void NpStadiumCutCount(int bit, int n)
{
    for (int i = 0; i < 8; ++i)
        if (bit == (1 << i)) { sCount[i] += (unsigned long long)n; return; }
}

static void Report(const char* why)
{
    if (!sMask && !sCount[0] && !sCount[1] && !sCount[2] && !sCount[3] && !sCount[4]) return;
    fprintf(stderr, "[stadiumcut] %s stadium %d mask 0x%x, frames %llu, actions: warble-particles dropped %llu, haze templates thinned %llu, crowd refreshes at the floor %llu, "
            "far drawables dropped %llu (of the floor %.0f px), world NPCs not drawn %llu, ambient effect systems dropped %llu, Dump rain streaks not built %llu, test %llu%s\n",
            why, sStadium, (unsigned)sMask, sFrames, sCount[0], sCount[1], sCount[2], sCount[3], NpStadiumCutFarPx(), sCount[4], sCount[5], sCount[6], sCount[7],
            sMask && !(sCount[0] | sCount[1] | sCount[2] | sCount[3] | sCount[4] | sCount[5] | sCount[6] | sCount[7]) ? " (DID NOTHING)" : "");
}

extern "C" void NpStadiumCutFrame(void)
{
    ++sFrames;
    if (sMask && sFrames % 900 == 0) Report("window");
}

extern "C" void NpStadiumCutLogTemplate(unsigned hash, int blend, unsigned tex, float size, float life, float fountain, int layer, int nParticles)
{
    static const bool on = getenv("MSC_STADIUM_CUT_LOG") && getenv("MSC_STADIUM_CUT_LOG")[0] == '1';
    if (!on) return;
    static unsigned seen[256];
    static int nSeen;
    for (int i = 0; i < nSeen; ++i)
        if (seen[i] == hash) return;
    if (nSeen < 256) seen[nSeen++] = hash;
    fprintf(stderr, "[stadiumcut-fx] stadium %d template 0x%08x blend %d (0 normal, 1 additive) texture %u size %.1f life %.1f fountain %.1f layer %d particles %d\n", sStadium, hash, blend, tex, size, life, fountain, layer, nParticles);
}

// The template hashes bit 32 drops: MSC_STADIUM_CUT_FXHASH (comma list of 0x..) or, unset, the table below (per stadium).
static bool FxList(unsigned hash)
{
    static unsigned list[32];
    static int n = -1;
    if (n < 0)
    {
        n = 0;
        const char* e = getenv("MSC_STADIUM_CUT_FXHASH");
        for (; e && *e && n < 32;)
        {
            while (*e == ',' || *e == ' ') ++e;
            if (!*e) break;
            char* end;
            list[n++] = (unsigned)strtoul(e, &end, 0);
            if (end == e) break;
            e = end;
        }
    }
    for (int i = 0; i < n; ++i)
        if (list[i] == hash) return true;
    return false;
}

extern "C" int NpStadiumCutFxHashed(unsigned hash)
{
    if (!(sMask & NP_CUT_AMBIENTFX) || !FxList(hash)) return 0;
    ++sCount[5];
    return 1;
}

extern "C" unsigned long long NpStadiumCutFrameNo(void) { return sFrames; }

extern "C" int NpStadiumCutNewMax(unsigned hash, int n)
{
    static unsigned h[512];
    static int mx[512], cnt;
    for (int i = 0; i < cnt; ++i)
        if (h[i] == hash) { if (n >= 2 * mx[i] || (mx[i] == 0 && n > 0)) { mx[i] = n; return 1; } return 0; }
    if (cnt < 512) { h[cnt] = hash; mx[cnt] = n; ++cnt; return 1; }
    return 0;
}

extern "C" int NpStadiumCutLogObjects(void)
{
    static const int on = getenv("MSC_STADIUM_CUT_LOG") && getenv("MSC_STADIUM_CUT_LOG")[0] == '2';
    return on;
}

// One line the first time an object is seen at a given size class (the size changes with the camera: log again when it grows by a class).
extern "C" void NpStadiumCutLogObject(unsigned hash, int type, int animated, int flags, float px)
{
    struct E { unsigned hash; int cls; };
    static E seen[1024];
    static int nSeen;
    const int cls = px < 0.0f ? -1 : (px < 8.0f ? 0 : px < 24.0f ? 1 : px < 64.0f ? 2 : px < 160.0f ? 3 : 4);
    for (int i = 0; i < nSeen; ++i)
        if (seen[i].hash == hash) { if (seen[i].cls >= cls) return; seen[i].cls = cls; goto print; }
    if (nSeen < 1024) { seen[nSeen].hash = hash; seen[nSeen].cls = cls; ++nSeen; }
print:
    fprintf(stderr, "[stadiumcut-obj] stadium %d object 0x%08x type %d animated %d flags %d projected %.0f px (class %d)\n", sStadium, hash, type, animated, flags, px, cls);
}

extern "C" void NpStadiumCutSet(int stadium)
{
    if (stadium < 0)
    {
        Report("end");
        sMask = 0;
        sStadium = -1;
        return;
    }
    memset(sCount, 0, sizeof sCount);
    sFrames = 0;
    sStadium = stadium;
    sMask = NpStadiumCutParse(getenv("MSC_STADIUM_CUT"), stadium);
    if (sMask) fprintf(stderr, "[stadiumcut] stadium %d: mask 0x%x (MSC_STADIUM_CUT=%s)\n", stadium, (unsigned)sMask, getenv("MSC_STADIUM_CUT"));
}

#endif // NP_NATIVE
