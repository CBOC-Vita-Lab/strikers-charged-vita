/* np/bg30.h: 1.3 MSC_BG30, the background at 30 and the players at 60 (owner's idea, 13 Nov 2026; design and proofs: w8/bg30.md).
 *
 *   MSC_BG30=0|off   (default) nothing changes.
 *   MSC_BG30=1       PROOF (stage a): every live-play picture keeps its background (stadium, crowd, sky, far scenery: the prefix of the main scene) in a
 *                    texture and draws it straight back over itself (renderer vgx_bg30.h, kind 3). The picture must be identical to MSC_BG30=0.
 *   MSC_BG30=2       HALF RATE: live-play pictures alternate FULL (everything, the background kept) and LIGHT (the game sends no background: its layer views
 *                    are not sent and the crowd is not updated or drawn; the renderer re-projects the kept background to the new camera through the pitch
 *                    plane and draws the players, ball, items, shadows, nets, effects and HUD over it, fresh, from this picture's blended snapshot).
 * Only in TASK_GAMEPLAY under the gameplay camera with no camera transition, no Mega Strike overlay, no captain's shot, no picture-in-picture; a camera that
 * jumps (a cut: > 1 m or > 1 degree of view in one picture) or that sees above the horizon makes the picture full. Render side only: the simulation, the
 * pad and every random number the simulation draws are untouched (det trace proof in w8/bg30.md).
 * MSC_BG30_LOG=0 turns the [bg30g] census line (every 600 pictures while the mode is on) off.
 * wave 10 (bg30b), all inside MSC_BG30=2:
 *   MSC_BG30_PANPX=<px>   (default 12; 0 off) a light picture needs the camera to have moved at most that many pixels (960x544) on the pitch plane since the full picture
 *                         it re-uses (the largest move of the plane's screen corners and centre); a faster pan makes the picture full (counted: "pan").
 *   MSC_BG30_BALANCE=<bits> (default 0) the pair's two pictures are made equal in game-thread cost: bit 1: the crowd sprites' pose update runs in the LIGHT picture (once a pair,
 *                         twice the sprites at the game's own 30 rate) and the full picture only draws them; bit 2: the stadium's animation update likewise (the full picture's time is
 *                         owed to the light picture's update). A full picture with nothing prepared by a light one updates as before. Render side only (own random streams).
 *   MSC_BG30_CUTS=<stadium>:<bgLevel>:<bgSamples>:<crowdLow>:<ambientOff>[,...]  the per-stadium background cut table (empty by default: no cuts until the console numbers).
 *   MSC_PAIR_RATE=1       the pacing13 controller (np_pair_rate.h) decides when pairs run (NpFpsPairWanted) and is told each picture (NpFpsPairReport).
 */
#ifndef NP_BG30_H
#define NP_BG30_H

#ifdef __cplusplus
extern "C" {
#endif

/* Kinds of a picture, as the renderer gets them (EXT_BG30) */
enum { NPBG30_NONE = 0, NPBG30_FULL = 1, NPBG30_LIGHT = 2, NPBG30_REUSE = 3 };

/* What the game knows about this picture (GameRenderTask fills it) */
typedef struct NpBg30In {
    int eligible;          /* live play, gameplay camera, no transition / Mega / captain's shot / PiP */
    int why;               /* when not eligible: the first reason (NPBG30_WHY_*) */
    float view[16];        /* the camera's view matrix (row vectors, nlMatrix4 order) */
    float fovDeg;          /* vertical field of view */
    int rate60;            /* pictures run at 60 or free-run (not a held 30: there every picture is full, a 15 Hz background would show), or the pair controller says pair */
} NpBg30In;
enum { NPBG30_WHY_STATE = 1, NPBG30_WHY_CAMERA, NPBG30_WHY_TRANSITION, NPBG30_WHY_MEGA, NPBG30_WHY_CAPTAIN, NPBG30_WHY_PIP, NPBG30_WHY_REPLAYCAM, NPBG30_WHY_SCREEN, NPBG30_WHY_COUNT };

int NpBg30ScreensStrict(void);              /* lessons17 MSC_BG30_SCREENS=1 (default 0): a state change pending, a wipe running or the game-over flag set also make a picture ordinary (NPBG30_WHY_SCREEN) */
int NpBg30Mode(void);                       /* MSC_BG30: 0, 1 (proof) or 2 (half rate) */
void NpBg30Begin(void);                     /* the top of every game render task: the picture is ordinary until decided (also for the send-side skip) */
int NpBg30Decide(const NpBg30In* in);       /* this picture's kind; also remembers it (NpBg30Kind) */
int NpBg30Kind(void);                       /* the kind decided for the picture being built */
int NpBg30Light(void);                      /* the picture being built is light: the game skips its background work */
void NpBg30Emit(void);                      /* records the picture's kind and camera for the renderer (in stream order) and arms the send-side skip */
int NpBg30SendLight(void);                  /* the picture being SENT is light (read by the send: GLView::Iterate) */
void NpBg30NoteCrowdSkip(void);
void NpBg30NoteStadiumSkip(void);           /* census: the stadium animation waited for the next full picture */
/* The frame-rate controller (np_fps.cpp): the game thread's per-picture cost of the picture just finished (learned per kind), and, while live play is held
 * at 30 although pairs could run, the saving a pair would give per picture (learned FULL - LIGHT over 2, or MSC_BG30_SAVE_MS until 30 of each were seen),
 * which the controller takes off its cost so that it can try 60 (its own failed-up rules then judge what pairs really cost). */
void NpBg30NoteCost(double perPictureMs);
int NpBg30HeldEligible(void);
double NpBg30PairSavingMs(void);             /* census: the crowd was not updated / drawn in a light picture */
void NpBg30Reset(void);                     /* (tests) */

/* wave 10 (bg30b). The balanced pair: what the crowd sprite update / the stadium update does in THIS picture. */
enum { NPBG30_SLOT_NORMAL = 0, NPBG30_SLOT_DO_PAIR = 1 /* light picture: do the whole pair's work now */, NPBG30_SLOT_SKIP = 2 /* full picture: a light one did it */ };
int NpBg30CrowdSlot(void);      /* asked where the crowd sprites update (bit 1 of MSC_BG30_BALANCE); notes what it answered */
int NpBg30StadiumSlot(void);    /* asked where the stadium animation updates (bit 2) */
void NpBg30StadiumSet(int stadium);   /* a stadium was loaded (-1: left): the cut table row for it, if any */
int NpBg30CutCrowdLow(void);          /* the cut row asks for the cheaper crowd (the performance refresh floor) */
int NpBg30CutAmbientOff(void);        /* ... for no ambient effects in the background */
int NpBg30CutBg(int* level, int* samples);   /* ... for a background target level/samples (renderer: the ladder), 0 when the stadium has no row */
int NpBg30Phase(void);                /* the picture just built: 0 ordinary, 1 full, 2 light (what NpFpsPairReport takes) */

/* Pure parts, for the host unit test (native/tools/bg30/bg30_unit.cpp) */
int NpBg30CameraJump(const float* a, const float* b, float fovA, float fovB);   /* 1: a cut between the two views */
int NpBg30SeesPlane(const float* view, float fovDeg, float aspect);              /* 1: the screen's top corners look down at the pitch plane */
/* the largest move, in pixels of a w x h picture, of the pitch plane's four screen corners and its centre from view b's picture to view a's (a: the kept camera, b: the new one);
 * a negative number when the plane is not seen */
float NpBg30PanPx(const float* a, const float* b, float fovA, float fovB, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* NP_BG30_H */
