// np/fx.h: additive effect sprites drawn at a lower resolution (Vita port, 23 Oct). ParticleSystem marks the packet of an additive sprite
// mesh (NpFxMarkPacket: the mesh's texture binding gets a marker in its spare byte, and the mesh program is remembered); glxSend asks
// NpFxPacketMarked for every packet it draws and wraps the marked ones in np_gx_fx_lowres(1) / (0) (platform/gx: a renderer extension
// record). The renderer (vgx_backend_gl.cpp "FX") draws a run of such draws into a target of half the size, with the scene's depth, and
// adds it to the scene in one full-screen pass when the run ends. The game's own drawing is unchanged without the renderer's support.
#ifndef NP_FX_H
#define NP_FX_H

struct glModelPacket;

#ifdef __cplusplus
// Mega Strike particle thinning and the zero-alpha collapse (ParticleSystem.cpp; used by both sprite renderers)
class EffectsTemplate;
struct Particle;
struct ParticleReturn;
struct NpFxThin {
    bool thin, zero;
    unsigned full, span;
};
NpFxThin NpFxThinFor(const EffectsTemplate* tmpl, bool whiteTexture);
bool NpFxThinParticle(const NpFxThin& t, const Particle* particle, ParticleReturn* ret);   // true: write the quad collapsed
extern "C" {
#endif
void NpFxMarkPacket(glModelPacket* packet);
int NpFxPacketMarked(const glModelPacket* packet);
#ifdef __cplusplus
}
#endif

#endif
