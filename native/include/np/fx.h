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
    unsigned full, span, keep;   // keep: one sprite in this many stays (4; MSC_FX_HAZE sets its own)
};
NpFxThin NpFxThinFor(const EffectsTemplate* tmpl, bool whiteTexture);
bool NpFxThinParticle(const NpFxThin& t, const Particle* particle, ParticleReturn* ret);   // true: write the quad collapsed
// Tight sprites, the governor and the sprites too faint to reach the picture (np/fxtight.h; ParticleSystem.cpp, written by main.cpp BuildParticleQuads). Render only.
class ParticleSystem;
class GLView;
struct NpTightPoly;
struct NpFxTight {
    const NpTightPoly* polys;   // null: nothing known about this texture
    int frames;                 // of the template
    int verts;                  // a sprite's vertices in this mesh: 4 (quad list, the original form) or 3 x (corners - 2) (triangle list)
    bool classify;              // a billboard in a perspective view: sprites get a size class
    bool additive;
    bool tight, drop;           // the polygons, the faint sprites
    bool mark;                  // the mesh may be marked for the effects pass (NpFxMarkPacket)
    bool active;                // any of this in use (NpFxTightParticle has work)
    float vz[4];                // the view matrix's z row
    float k2, tex2;             // np/fxtight.h NpTightClass
    float px2;                  // (pixels of the picture at 448 lines, per world unit at z = 1) squared: the statistics and the governor
    int gov;                    // the governor's level (0 none; 256 one sprite in two; 512 one in four)
    int offview;                // MSC_FX_OFFVIEW (gx25): 0 off, 1 sprites wholly outside the view's side planes are not written, 2 counted only, 3 counted and the matrix checked
    float clip[3][4];           // world position (x, y, z, 1) to clip x, clip y, clip w of the view this system is drawn in
    const GLView* offView;      // (mode 3: the view the clip rows were made from)
    float model[16];            // (mode 3: the mesh's own matrix)
};
// MSC_FX_OFFVIEW: true when all four corners of the sprite lie beyond one and the same side plane (|x| > w or |y| > w, with a margin) of the view, so that no fragment of it can be drawn.
bool NpFxOffView(const NpFxTight& t, const ParticleReturn& ret);
// Called for every sprite of a system that has gone through NpFxTightParticle: counts (modes 2, 3) and tells whether the sprite is left out of the mesh (mode 1; collapsed sprites also).
bool NpFxOffViewDrop(const NpFxTight& t, const ParticleReturn& ret, bool collapse);
// Fewer vertices were written than the mesh began with (offview mode 1): the packet and the flush range follow.
void NpFxTrimPacket(glModelPacket* packet, int vertices);
void NpFxOffViewStat(void);   // once a picture: the periodic [fxoffview] line
void NpFxTightFor(NpFxTight* t, ParticleSystem* source, GLView* view);
// The polygon of a sprite (null: the quad), after NpFxThinParticle. *collapse: nothing of it can show. Also the governor's alpha and the census.
const NpTightPoly* NpFxTightParticle(const NpFxTight& t, const Particle* particle, ParticleReturn* ret, bool* collapse);
extern "C" {
#endif
void NpFxMarkPacket(glModelPacket* packet);
// The scene class of the frame being drawn (W12 OVERDRAW): 0 other, 1 match play, 2 replay, 3 cut-scene (NIS). Reported to the renderer once per
// frame (an extension record), which uses it for its census and for MSC_FX_LOWRES_SCENES.
void NpFxSetScene(int cls);
/* 1.3 MSC_TEX_PRETOUCH (NL/glx/glxTexture.cpp): once per rendered frame; inFrontEnd 1 = the front end is up (the pins of the match just over are let go). */
void NpTexPretouchStep(int scene, unsigned state);   // 0 front end, 1 loading scene, 2 live play, 3 held scene
int NpTexPretouchHold(void);        // nonzero while the loading scene should wait for the pre-upload (3 s at most)
int NpFxPacketMarked(const glModelPacket* packet);
// Draw classes (W12 FG): RLView::BeginRender reports the class of the layer view whose packets the send is about to draw (NpFxSetLayer: an
// extension record), EndRender returns to NPFX_UNKNOWN. The renderer uses it to tell the foreground (players, goalies, ball, items, HUD) from
// the background (stadium, crowd, sky) for MSC_DYNRES_FG. Anything not listed is unknown, and unknown counts as foreground.
enum {
    NPFX_UNKNOWN = 0,   // not classified (render-to-texture passes, shadow maps, movies, ...): foreground
    NPFX_PLAYER = 1,    // eCLV_Characters, eCLV_MoreCharacters, eCLV_PeachPhoto3D: skaters, goalies, the ball's carrier
    NPFX_ITEM = 2,      // eCLV_Unshadowed: item quads, ball and character shadows, indicators, sprites in world space
    NPFX_FX = 3,        // particle and lingering-particle layers, ball charge
    NPFX_HUD = 4,       // the ortho layers
    NPFX_NPC = 5,       // eCLV_WorldShadowed: decorative NPCs and the cut-scene (NIS) characters
    NPFX_BG_FIRST = 8,  // from here on: background
    NPFX_STADIUM = 8,   // eCLV_Shadowed: the stadium's opaque world objects (pitch, boards, stands, structure)
    NPFX_STADIUM_A = 9, // eCLV_WorldAlphaBlended: the stadium's blended world objects
    NPFX_CROWD = 10,    // eCLV_ImpostorOut: the crowd impostor sprites
    NPFX_SKY = 11,      // eCLV_NoFog: sky domes and other no-fog world objects
    NPFX_FAR = 12,      // eCLV_HighRange3D, eCLV_HighRange3DNoFog: far scenery (and the Mega Strike characters, see the renderer)
    NPFX_CROWD_CAP = 13 // eCLV_ImpostorTexture: the crowd impostor capture passes (offscreen)
};
void NpFxSetLayer(int cls, int layer);
#ifdef __cplusplus
}
#endif

#endif
