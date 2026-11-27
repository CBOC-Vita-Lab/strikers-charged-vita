// cpu24 (wave 24): exact game-thread CPU cuts, each behind its own switch with a verify mode.
//   MSC_CPU24_PART=0|1|2   ParticleSystem.cpp EvaluateParticleColour: the keyframe index by a float truncation instead of a double floor() call, the stored timeFraction instead
//                          of a second divide timeElapsed / lifeSpan, no multiply and clamp when the three colour scales are 1.0f
//   MSC_CPU24_TIGHTSTAT=0|1 NpFxTightParticle: the diagnostic sums (sTs.quad / sTs.tight, px2 / (640*448)) are only made while MSC_FX_TIGHT_STAT is on (nothing else reads them)
//   MSC_CPU24_NOTIFY=0|1   the mesh writers' End() (GLTexturedColourMeshWriter and the others of the same shape): the three or four vertex streams of one mesh, allocated one after the other, are
//                          reported to the renderer as ONE written range (DCStoreRangeNoSync), not one call each: one barrier-and-atomics round of np_gx_notify_write instead of three
// 0 = the original code, 1 = the new way only, 2 = both ways, the original's answer is used, a difference is counted ("[cpu24] <name> checked N mismatches M").
// Every switch changes speed only: same results, same order of effects. Default 0 in code (the console plan turns a proven one on).
// MSC_CPU24_BREAK=<n> (host proof mutant, never set in play): 1 = PART returns a colour with the red channel off by one for every 64th particle,
// 5 = NOTIFY reports nothing for a merged mesh, 4 = NOTIFY reports only the first stream of a mesh (the renderer's caches may then serve a stale decode: frame hashes should change). TIGHTSTAT has no output to break (its proof is the frame hashes and the det trace).
#ifndef NP_CPU24_H
#define NP_CPU24_H

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif

enum { NP_C24_PART = 0, NP_C24_TIGHTSTAT = 1, NP_C24_NOTIFY = 2, NP_C24_COUNT = 3 };

extern int NpCpu24Modes[NP_C24_COUNT];   /* -1 until first asked; 3 = mode 1 with the proof mutant (MSC_CPU24_BREAK) */
int NpCpu24Init(int which);
static inline int NpCpu24Mode(int which)
{
    const int v = NpCpu24Modes[which];
    return v >= 0 ? v : NpCpu24Init(which);
}

/* counters (plain, approximate beside the pose lanes): what 0 = calls served the new way, 1 = checked by mode 2, 2 = a difference found by mode 2 */
void NpCpu24Note(int which, int what);
void NpCpu24Frame(void);   /* once a picture: a line every 600 pictures with a verify mode on */
extern int NpCpu24BreakV;   /* MSC_CPU24_BREAK; -1 until read */
int NpCpu24BreakInit(void);
static inline int NpCpu24Break(void) { return NpCpu24BreakV >= 0 ? NpCpu24BreakV : NpCpu24BreakInit(); }

#ifdef __cplusplus
}

extern "C" void DCStoreRangeNoSync(const void* address, unsigned long length);
// The vertex streams of one mesh writer, written count vertices deep. MSC_CPU24_NOTIFY=1: the streams lie within a few cache lines of each other (the writers' Begin() allocates
// them one after the other from the frame arena), so one notification over the span [lowest start, highest end) covers all of them; it also covers the alignment gaps between
// them, memory the writer did not write, which at worst makes the renderer look at a cached decode one time more than needed (a notification only ever invalidates). A stream
// that is empty, or spans that are not close (gap more than 64 bytes a stream: a resource allocation, a model with its streams elsewhere) keep the original call per stream.
template <class S>
static inline void NpCpu24StoreStreams(const S* streams, int numStreams, unsigned long count)
{
    if (NpCpu24Mode(NP_C24_NOTIFY) == 1 && numStreams >= 2 && numStreams <= 8 && count != 0)
    {
        unsigned long lo = ~0ul, hi = 0, total = 0;
        bool ok = true;
        for (int i = 0; i < numStreams; ++i)
        {
            const unsigned long a = (unsigned long)streams[i].address, len = count * (unsigned long)streams[i].stride;
            if (a == 0 || len == 0)
            {
                ok = false;
                break;
            }
            if (a < lo)
                lo = a;
            if (a + len > hi)
                hi = a + len;
            total += len;
        }
        if (ok && hi - lo <= total + 64ul * (unsigned long)numStreams)
        {
            if (NpCpu24Break() == 5)
                return;   // proof mutant: the streams are not reported at all
            if (NpCpu24Break() == 4)
            {
                DCStoreRangeNoSync(streams[0].address, count * (unsigned long)streams[0].stride);   // proof mutant: the other streams are not reported
                return;
            }
            DCStoreRangeNoSync((const void*)lo, hi - lo);
            return;
        }
    }
    for (int i = 0; i < numStreams; ++i)
        DCStoreRangeNoSync(streams[i].address, count * streams[i].stride);
}
#endif
#endif /* NP_NATIVE */
#endif
